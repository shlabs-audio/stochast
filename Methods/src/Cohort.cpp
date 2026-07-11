#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
// Cohort — online k-means quantiser.
//
//   Maintains K cluster centres along a 1-D voltage axis. On every audio
//   sample, the centre closest to the current SIG input is identified (the
//   "winner") and the SIG voltage is quantised to that centre's value.
//   If TRIG is unpatched, the winner centre is pulled toward SIG by η on
//   every sample (online k-means / competitive learning); if TRIG is
//   patched, the centre only learns on rising triggers. Either way, the
//   quantised OUT is updated continuously, so the output always reflects
//   the most-recent input.
//
//   Pedagogically: Cohort gives students a hands-on view of unsupervised
//   1-D categorisation. Feed it a bimodal sample (Sample's "U-shaped
//   opinion" preset, say) and the centres self-organise to the two modes.
//   Set SEMI on and Cohort doubles as a self-tuning scale quantiser for
//   musical CVs.
// ============================================================================

struct Cohort : Module {
    enum ParamId  { K_PARAM, RATE_PARAM, RANGE_PARAM, SEMI_PARAM, RESET_PARAM, NUM_PARAMS };
    enum InputId  { SIGNAL_INPUT, TRIG_INPUT, NUM_INPUTS };
    enum OutputId { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightId  { SEMI_LIGHT, RESET_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxK = 12;
    static constexpr int HIST_BUF = 1024;

    std::array<float, kMaxK> centres{};
    int activeK = 6;
    int prevActiveK = 6;
    float currentRangeV = 5.f;
    float lastInput = 0.f;
    float lastOutput = 0.f;
    int   lastWinner = 0;

    // Recent-input ring buffer for the density histogram
    float histRing[HIST_BUF] = {};
    int   histWriteIdx = 0;
    int   scopeFrameCounter = 0;

    dsp::SchmittTrigger trigSchmitt;
    dsp::SchmittTrigger resetSchmitt;

    Cohort() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(K_PARAM, 2.f, (float)kMaxK, 6.f, "Clusters (K)");
        paramQuantities[K_PARAM]->snapEnabled = true;
        configParam(RATE_PARAM,  0.f, 1.f, 0.30f, "Learning rate");
        configParam(RANGE_PARAM, 0.f, 1.f, 0.50f, "Reset spread (±V)");
        configSwitch(SEMI_PARAM, 0.f, 1.f, 0.f, "Semitone snap", {"Off", "On"});
        configButton(RESET_PARAM, "Reset cluster centres");
        configInput(SIGNAL_INPUT, "Signal");
        configInput(TRIG_INPUT, "Update trigger (continuous if unpatched)");
        configOutput(OUT_OUTPUT, "Quantised");
        initCentres(5.f);
    }

    void initCentres(float rangeV) {
        currentRangeV = rangeV;
        // Spread the active K centres evenly across [-rangeV, +rangeV].
        // Inactive slots (i >= activeK) are parked at zero — they're never
        // visited by the winner-search loop, but live in the same array so
        // the dataToJson persistence round-trip stays simple.
        const int K = std::max(2, activeK);
        for (int i = 0; i < kMaxK; ++i) {
            if (i < K) {
                float t = (K > 1) ? (float)i / (K - 1) : 0.5f;
                centres[i] = -rangeV + 2.f * rangeV * t;
            } else {
                centres[i] = 0.f;
            }
        }
    }

    void onReset() override {
        initCentres(5.f);
        for (int i = 0; i < HIST_BUF; ++i) histRing[i] = 0.f;
        histWriteIdx = 0;
        lastInput = lastOutput = 0.f;
    }

    void process(const ProcessArgs& args) override {
        activeK = clamp((int)std::round(params[K_PARAM].getValue()), 2, kMaxK);

        float rateT = clamp(params[RATE_PARAM].getValue(), 0.f, 1.f);
        float eta = (rateT < 1e-4f) ? 0.f : 0.00005f * std::pow(1000.f, rateT);

        float rangeT = clamp(params[RANGE_PARAM].getValue(), 0.f, 1.f);
        float rangeV = 0.5f + 9.5f * rangeT;
        currentRangeV = rangeV;

        // If the user changed K, redistribute the centres across the current
        // RANGE so the new clustering starts from a sensible spread rather
        // than from stale values left over from the previous K.
        if (activeK != prevActiveK) {
            initCentres(rangeV);
            prevActiveK = activeK;
        }

        bool semi = params[SEMI_PARAM].getValue() > 0.5f;
        lights[SEMI_LIGHT].setBrightness(semi ? 1.f : 0.f);

        if (resetSchmitt.process(params[RESET_PARAM].getValue())) {
            initCentres(rangeV);
        }
        lights[RESET_LIGHT].setBrightness(params[RESET_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        bool doUpdate;
        if (inputs[TRIG_INPUT].isConnected()) {
            doUpdate = trigSchmitt.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
        } else {
            doUpdate = true;
        }
        // Only learn when SIG is actually patched — otherwise an unpatched
        // module would drag all centres to 0 V over time (k-means on a
        // constant input does converge, but it's a confusing default).
        const bool sigPatched = inputs[SIGNAL_INPUT].isConnected();

        float x = inputs[SIGNAL_INPUT].getVoltage();
        lastInput = x;

        int best = 0;
        float bestD = std::abs(x - centres[0]);
        for (int i = 1; i < activeK; ++i) {
            float d = std::abs(x - centres[i]);
            if (d < bestD) { bestD = d; best = i; }
        }
        lastWinner = best;

        if (doUpdate && eta > 0.f && sigPatched) {
            centres[best] += eta * (x - centres[best]);
        }

        float out = centres[best];
        if (semi) out = std::round(out * 12.f) / 12.f;
        lastOutput = out;
        outputs[OUT_OUTPUT].setVoltage(clamp(out, -10.f, 10.f));

        // Push to histogram ring at ~120Hz
        int stride = std::max(1, (int)(args.sampleRate / 120.f));
        if (++scopeFrameCounter >= stride) {
            scopeFrameCounter = 0;
            histRing[histWriteIdx] = x;
            histWriteIdx = (histWriteIdx + 1) % HIST_BUF;
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_t* arr = json_array();
        for (int i = 0; i < kMaxK; ++i)
            json_array_append_new(arr, json_real(centres[i]));
        json_object_set_new(root, "centres", arr);
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* arr = json_object_get(root, "centres");
        if (arr && json_is_array(arr)) {
            size_t n = json_array_size(arr);
            for (size_t i = 0; i < n && i < (size_t)kMaxK; ++i) {
                json_t* v = json_array_get(arr, i);
                if (json_is_number(v)) centres[i] = (float)json_number_value(v);
            }
        }
        // Params are restored before dataFromJson: sync prevActiveK to the
        // loaded K so the first process() does not fire a spurious reinit that
        // would clobber the centres we just restored.
        prevActiveK = clamp((int)std::round(params[K_PARAM].getValue()), 2, kMaxK);
    }
};

// ============================================================================
// Scope widget — density histogram of recent inputs (top), cluster-axis
// with K centre ticks + current-input pointer (bottom).
// ============================================================================

struct CohortScope : LightWidget {
    Cohort* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        // Dark background over SVG rect
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        if (!module) {
            nvgFontSize(vg, 9.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 120));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "INPUT DENSITY  +  CENTRES", nullptr);
            return;
        }

        const float W = box.size.x;
        const float H = box.size.y;
        const float histH = H * 0.50f;     // top half — histogram
        const float axisY = H * 0.68f;     // axis line
        const float textY = H - 4.f;       // bottom — output readout
        const int activeK = module->activeK;

        // Auto-fit window: frame active centres + recent input data,
        // independent of the RANGE knob (which only controls spread on RESET).
        float vmin = +1e9f, vmax = -1e9f;
        for (int i = 0; i < activeK; ++i) {
            float c = module->centres[i];
            if (c < vmin) vmin = c;
            if (c > vmax) vmax = c;
        }
        for (int i = 0; i < Cohort::HIST_BUF; ++i) {
            float v = module->histRing[i];
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
        if (vmin > vmax) { vmin = -1.f; vmax = 1.f; }
        float span = std::max(0.5f, vmax - vmin);
        float pad  = span * 0.08f;
        vmin -= pad;
        vmax += pad;
        float invSpan = 1.f / (vmax - vmin);
        auto valX = [&](float v) { return (v - vmin) * invSpan * W; };

        // Cluster palette (stable across K so colors don't reshuffle when K changes)
        static const NVGcolor kPalette[12] = {
            nvgRGB(232, 100, 100),
            nvgRGB(232, 150,  60),
            nvgRGB(220, 200,  60),
            nvgRGB(150, 210,  80),
            nvgRGB( 80, 200, 120),
            nvgRGB( 80, 200, 200),
            nvgRGB( 80, 160, 240),
            nvgRGB(140, 110, 230),
            nvgRGB(210, 110, 220),
            nvgRGB(220, 130, 170),
            nvgRGB(180, 160, 120),
            nvgRGB(150, 180, 220),
        };
        auto clusterCol = [&](int i, float a) {
            NVGcolor c = kPalette[i % 12];
            c.a = a;
            return c;
        };

        // 1D Voronoi: which cluster owns this value?
        auto clusterOf = [&](float v) {
            int best = 0;
            float bestD = std::abs(v - module->centres[0]);
            for (int i = 1; i < activeK; ++i) {
                float d = std::abs(v - module->centres[i]);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        };

        // --- Decision boundaries (midpoints between sorted centres) -------
        // Draw first so everything else sits on top.
        float sortedC[Cohort::kMaxK];
        for (int i = 0; i < activeK; ++i) sortedC[i] = module->centres[i];
        std::sort(sortedC, sortedC + activeK);
        for (int i = 0; i + 1 < activeK; ++i) {
            float mid = 0.5f * (sortedC[i] + sortedC[i + 1]);
            if (mid < vmin || mid > vmax) continue;
            float x = valX(mid);
            // Dashed vertical line spanning full scope height
            for (float y = 2.f; y < H - 2.f; y += 4.f) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, x, y);
                nvgLineTo(vg, x, std::min(H - 2.f, y + 2.f));
                nvgStrokeColor(vg, nvgRGBA(70, 80, 105, 200));
                nvgStrokeWidth(vg, 0.5f);
                nvgStroke(vg);
            }
        }

        // --- Density histogram, cluster-coloured --------------------------
        const int NB = 32;
        int bins[NB] = {};
        int peak = 1;
        for (int i = 0; i < Cohort::HIST_BUF; ++i) {
            float v = module->histRing[i];
            if (v < vmin || v > vmax) continue;
            int b = (int)((v - vmin) * invSpan * NB);
            if (b < 0) b = 0;
            if (b >= NB) b = NB - 1;
            bins[b]++;
            if (bins[b] > peak) peak = bins[b];
        }
        float binW = W / NB;
        for (int b = 0; b < NB; ++b) {
            if (bins[b] == 0) continue;
            float binMid = vmin + (b + 0.5f) / NB * (vmax - vmin);
            int owner = clusterOf(binMid);
            float frac = (float)bins[b] / peak;
            float barH = frac * histH;
            nvgBeginPath(vg);
            nvgRect(vg, b * binW + 0.5f, histH - barH, binW - 1.f, barH);
            nvgFillColor(vg, clusterCol(owner, 0.85f));
            nvgFill(vg);
        }

        // --- Axis line ----------------------------------------------------
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0, axisY);
        nvgLineTo(vg, W, axisY);
        nvgStrokeColor(vg, nvgRGB(80, 90, 120));
        nvgStrokeWidth(vg, 0.7f);
        nvgStroke(vg);

        // Integer-voltage ticks + sparse labels
        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        int vstart = (int)std::ceil(vmin);
        int vend   = (int)std::floor(vmax);
        int step = 1;
        if (vend - vstart > 12) step = 2;
        if (vend - vstart > 24) step = 5;
        nvgFillColor(vg, nvgRGBA(120, 130, 150, 160));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        for (int v = vstart; v <= vend; v += step) {
            float x = valX((float)v);
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, axisY - 2);
            nvgLineTo(vg, x, axisY + 2);
            nvgStrokeColor(vg, nvgRGB(80, 90, 120));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
            if (v == 0 || std::abs(v) % (step * 2) == 0) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", v);
                nvgText(vg, x, axisY + 3, buf, nullptr);
            }
        }

        // --- Cluster centres ---------------------------------------------
        const int winner = module->lastWinner;
        for (int i = 0; i < activeK; ++i) {
            float c = module->centres[i];
            if (c < vmin || c > vmax) continue;
            float x = valX(c);
            bool isWin = (i == winner);

            // Winner gets a soft glow ring
            if (isWin) {
                nvgBeginPath(vg);
                nvgCircle(vg, x, axisY, 7.f);
                nvgFillColor(vg, clusterCol(i, 0.25f));
                nvgFill(vg);
            }
            nvgBeginPath(vg);
            nvgCircle(vg, x, axisY, isWin ? 4.f : 2.8f);
            nvgFillColor(vg, clusterCol(i, 1.f));
            nvgFill(vg);
        }

        // --- Input → winner: vertical drop + snap arc --------------------
        {
            float inV = clamp(module->lastInput, vmin, vmax);
            float inX = valX(inV);
            NVGcolor pulse = clusterCol(winner, 0.95f);

            // Vertical drop line from top through histogram down toward axis
            nvgBeginPath(vg);
            nvgMoveTo(vg, inX, 2.f);
            nvgLineTo(vg, inX, axisY - 6.f);
            nvgStrokeColor(vg, pulse);
            nvgStrokeWidth(vg, 1.1f);
            nvgStroke(vg);

            // Small "IN" caret at top
            nvgBeginPath(vg);
            nvgMoveTo(vg, inX, 2.f);
            nvgLineTo(vg, inX - 3.f, 7.f);
            nvgLineTo(vg, inX + 3.f, 7.f);
            nvgClosePath(vg);
            nvgFillColor(vg, pulse);
            nvgFill(vg);

            // Arc from (inX, axisY-6) to (winnerX, axisY) showing the snap
            float winX = valX(clamp(module->centres[winner], vmin, vmax));
            if (std::abs(winX - inX) > 0.5f) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, inX, axisY - 6.f);
                nvgQuadTo(vg, (inX + winX) * 0.5f, axisY - 1.f, winX, axisY);
                nvgStrokeColor(vg, pulse);
                nvgStrokeWidth(vg, 1.2f);
                nvgLineCap(vg, NVG_ROUND);
                nvgStroke(vg);

                // Tiny arrowhead at the winner end
                float dir = (winX > inX) ? 1.f : -1.f;
                nvgBeginPath(vg);
                nvgMoveTo(vg, winX, axisY);
                nvgLineTo(vg, winX - dir * 3.5f, axisY - 2.5f);
                nvgLineTo(vg, winX - dir * 3.5f, axisY + 2.5f);
                nvgClosePath(vg);
                nvgFillColor(vg, pulse);
                nvgFill(vg);
            }
        }

        // --- IN / OUT readout at bottom ----------------------------------
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "IN %+.2f   ->   OUT %+.2f V",
                     module->lastInput, module->lastOutput);
            nvgFontSize(vg, 8.f);
            nvgFillColor(vg, clusterCol(winner, 1.f));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
            nvgText(vg, W * 0.5f, textY, buf, nullptr);
        }

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

struct CohortWidget : ModuleWidget {
    CohortWidget(Cohort* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Cohort.svg")));
        addChild(new ModuleTitle("COHORT", 180.f));

        // Cohort uses a non-standard 12HP layout — all labels are placed
        // via custom() at the existing widget coordinates.
        auto* labels = new PanelLabels(180.f);
        const NVGcolor cLabel = nvgRGB(0x9a, 0xa0, 0xb4);
        const NVGcolor cBright = nvgRGB(0xe6, 0xe9, 0xf2);
        const int C = NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE;
        // Knob labels (above the y=200 knob row)
        labels->custom( 30.f, 172.f, 9.f, cLabel, C, 2.f, "K");
        labels->custom( 90.f, 172.f, 9.f, cLabel, C, 2.f, "RATE");
        labels->custom(150.f, 172.f, 9.f, cLabel, C, 2.f, "RANGE");
        // Button labels (above the y=250 button row)
        labels->custom( 60.f, 230.f, 8.f, cLabel, C, 1.f, "SEMI");
        labels->custom(120.f, 230.f, 8.f, cLabel, C, 1.f, "RESET");
        // Input labels (above the y=305 input jacks)
        labels->custom( 60.f, 288.f, 8.f, cLabel, C, 1.f, "IN");
        labels->custom(120.f, 288.f, 8.f, cLabel, C, 1.f, "TRIG");
        // Output label (above the y=358 output jack)
        labels->custom( 90.f, 335.f, 8.f, cBright, C, 2.f, "OUT");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(150, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(150, 365)));

        // Scope
        auto* scope = new CohortScope;
        scope->module = module;
        scope->box.pos  = Vec(8, 44);
        scope->box.size = Vec(164, 106);
        addChild(scope);

        // Knobs
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(30,  200), module, Cohort::K_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(90,  200), module, Cohort::RATE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(150, 200), module, Cohort::RANGE_PARAM));

        // Buttons
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            Vec(60,  250), module, Cohort::SEMI_PARAM, Cohort::SEMI_LIGHT));
        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(
            Vec(120, 250), module, Cohort::RESET_PARAM, Cohort::RESET_LIGHT));

        // Inputs
        addInput(createInputCentered<PJ301MPort>(
            Vec(60,  305), module, Cohort::SIGNAL_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 305), module, Cohort::TRIG_INPUT));

        // Output
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(90, 358), module, Cohort::OUT_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Cohort",
            {"Online k-means quantiser. Learns K cluster centres from",
             "incoming CV and snaps the output to the nearest one.",
             "A data-driven alternative to fixed Code cutpoints."},
            "Sample (data source), Code (uniform-cutpoint alternative)");
    }
};

Model* modelCohort = createModel<Cohort, CohortWidget>("Cohort");
