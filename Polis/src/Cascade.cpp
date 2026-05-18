#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

// ============================================================================
// Cascade — Granovetter threshold model of collective behavior.
//
//   N agents (1..16), each with a personal activation threshold t_i ∈ [0,1],
//   drawn from a Normal(MEAN, SPREAD) distribution and sorted ascending so the
//   visualization reads left-to-right as "early adopters" → "die-hards".
//
//   On each tick:
//     f         = fraction currently active
//     effective = clamp(f + PRESSURE + PRESSURE_CV, 0, 1)
//     agent i activates if effective ≥ t_i
//     iterate to fixed point so a cascade fully propagates within the tick
//
//   The "level rises like water" visual: PRESSURE fills the tank from the
//   bottom, and any agent whose threshold dot is submerged ignites.
// ============================================================================

struct Cascade : Module {
    enum ParamId {
        POPULATION_PARAM,
        MEAN_PARAM,
        SPREAD_PARAM,
        PRESSURE_PARAM,
        INFLUENCE_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        PRESSURE_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        ACTIVE_OUTPUT,
        DELTA_OUTPUT,
        IGNITION_OUTPUT,
        POLY_GATES_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, IGNITION_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxN = 16;
    static constexpr float kInternalClockHz = 30.f;
    static constexpr float kIgnitionPulseSec = 0.025f;

    enum CascadeMode { MODE_REVERSIBLE = 0, MODE_HYSTERETIC = 1 };
    int mode = MODE_REVERSIBLE;

    int N = 16;
    std::array<float, kMaxN> thresholds{};
    std::array<bool,  kMaxN> active{};
    float activeFrac = 0.f;
    float prevActiveFrac = 0.f;
    float ignitionPulse = 0.f;
    float internalClockPhase = 0.f;

    // Smoothed CV outputs (so they're continuous between ticks)
    float smoothActive = 0.f;
    float smoothDelta  = 0.f;

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger shuffleBtn;

    std::mt19937 rng{0xC45CADEu};

    Cascade() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 1.f, (float)kMaxN, 16.f, "Population");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(MEAN_PARAM,     0.f, 1.f, 0.40f, "Threshold mean");
        configParam(SPREAD_PARAM,   0.f, 1.f, 0.35f, "Threshold spread");
        configParam(PRESSURE_PARAM, 0.f, 1.f, 0.30f, "External pressure");
        configParam(INFLUENCE_PARAM, 0.f, 1.5f, 0.55f, "Social influence (peer feedback weight α)");
        configButton(SHUFFLE_PARAM, "Re-draw thresholds (resets activations)");
        configInput(CLOCK_INPUT,        "Clock (free-runs at 8 Hz if unpatched)");
        configInput(RESET_INPUT,        "Reset");
        configInput(PRESSURE_CV_INPUT,  "Pressure CV (0..10 V adds to knob)");
        configOutput(ACTIVE_OUTPUT,     "Active fraction (0..10 V)");
        configOutput(DELTA_OUTPUT,      "Δ active per tick (±5 V)");
        configOutput(IGNITION_OUTPUT,   "Ignition gate");
        configOutput(POLY_GATES_OUTPUT, "Per-agent gates (poly)");
        drawThresholds();
        resetAgents();
    }

    void drawThresholds() {
        float mean   = clamp(params[MEAN_PARAM].getValue(),   0.f, 1.f);
        float spread = clamp(params[SPREAD_PARAM].getValue(), 0.f, 1.f);
        float sigma  = std::max(0.001f, spread * 0.4f);
        std::normal_distribution<float> nd(mean, sigma);
        for (int i = 0; i < kMaxN; ++i) {
            thresholds[i] = clamp(nd(rng), 0.f, 1.f);
        }
        std::sort(thresholds.begin(), thresholds.end());
    }

    void resetAgents() {
        for (int i = 0; i < kMaxN; ++i) active[i] = false;
        activeFrac = prevActiveFrac = 0.f;
        ignitionPulse = 0.f;
    }

    void onReset() override {
        resetAgents();
        drawThresholds();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "mode", json_integer(mode));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* m = json_object_get(root, "mode")) {
            mode = (int)json_integer_value(m);
        }
    }

    int countActive() const {
        int c = 0;
        for (int i = 0; i < N; ++i) if (active[i]) ++c;
        return c;
    }

    float currentPressure() {
        float p = clamp(params[PRESSURE_PARAM].getValue(), 0.f, 1.f);
        if (inputs[PRESSURE_CV_INPUT].isConnected()) {
            p += clamp(inputs[PRESSURE_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        }
        return clamp(p, 0.f, 1.f);
    }

    float currentInfluence() {
        return clamp(params[INFLUENCE_PARAM].getValue(), 0.f, 1.5f);
    }

    float effectiveLevel(float pressure, float influence) {
        return clamp(influence * activeFrac + pressure, 0.f, 1.f);
    }

    void tick() {
        prevActiveFrac = activeFrac;

        // Snapshot prior state so we can count *new* activations this tick
        std::array<bool, kMaxN> prev = active;

        // In reversible mode, recompute the active set from scratch every tick:
        // pressure becomes a bidirectional control. In hysteretic mode (true
        // Granovetter), keep prior activations — agents can only flip on.
        if (mode == MODE_REVERSIBLE) {
            for (int i = 0; i < N; ++i) active[i] = false;
        }

        float pressure  = currentPressure();
        float influence = currentInfluence();
        bool changed = true;
        int safety = N + 1;
        while (changed && safety-- > 0) {
            changed = false;
            float f = (float)countActive() / std::max(1, N);
            float effective = clamp(influence * f + pressure, 0.f, 1.f);
            for (int i = 0; i < N; ++i) {
                if (!active[i] && effective >= thresholds[i]) {
                    active[i] = true;
                    changed = true;
                }
            }
        }

        int newlyActivated = 0;
        for (int i = 0; i < N; ++i) if (active[i] && !prev[i]) ++newlyActivated;

        activeFrac = (float)countActive() / std::max(1, N);

        // Ignition: a meaningful jump (≥ max(2, N/8) new agents in one tick)
        if (newlyActivated >= std::max(2, N / 8)) {
            ignitionPulse = kIgnitionPulseSec;
        }
    }

    void process(const ProcessArgs& args) override {
        int newN = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 1, kMaxN);
        if (newN != N) {
            for (int i = newN; i < kMaxN; ++i) active[i] = false;
            N = newN;
        }

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            resetAgents();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            drawThresholds();
            resetAgents();
        }
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

        // 1-pole smoothing (~30 Hz) for continuous CV
        float a = 1.f - std::exp(-2.f * (float)M_PI * 30.f / args.sampleRate);
        smoothActive += a * (activeFrac - smoothActive);
        smoothDelta  += a * ((activeFrac - prevActiveFrac) - smoothDelta);

        outputs[ACTIVE_OUTPUT].setVoltage(clamp(smoothActive * 10.f, 0.f, 10.f));
        outputs[DELTA_OUTPUT].setVoltage(clamp(smoothDelta * 50.f, -5.f, 5.f));

        if (ignitionPulse > 0.f) ignitionPulse -= args.sampleTime;
        bool igniting = ignitionPulse > 0.f;
        outputs[IGNITION_OUTPUT].setVoltage(igniting ? 10.f : 0.f);
        lights[IGNITION_LIGHT].setBrightness(igniting ? 1.f : 0.f);

        outputs[POLY_GATES_OUTPUT].setChannels(N);
        for (int i = 0; i < N; ++i) {
            outputs[POLY_GATES_OUTPUT].setVoltage(active[i] ? 10.f : 0.f, i);
        }
    }
};

// ============================================================================
// Visualization — vertical bars for each agent's threshold; a rising "water
// level" representing effective pressure submerges agents that activate.
// ============================================================================

struct CascadeView : LightWidget {
    Cascade* module = nullptr;

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
                    "GRANOVETTER THRESHOLDS", nullptr);
            return;
        }

        const int N = module->N;
        const auto& thr = module->thresholds;
        const auto& act = module->active;

        float pad = 6.f;
        float fracBarH = 6.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - fracBarH - 4.f;
        float x0 = pad, y0 = pad;

        // Faint mid-line at threshold = 0.5
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, y0 + H * 0.5f);
        nvgLineTo(vg, x0 + W, y0 + H * 0.5f);
        nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 100));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Two water levels:
        //   pressure alone — external force alone, no social feedback
        //   α·f + pressure — actual effective level (peer amplification)
        // The gap between the dashed and solid lines is the social-amplification effect.
        float pressure  = module->currentPressure();
        float influence = module->currentInfluence();
        float effective = module->effectiveLevel(pressure, influence);
        float pressY    = y0 + H * (1.f - pressure);
        float effectY   = y0 + H * (1.f - effective);

        // Effective "water" filled from bottom
        nvgBeginPath(vg);
        nvgRect(vg, x0, effectY, W, (y0 + H) - effectY);
        nvgFillColor(vg, nvgRGBA(40, 95, 145, 70));
        nvgFill(vg);

        // Pressure-alone line (dashed, dimmer) — shows social amplification gap
        if (pressure > 0.001f) {
            nvgBeginPath(vg);
            float dash = 4.f;
            for (float xx = x0; xx < x0 + W; xx += dash * 2.f) {
                nvgMoveTo(vg, xx, pressY);
                nvgLineTo(vg, std::min(xx + dash, x0 + W), pressY);
            }
            nvgStrokeColor(vg, nvgRGBA(100, 130, 170, 140));
            nvgStrokeWidth(vg, 0.8f);
            nvgStroke(vg);
        }

        // Effective level line (solid, bright)
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, effectY);
        nvgLineTo(vg, x0 + W, effectY);
        nvgStrokeColor(vg, nvgRGB(80, 165, 220));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        // Agent bars + threshold dots
        if (N > 0) {
            float slot = W / N;
            float barW = std::max(2.f, slot * 0.45f);
            for (int i = 0; i < N; ++i) {
                float cx = x0 + slot * (i + 0.5f);
                float thY = y0 + H * (1.f - thr[i]);
                bool a = act[i];

                // Stem from threshold down to baseline
                nvgBeginPath(vg);
                nvgRect(vg, cx - barW * 0.5f, thY, barW, (y0 + H) - thY);
                nvgFillColor(vg, a ? nvgRGBA(220, 110, 60, 200)
                                   : nvgRGBA(80, 90, 110, 110));
                nvgFill(vg);

                // Dot at threshold
                nvgBeginPath(vg);
                nvgCircle(vg, cx, thY, a ? 2.6f : 1.9f);
                nvgFillColor(vg, a ? nvgRGB(245, 160, 90) : nvgRGB(110, 120, 140));
                nvgFill(vg);
            }
        }

        // Bottom bar: active fraction (independent of pressure)
        float fbY = box.size.y - pad - fracBarH;
        nvgBeginPath(vg);
        nvgRect(vg, x0, fbY, W, fracBarH);
        nvgFillColor(vg, nvgRGBA(50, 56, 78, 200));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, x0, fbY, W * module->activeFrac, fracBarH);
        nvgFillColor(vg, nvgRGB(220, 110, 60));
        nvgFill(vg);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Header label
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, "THRESHOLDS  ·  PRESSURE", nullptr);

        // Active fraction readout
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d / %d", module->countActive(), N);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct CascadeWidget : ModuleWidget {
    CascadeWidget(Cascade* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Cascade.svg")));
        addChild(new ModuleTitle("CASCADE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP");    labels->k1(1, "MEAN");
        labels->k1(2, "SPREAD"); labels->k1(3, "PRESS");
        labels->k2(1, "INFLUENCE"); labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "P·CV");
        labels->outSection();
        labels->out(0, "ACTIVE"); labels->out(1, "DELTA");
        labels->out(2, "IGNITE"); labels->out(3, "GATES");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new CascadeView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Cascade::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Cascade::MEAN_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Cascade::SPREAD_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Cascade::PRESSURE_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(120, 294), module, Cascade::INFLUENCE_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Cascade::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Cascade::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Cascade::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Cascade::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Cascade::PRESSURE_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Cascade::ACTIVE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Cascade::DELTA_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Cascade::IGNITION_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(222, 358), module, Cascade::IGNITION_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Cascade::POLY_GATES_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Cascade*>(this->module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem(
            "Cascade mode",
            {"Reversible (musical)", "Hysteretic (Granovetter)"},
            &m->mode));

        appendAboutMenu(menu, "Cascade",
            {"Granovetter-style threshold model of collective action.",
             "Agents each have a personal threshold; an agent activates",
             "when the share already active exceeds their threshold."},
            "Network (supply structure), Diffusion (SIR-style variant)");
    }
};

Model* modelCascade = createModel<Cascade, CascadeWidget>("Cascade");
