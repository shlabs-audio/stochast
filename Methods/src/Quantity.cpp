#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

// ============================================================================
// Quantity — set a real-world quantity, emit the back-converted CV.
//
//   Quantity is the mirror of Gauge. Where Gauge reads a downstream CV and
//   displays it as a real-world unit (e.g., "IQ 115"), Quantity lets the user
//   *dial in* a real-world value directly and emits the CV that would produce
//   that value through the inverse of Gauge's mapping:
//
//                CV  =  ( VALUE  −  B )  /  A
//
//   The preset menu and A / B knobs match Gauge exactly. Selecting a preset
//   stamps a known (A, B) pair into the knobs *and* sets VALUE to a sensible
//   default for that preset (the centre of its typical range). The user can
//   then dial VALUE in real-world units while the patch downstream sees the
//   appropriate CV.
//
//   Typical pairing:  Quantity  →  Test.H₀   |   Empiria viz  →  Gauge
//                     (set in real units)        (read in real units)
//
//   The CV input modulates VALUE: when patched, the emitted CV is
//                CV_OUT  =  ( VALUE + CV_IN · scale_in − B )  /  A
//   where scale_in interprets the input on the same real-world scale as
//   VALUE (CV_IN of +1 V corresponds to +1 unit of the current preset).
// ============================================================================

struct QuantityModule : Module {

    struct PresetSpec {
        const char* name;
        const char* unit;
        float a;                // multiplier (matches Gauge)
        float b;                // offset (matches Gauge)
        float valueDefault;     // sensible starting VALUE in real-world units
        int   defaultDecimals;
    };

    enum PresetIdx {
        PRESET_VOLTAGE = 0,
        PRESET_PERCENT_UNI,
        PRESET_PERCENT_BI,
        PRESET_PERCENT_SIGNED,
        PRESET_PROBABILITY,
        PRESET_ZSCORE,
        PRESET_IQ,
        PRESET_HEIGHT_CM,
        PRESET_LIKERT5,
        PRESET_LIKERT7,
        PRESET_TEST_SCORE,
        PRESET_TEMP_C,
        PRESET_TEMP_F,
        PRESET_RT_MS,
        PRESET_COUNT,
        PRESET_CUSTOM,
        NUM_PRESETS
    };

    static const PresetSpec& presetSpec(int i) {
        static const PresetSpec table[NUM_PRESETS] = {
            //  name                                       unit    A       B       defaultVALUE  dec
            {"Voltage (passthrough)",                      "V",   1.0f,    0.0f,     0.0f,        2},
            {"Percent — unipolar (0..10 V → 0..100 %)",    "%",  10.0f,    0.0f,    50.0f,        1},
            {"Percent — bipolar (−5..+5 V → 0..100 %)",    "%",  10.0f,   50.0f,    50.0f,        1},
            {"Percent — signed (±10 V → ±100 %)",          "%",  10.0f,    0.0f,     0.0f,        1},
            {"Probability (0..10 V → 0..1)",               "",    0.1f,    0.0f,     0.5f,        3},
            {"Z-score (z = V)",                            "z",   1.0f,    0.0f,     0.0f,        2},
            {"IQ (μ = 100, σ = 15)",                       "IQ", 15.0f,  100.0f,   100.0f,        0},
            {"Adult height, cm (μ = 170, σ = 10)",         "cm", 10.0f,  170.0f,   170.0f,        1},
            {"Likert 5-point (centred 3)",                 "/5",  0.4f,    3.0f,     3.0f,        0},
            {"Likert 7-point (centred 4)",                 "/7",  0.6f,    4.0f,     4.0f,        0},
            {"Test score, SAT-style (μ = 500, σ = 100)",   "pt",100.0f,  500.0f,   500.0f,        0},
            {"Temperature (°C)",                           "°C",  5.0f,   20.0f,    20.0f,        1},
            {"Temperature (°F)",                           "°F",  9.0f,   68.0f,    68.0f,        1},
            {"Reaction time, ms",                          "ms",500.0f,    0.0f,  1000.0f,        0},
            {"Count (V × 10)",                             "",   10.0f,    0.0f,    50.0f,        0},
            {"Custom (reset to A = 1, B = 0)",             "",    1.0f,    0.0f,     0.0f,        2},
        };
        return table[clamp(i, 0, NUM_PRESETS - 1)];
    }

    enum ParamId  { VALUE_PARAM, A_PARAM, B_PARAM, DECIMALS_PARAM, NUM_PARAMS };
    enum InputId  { CV_INPUT, NUM_INPUTS };
    enum OutputId { CV_OUTPUT, THRU_OUTPUT, NUM_OUTPUTS };
    enum LightId  { NUM_LIGHTS };

    int   currentPreset    = PRESET_VOLTAGE;
    float lastValue        = 0.f;    // VALUE actually used (after CV input)
    float lastCV           = 0.f;    // emitted CV
    bool  invalidA         = false;  // A ≈ 0 — division by zero guard

    QuantityModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        // VALUE knob range covers all built-in presets without clamping.
        configParam(VALUE_PARAM, -2000.f, 2000.f, 0.0f,
                    "Real-world value (units depend on the active preset; "
                    "right-click → \"Type value\" for precise entry)");
        configParam(A_PARAM,     -200.f, 200.f, 1.0f,
                    "Slope A — drives the math at all times; selecting a "
                    "preset stamps a value here");
        configParam(B_PARAM,    -2000.f, 2000.f, 0.0f,
                    "Offset B — drives the math at all times; selecting a "
                    "preset stamps a value here");
        configParam(DECIMALS_PARAM, 0.f, 4.f, 2.f, "Display decimals");
        paramQuantities[DECIMALS_PARAM]->snapEnabled = true;
        configInput(CV_INPUT,
                    "CV modulation (added to VALUE in real-world units; +1 V → +1 unit)");
        configOutput(CV_OUTPUT,
                     "Back-converted CV: ( VALUE − B ) / A, clamped to ±12 V");
        configOutput(THRU_OUTPUT,
                     "Pass-through of the CV input (unchanged) for chaining");
    }

    void onReset() override {
        currentPreset = PRESET_VOLTAGE;
        snapToPreset();
        lastValue = 0.f;
        lastCV    = 0.f;
        invalidA  = false;
    }

    // Stamp the current preset's (A, B, VALUE, DECIMALS) defaults into the
    // four knobs. Called when the user picks a preset from the right-click
    // menu, or explicitly via "Snap knobs to current preset".
    void snapToPreset() {
        const PresetSpec& ps = presetSpec(currentPreset);
        params[A_PARAM].setValue(ps.a);
        params[B_PARAM].setValue(ps.b);
        params[VALUE_PARAM].setValue(ps.valueDefault);
        params[DECIMALS_PARAM].setValue((float)ps.defaultDecimals);
    }

    // True when A, B, and VALUE all match the preset's defaults exactly.
    bool matchesPreset() {
        const PresetSpec& ps = presetSpec(currentPreset);
        const float a = params[A_PARAM].getValue();
        const float b = params[B_PARAM].getValue();
        const float v = params[VALUE_PARAM].getValue();
        return std::fabs(a - ps.a) < 1e-4f
            && std::fabs(b - ps.b) < 1e-4f
            && std::fabs(v - ps.valueDefault) < 1e-4f;
    }

    void process(const ProcessArgs& args) override {
        const float a = params[A_PARAM].getValue();
        const float b = params[B_PARAM].getValue();
        const float vKnob = params[VALUE_PARAM].getValue();

        // CV input adds to VALUE on the real-world scale, so a +1 V CV
        // modulation moves VALUE by +1 unit of the current preset.
        const int channels = inputs[CV_INPUT].isConnected()
                             ? std::max(1, inputs[CV_INPUT].getChannels()) : 1;
        outputs[CV_OUTPUT].setChannels(channels);
        outputs[THRU_OUTPUT].setChannels(channels);

        invalidA = std::fabs(a) < 1e-6f;
        float ch0value = 0.f, ch0cv = 0.f;
        for (int c = 0; c < channels; ++c) {
            float modV = inputs[CV_INPUT].isConnected()
                         ? inputs[CV_INPUT].getVoltage(c) : 0.f;
            float v = vKnob + modV;
            // Back-conversion: real → CV.
            // CV = (VALUE − B) / A. Guard against A ≈ 0 by emitting B as a
            // signed offset CV that downstream modules can still read.
            float cvOut = invalidA ? 0.f : (v - b) / a;
            outputs[CV_OUTPUT].setVoltage(clamp(cvOut, -12.f, 12.f), c);
            outputs[THRU_OUTPUT].setVoltage(modV, c);
            if (c == 0) { ch0value = v; ch0cv = cvOut; }
        }
        lastValue = ch0value;
        lastCV    = ch0cv;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "preset", json_integer(currentPreset));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "preset"))
            currentPreset = clamp((int)json_integer_value(j), 0, NUM_PRESETS - 1);
    }
};

// ============================================================================
// Visualisation: preset name + large VALUE readout + unit + emitted CV.
// ============================================================================

struct QuantityView : LightWidget {
    QuantityModule* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        nvgSave(vg);
        nvgScissor(vg, 0.f, 0.f, box.size.x, box.size.y);

        if (!module) {
            nvgFontSize(vg, 9.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 130));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "REAL  →  CV", nullptr);
            nvgRestore(vg);
            return;
        }

        const auto& ps = QuantityModule::presetSpec(module->currentPreset);
        const int decimals = clamp((int)std::round(
            module->params[QuantityModule::DECIMALS_PARAM].getValue()), 0, 4);

        const float W = box.size.x;
        const float H = box.size.y;
        const float innerW = W - 12.f;

        auto fitFontSize = [&](const char* txt,
                               std::initializer_list<float> candidates,
                               float maxW) -> float {
            float chosen = *(candidates.end() - 1);
            for (float sz : candidates) {
                nvgFontSize(vg, sz);
                float bounds[4];
                nvgTextBounds(vg, 0, 0, txt, nullptr, bounds);
                if (bounds[2] - bounds[0] <= maxW) { chosen = sz; break; }
            }
            nvgFontSize(vg, chosen);
            return chosen;
        };

        // ---- Preset name at top, with a "(modified)" hint ----
        const bool modified = !module->matchesPreset();
        std::string header = ps.name;
        if (modified) header += "  (modified)";
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, modified ? nvgRGB(245, 200, 90)
                                  : nvgRGB(0x9a, 0xa0, 0xb4));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgTextLetterSpacing(vg, 1.5f);
        fitFontSize(header.c_str(), {8.5f, 7.5f, 6.5f, 5.5f}, innerW);
        nvgText(vg, W * 0.5f, 8.f, header.c_str(), nullptr);

        // ---- Big numeric readout: the VALUE the user has dialled in ----
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, module->lastValue);
        nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgTextLetterSpacing(vg, 1.f);
        fitFontSize(buf, {36.f, 30.f, 26.f, 22.f, 18.f, 14.f}, innerW);
        nvgText(vg, W * 0.5f,        H * 0.42f, buf, nullptr);
        nvgText(vg, W * 0.5f + 0.5f, H * 0.42f, buf, nullptr);

        // ---- Unit label ----
        if (ps.unit && ps.unit[0]) {
            nvgFontSize(vg, 14.f);
            nvgFillColor(vg, nvgRGB(120, 200, 224));
            nvgTextLetterSpacing(vg, 1.5f);
            nvgText(vg, W * 0.5f, H * 0.42f + 24.f, ps.unit, nullptr);
        }

        // ---- Emitted CV (the actual downstream signal) ----
        if (module->invalidA) {
            std::snprintf(buf, sizeof(buf), "CV  ?  (A = 0)");
            nvgFontSize(vg, 8.f);
            nvgFillColor(vg, nvgRGB(245, 90, 90));
        } else {
            std::snprintf(buf, sizeof(buf), "→  CV  %+.3f V", module->lastCV);
            nvgFontSize(vg, 8.f);
            nvgFillColor(vg, nvgRGBA(0x9a, 0xa0, 0xb4, 0xa0));
        }
        nvgTextLetterSpacing(vg, 1.f);
        nvgText(vg, W * 0.5f, H * 0.42f + 44.f, buf, nullptr);

        // ---- Mapping reminder: V = (VALUE − B) / A ----
        std::snprintf(buf, sizeof(buf), "( VALUE − B ) / A   A=%.3g   B=%.3g",
                      module->params[QuantityModule::A_PARAM].getValue(),
                      module->params[QuantityModule::B_PARAM].getValue());
        nvgFontSize(vg, 7.5f);
        nvgFillColor(vg, nvgRGBA(0x9a, 0xa0, 0xb4, 0xc0));
        nvgText(vg, W * 0.5f, H * 0.42f + 56.f, buf, nullptr);

        nvgRestore(vg);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, W - 1, H - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct QuantityWidget : ModuleWidget {
    QuantityWidget(QuantityModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Quantity.svg")));
        addChild(new ModuleTitle("QUANTITY", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "VALUE");
        labels->k1(1, "A");
        labels->k1(2, "B");
        labels->k1(3, "DECIM");
        labels->inSection();
        labels->in(0, "CV");
        labels->outSection();
        labels->out(0, "CV");
        labels->out(1, "THRU");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new QuantityView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, QuantityModule::VALUE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, QuantityModule::A_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, QuantityModule::B_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, QuantityModule::DECIMALS_PARAM));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, QuantityModule::CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, QuantityModule::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, QuantityModule::THRU_OUTPUT));
    }

    // Right-click → unit preset (selecting one stamps VALUE, A, B, DECIM)
    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<QuantityModule*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Unit preset"));

        struct PresetItem : MenuItem {
            QuantityModule* m;
            int p;
            void onAction(const event::Action&) override {
                m->currentPreset = p;
                m->snapToPreset();
            }
        };
        for (int i = 0; i < QuantityModule::NUM_PRESETS; ++i) {
            const auto& ps = QuantityModule::presetSpec(i);
            auto* it = new PresetItem;
            it->text = ps.name;
            it->m = m;
            it->p = i;
            it->rightText = (m->currentPreset == i)
                ? (m->matchesPreset() ? "✓" : "(modified)")
                : "";
            menu->addChild(it);
        }

        struct SnapItem : MenuItem {
            QuantityModule* m;
            void onAction(const event::Action&) override { m->snapToPreset(); }
        };
        auto* snap = new SnapItem;
        snap->text = "Snap knobs to current preset";
        snap->m = m;
        snap->disabled = m->matchesPreset();
        menu->addChild(snap);

        appendAboutMenu(menu, "Quantity",
            {"Dial a real-world quantity (IQ, °C, %, z-score, ...) and emit",
             "the back-converted CV. Mirror of Gauge: Quantity sets,",
             "Gauge reads. Presets stamp known (A, B, VALUE) into the knobs."},
            "Test (set H₀ in real units), Code (set LOW/HIGH cutpoints), Gauge (readout)");
    }
};

Model* modelQuantity = createModel<QuantityModule, QuantityWidget>("Quantity");
