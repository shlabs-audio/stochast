#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

// ============================================================================
// Gauge — convert a control voltage to a real-world quantity.
//
//   VCV Rack signals are control voltages (CV), usually scaled to 0..10 V
//   or ±10 V. Those numbers are perfectly serviceable for a synthesist but
//   utterly opaque to a learner who is trying to map a slider position to a
//   real-world concept like "IQ score 115" or "59 % probability". Gauge
//   closes that gap. It takes a CV input, applies a linear mapping
//   x = A·V + B, and reports the result both as a large on-panel numeric
//   readout and as a scaled CV output. The original CV is also passed
//   straight through unchanged so downstream Empiria modules continue to
//   see voltage in its native form.
//
//   The A and B knobs drive the mapping at all times. A preset is just a
//   shortcut that stamps a known (A, B) pair into the knobs — selecting it
//   from the right-click menu overwrites the knob values, but afterwards
//   the user can dial them by hand to tune the mapping to their patch.
//   The right-click menu also has "Snap knobs to current preset" to revert
//   manual edits and a "Custom (reset to A=1, B=0)" identity preset.
//
//   Built-in presets cover the units that recur in introductory statistics
//   and applied social-science teaching:
//
//     Voltage              A = 1,    B = 0      — passthrough display
//     Percent unipolar     A = 10,   B = 0      — 0..10 V → 0..100 %
//     Percent bipolar      A = 10,   B = 50     — −5..+5 V → 0..100 %
//     Percent signed       A = 10,   B = 0      — bipolar input → ±100 %
//     Probability          A = 0.1,  B = 0      — 0..10 V → 0..1
//     Z-score              A = 1,    B = 0      — z directly
//     IQ                   A = 15,   B = 100    — μ = 100, σ = 15
//     Height (cm)          A = 10,   B = 170    — adult human height
//     Likert (5)           A = 0.4,  B = 3      — 5-point centred at 3
//     Likert (7)           A = 0.6,  B = 4      — 7-point centred at 4
//     Test (SAT)           A = 100,  B = 500    — SAT-style standardised
//     Temperature °C       A = 5,    B = 20     — room temperature ± swing
//     Temperature °F       A = 9,    B = 68     — same in Fahrenheit
//     Reaction time, ms    A = 500,  B = 0      — pair with DDM.RT
//     Count                A = 10,   B = 0      — N-of-trials counts
//     Custom               A = 1,    B = 0      — identity reset
//
//   For percent specifically: the bipolar variant maps a typical −5..+5 V
//   LFO / modulation signal onto 0..100 %, which is what teachers usually
//   want when wiring an audio-rate or bipolar source into a "percent"
//   readout. The signed variant preserves the sign so you can read a CV
//   that genuinely swings ±.
// ============================================================================

struct Gauge : Module {

    struct PresetSpec {
        const char* name;
        const char* unit;       // displayed beside the numeric readout
        float a;                // multiplier
        float b;                // offset
        int   defaultDecimals;  // suggested display precision
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
            {"Voltage (passthrough)",                    "V",   1.0f,    0.0f, 2},
            {"Percent — unipolar (0..10 V → 0..100 %)",  "%",  10.0f,    0.0f, 1},
            {"Percent — bipolar (−5..+5 V → 0..100 %)",  "%",  10.0f,   50.0f, 1},
            {"Percent — signed (±10 V → ±100 %)",        "%",  10.0f,    0.0f, 1},
            {"Probability (0..10 V → 0..1)",             "",    0.1f,    0.0f, 3},
            {"Z-score (z = V)",                          "z",   1.0f,    0.0f, 2},
            {"IQ (μ = 100, σ = 15) — pair with Sample N(0,1)",
                                                         "IQ", 15.0f,  100.0f, 0},
            {"Adult height, cm (μ = 170, σ = 10) — pair with Sample N(0,1)",
                                                         "cm", 10.0f,  170.0f, 1},
            {"Likert 5-point (centred 3)",               "/5",  0.4f,    3.0f, 1},
            {"Likert 7-point (centred 4)",               "/7",  0.6f,    4.0f, 1},
            {"Test score, SAT-style (μ = 500, σ = 100) — pair with Sample N(0,1)",
                                                         "pt",100.0f,  500.0f, 0},
            {"Temperature (°C)",                         "°C",  5.0f,   20.0f, 1},
            {"Temperature (°F)",                         "°F",  9.0f,   68.0f, 1},
            {"Reaction time, ms — pair with DDM.RT",     "ms",500.0f,    0.0f, 0},
            {"Count (V × 10) — pair with N-of-trials outputs",
                                                         "",   10.0f,    0.0f, 0},
            {"Custom (reset to A = 1, B = 0)",           "",    1.0f,    0.0f, 2},
        };
        return table[clamp(i, 0, NUM_PRESETS - 1)];
    }

    enum ParamId  { A_PARAM, B_PARAM, DECIMALS_PARAM, RANGE_PARAM, NUM_PARAMS };
    enum InputId  { IN_INPUT, NUM_INPUTS };
    enum OutputId { VAL_OUTPUT, THRU_OUTPUT, NUM_OUTPUTS };
    enum LightId  { NUM_LIGHTS };

    int   currentPreset = PRESET_VOLTAGE;
    float lastValueV       = 0.f;   // raw V observed on input ch0
    float lastValueScaled  = 0.f;   // scaled value displayed
    float lastValueMin     = 0.f;   // observed min over recent history
    float lastValueMax     = 0.f;   // observed max
    static constexpr int kHistLen = 256;
    std::array<float, kHistLen> hist{};
    int   histIdx = 0;
    int   histFill = 0;

    Gauge() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(A_PARAM,  -200.f,  200.f, 1.0f,
                    "Slope A (multiplier). Selecting a preset stamps a value into this knob; tweak by hand to refine.");
        configParam(B_PARAM, -2000.f, 2000.f, 0.0f,
                    "Offset B. Selecting a preset stamps a value into this knob; tweak by hand to refine.");
        configParam(DECIMALS_PARAM, 0.f, 4.f, 2.f, "Display decimals");
        paramQuantities[DECIMALS_PARAM]->snapEnabled = true;
        configParam(RANGE_PARAM, 0.f, 1.f, 1.f,
                    "Bar range (0 = narrow, 1 = wide; only affects the on-panel gauge bar)");
        configInput(IN_INPUT,
                    "CV to convert (polyphonic; the readout shows channel 1)");
        configOutput(VAL_OUTPUT,
                    "Converted value as CV (clamped to ±12 V)");
        configOutput(THRU_OUTPUT,
                    "Pass-through of the input (unchanged) for chaining");
    }

    void onReset() override {
        currentPreset = PRESET_VOLTAGE;
        snapToPreset();
        hist.fill(0.f);
        histIdx = 0; histFill = 0;
        lastValueV = lastValueScaled = lastValueMin = lastValueMax = 0.f;
    }

    // Overwrite the A and B knob values with the current preset's defaults.
    // Called when the user picks a preset from the right-click menu, or
    // explicitly via "Snap knobs to current preset" to discard manual edits.
    void snapToPreset() {
        const PresetSpec& ps = presetSpec(currentPreset);
        params[A_PARAM].setValue(ps.a);
        params[B_PARAM].setValue(ps.b);
        params[DECIMALS_PARAM].setValue((float)ps.defaultDecimals);
    }

    // True when the knob values match the current preset's defaults exactly.
    // Used by the visualisation to show a "(modified)" hint. Non-const because
    // Rack's engine::Param::getValue() isn't const-qualified.
    bool matchesPreset() {
        const PresetSpec& ps = presetSpec(currentPreset);
        const float a = params[A_PARAM].getValue();
        const float b = params[B_PARAM].getValue();
        return std::fabs(a - ps.a) < 1e-4f && std::fabs(b - ps.b) < 1e-4f;
    }

    void process(const ProcessArgs& args) override {
        // Knobs always drive the math. The preset name is just a label
        // describing which (A, B) pair the user originally stamped in.
        const float a = params[A_PARAM].getValue();
        const float b = params[B_PARAM].getValue();

        const int channels = inputs[IN_INPUT].isConnected()
                             ? std::max(1, inputs[IN_INPUT].getChannels()) : 1;
        outputs[VAL_OUTPUT].setChannels(channels);
        outputs[THRU_OUTPUT].setChannels(channels);

        float ch0v = 0.f, ch0x = 0.f;
        for (int c = 0; c < channels; ++c) {
            float v = inputs[IN_INPUT].isConnected()
                      ? inputs[IN_INPUT].getVoltage(c) : 0.f;
            float x = a * v + b;
            outputs[VAL_OUTPUT].setVoltage(clamp(x, -12.f, 12.f), c);
            outputs[THRU_OUTPUT].setVoltage(v, c);
            if (c == 0) { ch0v = v; ch0x = x; }
        }
        lastValueV      = ch0v;
        lastValueScaled = ch0x;

        // Push channel-0 scaled value into a small ring buffer at ~120 Hz
        // so the visualisation can show a recent-range bar.
        static int frameCount = 0;
        int stride = std::max(1, (int)(args.sampleRate / 120.f));
        if (++frameCount >= stride) {
            frameCount = 0;
            hist[histIdx] = ch0x;
            histIdx = (histIdx + 1) % kHistLen;
            if (histFill < kHistLen) ++histFill;

            // Recompute observed min / max for the bar layout.
            float vmin = +1e30f, vmax = -1e30f;
            for (int i = 0; i < histFill; ++i) {
                float h = hist[i];
                if (h < vmin) vmin = h;
                if (h > vmax) vmax = h;
            }
            if (histFill == 0) { vmin = 0.f; vmax = 0.f; }
            lastValueMin = vmin;
            lastValueMax = vmax;
        }
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
// Visualisation: large numeric readout + unit label + recent-range bar.
// ============================================================================

struct GaugeView : LightWidget {
    Gauge* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        // Clip everything in the viz to the widget box. Big readouts and long
        // preset names with letter-spacing can otherwise overflow the 280 px
        // width and paint onto the surrounding panel.
        nvgSave(vg);
        nvgScissor(vg, 0.f, 0.f, box.size.x, box.size.y);

        if (!module) {
            nvgFontSize(vg, 9.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 130));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "CV  →  REAL UNITS", nullptr);
            nvgRestore(vg);
            return;
        }

        const auto& ps = Gauge::presetSpec(module->currentPreset);
        const int decimals = clamp((int)std::round(
            module->params[Gauge::DECIMALS_PARAM].getValue()), 0, 4);

        const float W = box.size.x;
        const float H = box.size.y;
        // Inner content width (leaves a small margin so text never butts up
        // against the viz border).
        const float innerW = W - 12.f;

        // Pick the largest font size from `candidates` that makes `txt` fit
        // inside `maxW`, using the current letter-spacing. Returns the size.
        auto fitFontSize = [&](const char* txt,
                               std::initializer_list<float> candidates,
                               float maxW) -> float {
            float chosen = *(candidates.end() - 1);   // smallest is the fallback
            for (float sz : candidates) {
                nvgFontSize(vg, sz);
                float bounds[4];
                nvgTextBounds(vg, 0, 0, txt, nullptr, bounds);
                if (bounds[2] - bounds[0] <= maxW) { chosen = sz; break; }
            }
            nvgFontSize(vg, chosen);
            return chosen;
        };

        // ---- Preset name at top, with a "(modified)" hint when knobs drift ----
        const bool modified = !module->matchesPreset();
        std::string header = ps.name;
        if (modified) header += "  (modified)";
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, modified ? nvgRGB(245, 200, 90)
                                  : nvgRGB(0x9a, 0xa0, 0xb4));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgTextLetterSpacing(vg, 1.5f);
        // Some preset names ("Percent — bipolar (−5..+5 V → 0..100 %)") are
        // very long; shrink the header font if it would overflow.
        fitFontSize(header.c_str(), {8.5f, 7.5f, 6.5f, 5.5f}, innerW);
        nvgText(vg, W * 0.5f, 8.f, header.c_str(), nullptr);

        // ---- Big numeric readout ------------------------------------------
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, module->lastValueScaled);

        nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgTextLetterSpacing(vg, 1.f);
        // Large numbers (e.g. SAT scores, RT in ms, percent at extreme inputs)
        // can be very wide at 36 pt; shrink toward 14 pt as needed so they
        // never overflow.
        fitFontSize(buf, {36.f, 30.f, 26.f, 22.f, 18.f, 14.f}, innerW);
        // faux-bold by drawing twice with a small horizontal offset
        nvgText(vg, W * 0.5f,        H * 0.42f, buf, nullptr);
        nvgText(vg, W * 0.5f + 0.5f, H * 0.42f, buf, nullptr);

        // ---- Unit label below the number ----------------------------------
        if (ps.unit && ps.unit[0]) {
            nvgFontSize(vg, 14.f);
            nvgFillColor(vg, nvgRGB(120, 200, 224));
            nvgTextLetterSpacing(vg, 1.5f);
            nvgText(vg, W * 0.5f, H * 0.42f + 24.f, ps.unit, nullptr);
        }

        // ---- Raw CV (always visible) --------------------------------------
        std::snprintf(buf, sizeof(buf), "raw  %+.2f V  →  ch1", module->lastValueV);
        nvgFontSize(vg, 8.f);
        nvgFillColor(vg, nvgRGBA(0x9a, 0xa0, 0xb4, 0xa0));
        nvgTextLetterSpacing(vg, 1.f);
        nvgText(vg, W * 0.5f, H * 0.42f + 44.f, buf, nullptr);

        // ---- Active mapping line — always shown so the formula is visible -
        std::snprintf(buf, sizeof(buf), "A·V + B   A=%.3g   B=%.3g",
                      module->params[Gauge::A_PARAM].getValue(),
                      module->params[Gauge::B_PARAM].getValue());
        nvgFontSize(vg, 7.5f);
        nvgFillColor(vg, nvgRGBA(0x9a, 0xa0, 0xb4, 0xc0));
        nvgText(vg, W * 0.5f, H * 0.42f + 56.f, buf, nullptr);

        // ---- Recent-range bar at the bottom -------------------------------
        const float barH = 14.f;
        const float barY = H - 30.f;
        const float barX0 = 16.f, barX1 = W - 16.f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, barX0, barY, barX1 - barX0, barH, 3.f);
        nvgFillColor(vg, nvgRGBA(40, 46, 70, 200));
        nvgFill(vg);

        // Choose a display window: prefer the preset's typical span; if the
        // observed history is wider than that, expand to fit.
        float spanLo = 0.f, spanHi = 0.f;
        switch (module->currentPreset) {
            case Gauge::PRESET_PERCENT_UNI:    spanLo = 0.f;    spanHi = 100.f;  break;
            case Gauge::PRESET_PERCENT_BI:     spanLo = 0.f;    spanHi = 100.f;  break;
            case Gauge::PRESET_PERCENT_SIGNED: spanLo = -100.f; spanHi = 100.f;  break;
            case Gauge::PRESET_PROBABILITY:    spanLo = 0.f;    spanHi = 1.f;    break;
            case Gauge::PRESET_ZSCORE:         spanLo = -3.f;   spanHi = 3.f;    break;
            case Gauge::PRESET_IQ:             spanLo = 55.f;   spanHi = 145.f;  break;
            case Gauge::PRESET_HEIGHT_CM:      spanLo = 130.f;  spanHi = 210.f;  break;
            case Gauge::PRESET_LIKERT5:        spanLo = 1.f;    spanHi = 5.f;    break;
            case Gauge::PRESET_LIKERT7:        spanLo = 1.f;    spanHi = 7.f;    break;
            case Gauge::PRESET_TEST_SCORE:     spanLo = 200.f;  spanHi = 800.f;  break;
            case Gauge::PRESET_TEMP_C:         spanLo = -30.f;  spanHi = 70.f;   break;
            case Gauge::PRESET_TEMP_F:         spanLo = -22.f;  spanHi = 158.f;  break;
            case Gauge::PRESET_RT_MS:          spanLo = 0.f;    spanHi = 3000.f; break;
            case Gauge::PRESET_COUNT:          spanLo = 0.f;    spanHi = 100.f;  break;
            default:                           spanLo = -12.f;  spanHi = 12.f;   break;
        }
        // Expand to fit observed history if necessary.
        if (module->histFill > 0) {
            spanLo = std::min(spanLo, module->lastValueMin);
            spanHi = std::max(spanHi, module->lastValueMax);
        }
        if (spanHi - spanLo < 1e-6f) spanHi = spanLo + 1.f;

        // Position pointer
        float t = (module->lastValueScaled - spanLo) / (spanHi - spanLo);
        t = clamp(t, 0.f, 1.f);
        float px = barX0 + t * (barX1 - barX0);

        // Filled portion left of pointer
        nvgBeginPath(vg);
        nvgRoundedRect(vg, barX0, barY, std::max(0.f, px - barX0), barH, 3.f);
        nvgFillColor(vg, nvgRGB(120, 200, 224));
        nvgFill(vg);

        // Pointer line
        nvgBeginPath(vg);
        nvgMoveTo(vg, px, barY - 3.f);
        nvgLineTo(vg, px, barY + barH + 3.f);
        nvgStrokeColor(vg, nvgRGB(245, 200, 90));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);

        // Bar endpoint labels
        char bufLo[24], bufHi[24];
        std::snprintf(bufLo, sizeof(bufLo), "%.*f", decimals, spanLo);
        std::snprintf(bufHi, sizeof(bufHi), "%.*f", decimals, spanHi);
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(0x9a, 0xa0, 0xb4, 0xc0));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, barX0, barY + barH + 2.f, bufLo, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, barX1, barY + barH + 2.f, bufHi, nullptr);

        nvgRestore(vg);   // end of scissor block

        // Frame (drawn after restoring the scissor so the border stroke is
        // not clipped at the edge pixels).
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

struct GaugeWidget : ModuleWidget {
    GaugeWidget(Gauge* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Gauge.svg")));
        addChild(new ModuleTitle("GAUGE", 300.f));

        // Panel labels (rendered via NanoVG)
        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "A");
        labels->k1(1, "B");
        labels->k1(2, "DECIM");
        labels->k1(3, "RANGE");
        labels->inSection();
        labels->in(0, "IN");
        labels->outSection();
        labels->out(0, "VAL");
        labels->out(1, "THRU");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new GaugeView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Gauge::A_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Gauge::B_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Gauge::DECIMALS_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Gauge::RANGE_PARAM));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Gauge::IN_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Gauge::VAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Gauge::THRU_OUTPUT));
    }

    // Right-click → unit preset (selecting one stamps A and B into the knobs)
    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Gauge*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Unit preset"));

        // Selecting a preset records the new preset index AND overwrites the
        // A / B / DECIMALS knobs with the preset's defaults. The user can
        // then tweak the knobs by hand; "Snap knobs to current preset" below
        // reverts the manual edits.
        struct PresetItem : MenuItem {
            Gauge* m;
            int p;
            void onAction(const event::Action&) override {
                m->currentPreset = p;
                m->snapToPreset();
            }
        };
        for (int i = 0; i < Gauge::NUM_PRESETS; ++i) {
            const auto& ps = Gauge::presetSpec(i);
            auto* it = new PresetItem;
            it->text = ps.name;
            it->m = m;
            it->p = i;
            it->rightText = (m->currentPreset == i)
                ? (m->matchesPreset() ? "✓" : "(modified)")
                : "";
            menu->addChild(it);
        }

        // Quick "snap back" — only meaningful when the user has dialled the
        // knobs away from the preset's defaults.
        struct SnapItem : MenuItem {
            Gauge* m;
            void onAction(const event::Action&) override { m->snapToPreset(); }
        };
        auto* snap = new SnapItem;
        snap->text = "Snap knobs to current preset";
        snap->m = m;
        snap->disabled = m->matchesPreset();
        menu->addChild(snap);

        appendAboutMenu(menu, "Gauge",
            {"Translates a CV into a real-world quantity via x = A·V + B.",
             "Presets stamp known (A, B) pairs into the knobs; the knobs",
             "always drive the math, so tweak by hand to fine-tune."},
            "Frame's MEAN, Test's t, Boot's EST, Code's CAT, Cohort's CTR");
    }
};

Model* modelGauge = createModel<Gauge, GaugeWidget>("Gauge");
