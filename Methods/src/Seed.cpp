#include "plugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

// ============================================================================
// Seed — reproducibility primitive.
//
//   Holds a single integer seed value (0..999) and exposes it as a CV plus a
//   change-detection trigger. The seed value is saved in the patch JSON, so
//   a patch shared between machines produces an identical simulation when
//   re-loaded — provided the random modules in the patch were initialised
//   from a deterministic seed (which is the default for every Empiria module).
//
//   Knob:    VALUE       — manual seed entry, 0..999, snap
//   Button:  RANDOMIZE   — picks a uniformly random seed
//   Input:   CLOCK       — increments the seed on each rising edge
//   Input:   RESET       — sets the seed to 0
//   Output:  SEED CV     — voltage proportional to seed (0..10 V)
//   Output:  TRIG        — 2 ms pulse fires whenever the seed changes;
//                          patch into other modules' RESET inputs to keep
//                          their dynamics aligned with the seed
// ============================================================================

struct Seed : Module {
    enum ParamId {
        VALUE_PARAM,
        RANDOMIZE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        SEED_CV_OUTPUT,
        TRIG_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { RANDOMIZE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxSeed       = 999;
    static constexpr float kTrigPulseSec  = 0.002f;

    int  seedValue     = 1;
    int  prevSeedValue = -1;
    float trigPulse    = 0.f;

    std::mt19937 rng{0xC0FFEEu};
    dsp::SchmittTrigger clockTrig, resetTrig, randomizeBtn;

    Seed() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(VALUE_PARAM, 0.f, (float)kMaxSeed, 1.f, "Seed value");
        paramQuantities[VALUE_PARAM]->snapEnabled = true;
        // Right-click Randomize must NOT scramble the seed value — that
        // would defeat the reproducibility primitive that Seed exists to
        // be. The dedicated RANDOMIZE button on the panel is the only
        // intended way to advance to a new seed.
        paramQuantities[VALUE_PARAM]->randomizeEnabled = false;
        configButton(RANDOMIZE_PARAM, "Randomize seed (0..999)");
        configInput(CLOCK_INPUT, "Clock — increment seed each rising edge");
        configInput(RESET_INPUT, "Reset seed to 0");
        configOutput(SEED_CV_OUTPUT, "Seed CV — 0..10 V proportional to value");
        configOutput(TRIG_OUTPUT, "Trigger pulse — fires when seed changes");
    }

    void onReset() override {
        params[VALUE_PARAM].setValue(1.f);
        seedValue = 1;
        prevSeedValue = -1;
        trigPulse = 0.f;
        rng.seed(0xC0FFEEu);
    }

    void process(const ProcessArgs& args) override {
        // Read current value (snap-knob → int)
        int v = clamp((int)std::round(params[VALUE_PARAM].getValue()), 0, kMaxSeed);

        // Randomize button
        if (randomizeBtn.process(params[RANDOMIZE_PARAM].getValue())) {
            std::uniform_int_distribution<int> dist(0, kMaxSeed);
            v = dist(rng);
            params[VALUE_PARAM].setValue((float)v);
        }
        // Reset
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            v = 0;
            params[VALUE_PARAM].setValue(0.f);
        }
        // Clock-driven increment
        if (inputs[CLOCK_INPUT].isConnected() &&
            clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
            v = (v + 1) % (kMaxSeed + 1);
            params[VALUE_PARAM].setValue((float)v);
        }

        seedValue = v;

        // Detect change → emit trig
        if (seedValue != prevSeedValue) {
            trigPulse = kTrigPulseSec;
            prevSeedValue = seedValue;
        }

        // Outputs
        outputs[SEED_CV_OUTPUT].setVoltage((float)seedValue / (float)kMaxSeed * 10.f);
        if (trigPulse > 0.f) trigPulse -= args.sampleTime;
        outputs[TRIG_OUTPUT].setVoltage(trigPulse > 0.f ? 10.f : 0.f);

        lights[RANDOMIZE_LIGHT].setBrightness(
            params[RANDOMIZE_PARAM].getValue() > 0.5f ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — large centred seed number, with the corresponding output
// voltage shown beneath as a filled bar.
// ============================================================================

struct SeedView : LightWidget {
    Seed* module = nullptr;

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
                    "SEED VALUE", nullptr);
            return;
        }

        int v = module->seedValue;

        // Label on top
        nvgFontSize(vg, 8.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x / 2, 10, "SEED VALUE", nullptr);

        // Big centred number
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d", v);
        nvgFontSize(vg, 64.f);
        nvgFillColor(vg, nvgRGB(245, 200, 90));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, box.size.x / 2, box.size.y / 2 - 6.f, buf, nullptr);

        // CV bar near the bottom
        float barX = 14.f, barW = box.size.x - 28.f;
        float barY = box.size.y - 28.f, barH = 6.f;
        nvgBeginPath(vg);
        nvgRect(vg, barX, barY, barW, barH);
        nvgFillColor(vg, nvgRGBA(50, 56, 78, 200));
        nvgFill(vg);
        float fill = barW * ((float)v / (float)Seed::kMaxSeed);
        nvgBeginPath(vg);
        nvgRect(vg, barX, barY, fill, barH);
        nvgFillColor(vg, nvgRGB(245, 200, 90));
        nvgFill(vg);

        // Voltage readout under bar
        std::snprintf(buf, sizeof(buf), "%.3f V  ·  %d / %d",
                      (float)v / (float)Seed::kMaxSeed * 10.f, v, Seed::kMaxSeed);
        nvgFontSize(vg, 8.f);
        nvgFillColor(vg, nvgRGBA(180, 190, 210, 220));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        nvgText(vg, box.size.x / 2, box.size.y - 6.f, buf, nullptr);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct SeedWidget : ModuleWidget {
    SeedWidget(Seed* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Seed.svg")));
        addChild(new ModuleTitle("SEED", 300.f));

        auto* labels = new PanelLabels(300.f);
        // VALUE label is centered (between cols 1 and 2), drawn via custom()
        labels->custom(150.f, 246.f, 9.f, nvgRGB(0x9a, 0xa0, 0xb4),
                       NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 2.f, "VALUE");
        labels->k2(3, "RANDOM");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->outSection();
        labels->out(2, "SEED·CV"); labels->out(3, "TRIG");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new SeedView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundLargeBlackKnob>(
            Vec(150, 262), module, Seed::VALUE_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Seed::RANDOMIZE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Seed::RANDOMIZE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Seed::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Seed::RESET_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Seed::SEED_CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Seed::TRIG_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Seed",
            {"Displays a single seed value (0..999) prominently.",
             "Outputs a seed CV and a TRIG that fires on every change.",
             "Patch TRIG into RESET inputs to re-initialise modules in lockstep."},
            "Sample, Boot, Cohort, Factor (any stochastic module)");
    }
};

Model* modelSeed = createModel<Seed, SeedWidget>("Seed");
