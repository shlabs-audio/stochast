#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

// ============================================================================
// Turing — Gray-Scott reaction-diffusion on a 48 × 48 toroidal grid.
//
//   Two chemicals u and v evolve under:
//     ∂u/∂t = Du ∇²u − u v² + F (1 − u)
//     ∂v/∂t = Dv ∇²v + u v² − (F + k) v
//
//   The non-linear u v² term is autocatalytic — v eats u and reproduces. F is
//   the feed rate (replenishment of u); k is the kill rate (decay of v). The
//   classical Turing-pattern regime sits around F ∈ [0.02, 0.08], k ∈ [0.045,
//   0.065]. Different (F, k) produce spots, stripes, labyrinth, solitons.
//
//   Du, Dv are diffusion constants (Du > Dv is the classical Turing condition);
//   a five-point Laplacian is used. Time integration uses explicit Euler with
//   dt internally fixed at 1.0 (stable for the canonical parameters).
//
//   Outputs:
//     U      mean(u) over the grid (0..10 V)
//     V      mean(v) over the grid (0..10 V scaled by ×5 since v is small)
//     VAR    spatial variance of v — high during pattern formation, low at
//            steady state. A pedagogical "complexity" readout.
//     OSC    gate that fires when var(v) crosses a threshold downward
//            (visible signal that a pattern has stabilized).
// ============================================================================

struct Turing : Module {
    static constexpr int   kGrid    = 48;
    static constexpr int   kCells   = kGrid * kGrid;
    static constexpr float kInternalHz = 240.f;     // diffusion needs many steps

    enum ParamId {
        F_PARAM,     // feed rate
        K_PARAM,     // kill rate
        DU_PARAM,    // u diffusion
        DV_PARAM,    // v diffusion
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        F_CV_INPUT,
        K_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        U_OUTPUT,
        V_OUTPUT,
        VAR_OUTPUT,
        OSC_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, OSC_LIGHT, NUM_LIGHTS };

    std::array<std::array<float, kGrid>, kGrid> u{}, v{}, uNext{}, vNext{};
    float meanU = 1.f, meanV = 0.f, varV = 0.f, prevVarV = 0.f;
    float oscPulse = 0.f;

    float internalPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;
    std::mt19937 rng;
    uint32_t seedVal = 0x4ABCDEF1u;

    Turing() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(F_PARAM, 0.005f, 0.12f, 0.040f, "Feed rate F");
        configParam(K_PARAM, 0.030f, 0.080f, 0.060f, "Kill rate k");
        configParam(DU_PARAM, 0.05f, 0.30f, 0.16f, "Du — u diffusion");
        configParam(DV_PARAM, 0.02f, 0.20f, 0.08f, "Dv — v diffusion");
        configButton(SHUFFLE_PARAM, "Re-seed (random v perturbation)");
        configInput(CLOCK_INPUT, "Clock — diffusion sub-step rate (internal 240 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset");
        configInput(F_CV_INPUT, "F CV (±0.05 from ±5 V)");
        configInput(K_CV_INPUT, "k CV (±0.02 from ±5 V)");
        configOutput(U_OUTPUT, "Mean(u) — substrate concentration (0..10 V)");
        configOutput(V_OUTPUT, "Mean(v) — autocatalyst, scaled ×5 (0..10 V)");
        configOutput(VAR_OUTPUT, "Spatial variance of v ('complexity', 0..10 V)");
        configOutput(OSC_OUTPUT, "Pulse when var(v) drops sharply (pattern formed)");
        seedField();
    }

    void onReset() override { seedVal = 0x4ABCDEF1u; seedField(); }

    void seedField() {
        rng.seed(seedVal);
        std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                u[y][x] = 1.f;
                v[y][x] = 0.f;
            }
        // Spawn a small central patch of v with some noise
        int cy = kGrid / 2, cx = kGrid / 2;
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                int yy = cy + dy, xx = cx + dx;
                if (yy < 0 || yy >= kGrid || xx < 0 || xx >= kGrid) continue;
                u[yy][xx] = 0.5f + noise(rng);
                v[yy][xx] = 0.25f + noise(rng);
            }
        meanU = 1.f; meanV = 0.f; varV = 0.f; prevVarV = 0.f;
    }

    float currentF() {
        float f = clamp(params[F_PARAM].getValue(), 0.f, 0.2f);
        if (inputs[F_CV_INPUT].isConnected())
            f += inputs[F_CV_INPUT].getVoltage() * 0.01f;
        return clamp(f, 0.f, 0.2f);
    }
    float currentK() {
        float kk = clamp(params[K_PARAM].getValue(), 0.f, 0.1f);
        if (inputs[K_CV_INPUT].isConnected())
            kk += inputs[K_CV_INPUT].getVoltage() * 0.004f;
        return clamp(kk, 0.f, 0.1f);
    }

    inline float lap(const std::array<std::array<float, kGrid>, kGrid>& f, int x, int y) const {
        int xp = (x + 1) % kGrid, xm = (x + kGrid - 1) % kGrid;
        int yp = (y + 1) % kGrid, ym = (y + kGrid - 1) % kGrid;
        return f[y][xp] + f[y][xm] + f[yp][x] + f[ym][x] - 4.f * f[y][x];
    }

    void substep() {
        float F  = currentF();
        float kk = currentK();
        float Du = clamp(params[DU_PARAM].getValue(), 0.f, 0.5f);
        float Dv = clamp(params[DV_PARAM].getValue(), 0.f, 0.3f);

        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                float uu = u[y][x], vv = v[y][x];
                float reaction = uu * vv * vv;
                uNext[y][x] = uu + Du * lap(u, x, y) - reaction + F * (1.f - uu);
                vNext[y][x] = vv + Dv * lap(v, x, y) + reaction - (F + kk) * vv;
                uNext[y][x] = clamp(uNext[y][x], 0.f, 2.f);
                vNext[y][x] = clamp(vNext[y][x], 0.f, 2.f);
            }
        }
        for (int y = 0; y < kGrid; ++y) { u[y] = uNext[y]; v[y] = vNext[y]; }
    }

    void recomputeStats() {
        double sU = 0.0, sV = 0.0, sV2 = 0.0;
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                sU += u[y][x];
                sV += v[y][x];
                sV2 += (double)v[y][x] * v[y][x];
            }
        meanU = (float)(sU / kCells);
        meanV = (float)(sV / kCells);
        prevVarV = varV;
        double m = sV / kCells;
        varV = (float)std::max(0.0, sV2 / kCells - m * m);
        if (prevVarV > 0.f && varV < 0.7f * prevVarV && varV < 0.01f) oscPulse = 0.030f;
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            seedVal = seedVal * 1664525u + 1013904223u;
            seedField();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            seedVal = seedVal * 1664525u + 1013904223u;
            seedField();
        }
        lights[SHUFFLE_LIGHT].setBrightness(
            params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        bool tick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick = true;
        } else {
            internalPhase += args.sampleTime * kInternalHz;
            if (internalPhase >= 1.f) { internalPhase -= 1.f; tick = true; }
        }
        if (tick) {
            substep();
            // Stat-recompute every ~kGrid steps to save CPU
            static int statTimer = 0;
            if (++statTimer >= 12) { statTimer = 0; recomputeStats(); }
        }

        if (oscPulse > 0.f) oscPulse -= args.sampleTime;
        outputs[U_OUTPUT].setVoltage(clamp(meanU, 0.f, 1.f) * 10.f);
        outputs[V_OUTPUT].setVoltage(clamp(meanV * 5.f, 0.f, 1.f) * 10.f);
        outputs[VAR_OUTPUT].setVoltage(clamp(varV * 100.f, 0.f, 1.f) * 10.f);
        outputs[OSC_OUTPUT].setVoltage(oscPulse > 0.f ? 10.f : 0.f);
        lights[OSC_LIGHT].setBrightness(oscPulse > 0.f ? 1.f : 0.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seedVal", json_integer((json_int_t)seedVal));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "seedVal"))
            seedVal = (uint32_t)json_integer_value(j);
    }
};

// ============================================================================
// Visualization — colour-map v concentration
// ============================================================================

struct TuringView : LightWidget {
    Turing* module = nullptr;

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
                    "REACTION  DIFFUSION", nullptr);
            return;
        }

        float pad = 6.f, topStripH = 12.f, botStripH = 12.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float side = std::min(W, H);
        float x0 = pad + (W - side) / 2.f;
        float y0 = pad + topStripH + (H - side) / 2.f;
        float cell = side / Turing::kGrid;

        // Find dynamic range to make patterns visible early
        float lo = 1e9f, hi = -1e9f;
        for (int y = 0; y < Turing::kGrid; ++y)
            for (int x = 0; x < Turing::kGrid; ++x) {
                float v = module->v[y][x];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        float range = std::max(0.05f, hi - lo);

        for (int y = 0; y < Turing::kGrid; ++y) {
            for (int x = 0; x < Turing::kGrid; ++x) {
                float v = module->v[y][x];
                float t = clamp((v - lo) / range, 0.f, 1.f);
                // Magma-ish: dark purple → magenta → orange → yellow
                int r = (int)(255.f * std::pow(t, 0.6f));
                int g = (int)(220.f * std::pow(t, 1.6f));
                int b = (int)(180.f * (1.f - std::pow(t, 0.4f)) + 40.f * t);
                nvgBeginPath(vg);
                nvgRect(vg, x0 + x * cell, y0 + y * cell, cell + 0.5f, cell + 0.5f);
                nvgFillColor(vg, nvgRGB(r, g, b));
                nvgFill(vg);
            }
        }

        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        char buf[80];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "F=%.3f  k=%.3f",
                      module->currentF(), module->currentK());
        nvgText(vg, 4, 3, buf, nullptr);

        std::snprintf(buf, sizeof(buf), "ū=%.2f  v̄=%.2f", module->meanU, module->meanV);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

struct TuringWidget : ModuleWidget {
    TuringWidget(Turing* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Turing.svg")));
        addChild(new ModuleTitle("TURING", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "F");  labels->k1(1, "k");
        labels->k1(2, "Du"); labels->k1(3, "Dv");
        labels->k2(0, "SHUF");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "F·CV");  labels->in(3, "k·CV");
        labels->outSection();
        labels->out(0, "ū");  labels->out(1, "v̄");
        labels->out(2, "VAR"); labels->out(3, "OSC");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new TuringView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Turing::F_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Turing::K_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Turing::DU_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Turing::DV_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(45, 294), module, Turing::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(45, 280), module, Turing::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Turing::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Turing::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Turing::F_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Turing::K_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Turing::U_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Turing::V_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Turing::VAR_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Turing::OSC_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(270, 344), module, Turing::OSC_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Turing",
            {"Turing reaction-diffusion (Gray-Scott style) on a 2D grid.",
             "Generates emergent patterns — spots, stripes, labyrinths —",
             "from local chemical-kinetics rules."},
            "Tape (record pattern's growth as CV), Seed (reproducible)");
    }
};

Model* modelTuring = createModel<Turing, TuringWidget>("Turing");
