#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

// ============================================================================
// Diffusion — Bass innovation-adoption dynamics.
//
//   N agents are either adopters or not. Each tick, every non-adopter has
//   probability λ_i = p + q · f to flip on, where
//       p = innovation rate (spontaneous adoption)
//       q = imitation rate (peer-driven adoption)
//       f = current fraction adopted
//   Adoption is irreversible (the canonical Bass model assumption).
//
//   Produces the classic S-curve: slow early uptake from innovators, an
//   accelerating middle as imitators kick in, then plateau as the non-adopter
//   pool thins. Analytical inflection at f* = max(0, (q − p) / (2q)).
//
//   SPEED runs multiple sub-steps per clock tick to control simulation pace.
//   Q_CV adds 0..10 V → 0..0.5 to the imitation rate, so an LFO into Q_CV
//   makes peer pressure breathe — useful for triggering re-cascades after
//   reset.
// ============================================================================

struct Diffusion : Module {
    enum ParamId {
        POPULATION_PARAM,
        P_PARAM,
        Q_PARAM,
        SPEED_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        Q_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        GATES_OUTPUT,
        ADOPTED_OUTPUT,
        DELTA_OUTPUT,
        SATURATE_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, SATURATE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxN              = 16;
    static constexpr int   kHistLen           = 256;
    static constexpr float kInternalClockHz   = 20.f;
    static constexpr float kSaturateFraction  = 0.95f;
    static constexpr float kSaturatePulseSec  = 0.05f;

    int N = 16;
    std::array<bool, kMaxN> adopted{};
    int countAdopted = 0;

    std::array<float, kHistLen> historyFrac{};
    int histIdx = 0;
    int histFrameCounter = 0;

    float smoothDelta = 0.f;
    float saturatePulse = 0.f;
    bool wasSaturated = false;
    float internalClockPhase = 0.f;

    std::mt19937 rng{0xD1FF0u};

    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    // Double-buffered adjacency receive (push-receive convention). Diffusion
    // also re-publishes the message on its own rightExpander so any further
    // graph-aware Empiria module downstream sees the same network through
    // the chain.
    EmpiriaNetworkMessage leftMsgA, leftMsgB;

    Diffusion() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        leftExpander.producerMessage = &leftMsgA;
        leftExpander.consumerMessage = &leftMsgB;
        configParam(POPULATION_PARAM, 1.f, (float)kMaxN, 16.f, "Population");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(P_PARAM, 0.f, 0.05f, 0.015f, "Innovation rate p (spontaneous adoption)");
        configParam(Q_PARAM, 0.f, 0.5f,  0.25f,  "Imitation rate q (peer-driven adoption)");
        configParam(SPEED_PARAM, 1.f, 32.f, 1.f, "Substeps per clock tick");
        paramQuantities[SPEED_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Reset to no adopters");
        configInput(CLOCK_INPUT, "Clock (free-runs at 20 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear adoptions)");
        configInput(Q_CV_INPUT, "Imitation rate CV (0..10 V → +0..0.5 added to q)");
        configOutput(GATES_OUTPUT, "Per-agent adoption gate (polyphonic)");
        configOutput(ADOPTED_OUTPUT, "Fraction adopted (0..10 V)");
        configOutput(DELTA_OUTPUT, "Adoption rate, smoothed (0..10 V)");
        configOutput(SATURATE_OUTPUT, "Saturation gate (95% adopted)");
        resetAdoptions();
    }

    void resetAdoptions() {
        for (int i = 0; i < kMaxN; ++i) adopted[i] = false;
        countAdopted = 0;
        smoothDelta = 0.f;
        saturatePulse = 0.f;
        wasSaturated = false;
        historyFrac.fill(0.f);
        histIdx = 0;
    }

    void onReset() override { resetAdoptions(); }

    float currentQ() {
        float q = clamp(params[Q_PARAM].getValue(), 0.f, 0.5f);
        if (inputs[Q_CV_INPUT].isConnected()) {
            q += clamp(inputs[Q_CV_INPUT].getVoltage() / 10.f * 0.5f, 0.f, 0.5f);
        }
        return clamp(q, 0.f, 1.f);
    }

    // Returns a pointer to the upstream Network adjacency message if a SHLabs
    // Network (or another Empiria forwarder) is currently to the left of this
    // module. Verified by the magic header so unrelated modules on the left
    // never produce a false positive.
    const EmpiriaNetworkMessage* tryGetNetworkMessage() const {
        auto* msg = static_cast<EmpiriaNetworkMessage*>(leftExpander.consumerMessage);
        if (!msg) return nullptr;
        if (msg->magic != EmpiriaNetworkMessage::kMagic) return nullptr;
        return msg;
    }

    // Forward the received adjacency to the right neighbour, so a chain like
    // Network | Diffusion | further-graph-aware-module all see the same graph.
    void forwardAdjacencyRight() {
        const auto* in = tryGetNetworkMessage();
        if (!in) return;
        // Gate on the neighbour's model before writing: only a known Empiria
        // consumer (Diffusion) allocates a full-size EmpiriaNetworkMessage
        // buffer. Writing into an arbitrary neighbour's buffer would overflow.
        if (!rightExpander.module ||
            rightExpander.module->model != modelDiffusion ||
            !rightExpander.module->leftExpander.producerMessage) return;
        auto* out = static_cast<EmpiriaNetworkMessage*>(
                        rightExpander.module->leftExpander.producerMessage);
        *out = *in;
        rightExpander.module->leftExpander.messageFlipRequested = true;
    }

    int runOneSubstep() {
        float p = clamp(params[P_PARAM].getValue(), 0.f, 0.05f);
        float q = currentQ();
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        int newAdopted = 0;

        const EmpiriaNetworkMessage* net = tryGetNetworkMessage();
        if (net && net->N >= 2) {
            // Network Bass model: for each non-adopter i, peer pressure is
            // proportional to the *local* fraction of its neighbours that have
            // adopted, not the global fraction.
            int M = std::min(N, net->N);
            for (int i = 0; i < M; ++i) {
                if (adopted[i]) continue;
                int deg = 0, adoptedNeighbours = 0;
                for (int j = 0; j < M; ++j) {
                    if (!net->adj[i][j]) continue;
                    ++deg;
                    if (adopted[j]) ++adoptedNeighbours;
                }
                float localF = (deg > 0) ? (float)adoptedNeighbours / deg : 0.f;
                float lambda = clamp(p + q * localF, 0.f, 1.f);
                if (ud(rng) < lambda) {
                    adopted[i] = true;
                    ++newAdopted;
                    ++countAdopted;
                }
            }
        } else {
            // Mass-action Bass model: same λ for everyone.
            float fAdopted = (N > 0) ? (float)countAdopted / N : 0.f;
            float lambda = clamp(p + q * fAdopted, 0.f, 1.f);
            for (int i = 0; i < N; ++i) {
                if (!adopted[i] && ud(rng) < lambda) {
                    adopted[i] = true;
                    ++newAdopted;
                    ++countAdopted;
                }
            }
        }
        return newAdopted;
    }

    void tick() {
        int substeps = clamp((int)std::round(params[SPEED_PARAM].getValue()), 1, 32);
        int totalNew = 0;
        for (int s = 0; s < substeps; ++s) totalNew += runOneSubstep();

        float instantDelta = (N > 0) ? (float)totalNew / N : 0.f;
        smoothDelta = 0.7f * smoothDelta + 0.3f * instantDelta;
    }

    void process(const ProcessArgs& args) override {
        int newN = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 1, kMaxN);
        if (newN != N) {
            for (int i = newN; i < kMaxN; ++i) adopted[i] = false;
            N = newN;
            countAdopted = 0;
            for (int i = 0; i < N; ++i) if (adopted[i]) ++countAdopted;
        }

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) resetAdoptions();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) resetAdoptions();
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick();
        } else {
            internalClockPhase += args.sampleTime * kInternalClockHz;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick();
            }
        }

        // Sample history at ~60 Hz for the trajectory plot
        int stride = std::max(1, (int)(args.sampleRate / 60.f));
        if (++histFrameCounter >= stride) {
            histFrameCounter = 0;
            float f = (N > 0) ? (float)countAdopted / N : 0.f;
            historyFrac[histIdx] = f;
            histIdx = (histIdx + 1) % kHistLen;
        }

        // Outputs
        outputs[GATES_OUTPUT].setChannels(N);
        for (int i = 0; i < N; ++i) {
            outputs[GATES_OUTPUT].setVoltage(adopted[i] ? 10.f : 0.f, i);
        }
        float f = (N > 0) ? (float)countAdopted / N : 0.f;
        outputs[ADOPTED_OUTPUT].setVoltage(clamp(f, 0.f, 1.f) * 10.f);
        outputs[DELTA_OUTPUT].setVoltage(clamp(smoothDelta * 100.f, 0.f, 10.f));

        // Saturation event: pulse on crossing the threshold
        bool nowSaturated = f >= kSaturateFraction;
        if (nowSaturated && !wasSaturated) saturatePulse = kSaturatePulseSec;
        wasSaturated = nowSaturated;
        if (saturatePulse > 0.f) saturatePulse -= args.sampleTime;
        bool gate = saturatePulse > 0.f;
        outputs[SATURATE_OUTPUT].setVoltage(gate ? 10.f : 0.f);
        lights[SATURATE_LIGHT].setBrightness(gate ? 1.f : 0.f);

        forwardAdjacencyRight();
    }
};

// ============================================================================
// Visualization — scrolling S-curve trajectory of the cumulative adoption
// fraction, with a row of per-agent dots at the bottom showing who has
// adopted (bright orange) versus who has not (dim grey).
// ============================================================================

struct DiffusionView : LightWidget {
    Diffusion* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        if (!module) {
            nvgFontSize(vg, 9.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 130));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "ADOPTION  ·  TIME", nullptr);
            return;
        }

        const int N = module->N;
        const int H = Diffusion::kHistLen;
        const int wIdx = module->histIdx;

        float pad = 6.f;
        float topStripH = 12.f;
        float agentRowH = 12.f;
        float W = box.size.x - 2 * pad;
        float Hh = box.size.y - 2 * pad - topStripH - agentRowH;
        float x0 = pad, y0 = pad + topStripH;

        // Reference horizontal lines at f = 0.5 and 1.0
        for (float v : {0.5f, 1.0f}) {
            nvgBeginPath(vg);
            float y = y0 + Hh * (1.f - v);
            nvgMoveTo(vg, x0, y);
            nvgLineTo(vg, x0 + W, y);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, v == 0.5f ? 100 : 130));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        // Inflection-point indicator: f* = (q-p)/(2q), shown as a faint dashed
        // horizontal line — the analytical Bass peak rate occurs here.
        float p = clamp(module->params[Diffusion::P_PARAM].getValue(), 0.f, 0.05f);
        float q = module->currentQ();
        if (q > 1e-3f) {
            float fStar = clamp((q - p) / (2.f * q), 0.f, 1.f);
            float ys = y0 + Hh * (1.f - fStar);
            nvgBeginPath(vg);
            float dash = 4.f;
            for (float xx = x0; xx < x0 + W; xx += dash * 2.f) {
                nvgMoveTo(vg, xx, ys);
                nvgLineTo(vg, std::min(xx + dash, x0 + W), ys);
            }
            nvgStrokeColor(vg, nvgRGBA(170, 110, 200, 130));
            nvgStrokeWidth(vg, 0.6f);
            nvgStroke(vg);
        }

        // Trajectory line
        nvgBeginPath(vg);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        for (int t = 0; t < H; ++t) {
            int idx = (wIdx + t) % H;
            float fv = clamp(module->historyFrac[idx], 0.f, 1.f);
            float x = x0 + (float)t / (H - 1) * W;
            float yp = y0 + Hh * (1.f - fv);
            if (t == 0) nvgMoveTo(vg, x, yp);
            else        nvgLineTo(vg, x, yp);
        }
        nvgStrokeColor(vg, nvgRGB(80, 165, 220));
        nvgStrokeWidth(vg, 1.4f);
        nvgStroke(vg);

        // Right-edge "now" dot
        float fCur = (N > 0) ? (float)module->countAdopted / N : 0.f;
        float yCur = y0 + Hh * (1.f - fCur);
        nvgBeginPath(vg);
        nvgCircle(vg, x0 + W, yCur, 2.6f);
        nvgFillColor(vg, nvgRGB(245, 160, 90));
        nvgFill(vg);

        // Per-agent row at the bottom — one dot per agent
        if (N > 0) {
            float dotsY = y0 + Hh + agentRowH * 0.5f;
            float slot = W / N;
            for (int i = 0; i < N; ++i) {
                float cx = x0 + slot * (i + 0.5f);
                bool a = module->adopted[i];
                nvgBeginPath(vg);
                nvgCircle(vg, cx, dotsY, a ? 2.4f : 1.6f);
                nvgFillColor(vg, a ? nvgRGB(245, 160, 90)
                                   : nvgRGBA(85, 95, 115, 140));
                nvgFill(vg);
            }
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — header
        char buf[64];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        const auto* net = module->tryGetNetworkMessage();
        if (net) {
            std::snprintf(buf, sizeof(buf), "p=%.3f  q=%.3f  NET·%d", p, q, net->N);
            nvgFillColor(vg, nvgRGB(120, 220, 140));
        } else {
            std::snprintf(buf, sizeof(buf), "p=%.3f  q=%.3f  MASS", p, q);
        }
        nvgText(vg, 4, 3, buf, nullptr);

        std::snprintf(buf, sizeof(buf), "%d/%d  (%.0f%%)",
                      module->countAdopted, N, fCur * 100.f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct DiffusionWidget : ModuleWidget {
    DiffusionWidget(Diffusion* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Diffusion.svg")));
        addChild(new ModuleTitle("DIFFUSION", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP"); labels->k1(1, "P");
        labels->k1(2, "Q");   labels->k1(3, "SPEED");
        labels->k2(3, "RESET");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "Q·CV");
        labels->outSection();
        labels->out(0, "GATES"); labels->out(1, "ADOPT");
        labels->out(2, "Δ");     labels->out(3, "DONE");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new DiffusionView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Diffusion::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Diffusion::P_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Diffusion::Q_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Diffusion::SPEED_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Diffusion::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Diffusion::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Diffusion::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Diffusion::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Diffusion::Q_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Diffusion::GATES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Diffusion::ADOPTED_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Diffusion::DELTA_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Diffusion::SATURATE_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(243, 358), module, Diffusion::SATURATE_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Diffusion",
            {"Spreads a state through a network supplied on its left",
             "expander. Adjacency comes from Network. Watch how seed",
             "structure shapes the eventual reach and speed of diffusion."},
            "Network (REQUIRED on the left), Cascade (threshold variant)");
    }
};

Model* modelDiffusion = createModel<Diffusion, DiffusionWidget>("Diffusion");
