#include "plugin.hpp"
#include <array>
#include <cmath>

// ============================================================================
// Factor — online principal-component analysis on six CV inputs.
//
// Implements Sanger's rule (Generalized Hebbian Algorithm) to extract the
// top three principal components in real time:
//
//   y_i  = w_i · x_centred
//   Δw_i = η · y_i · (x_centred − Σ_{j ≤ i} y_j · w_j)
//
// followed by an explicit re-normalisation of each w_i to unit length for
// numerical stability.
// ============================================================================

struct Factor : Module {
    static constexpr int N = 6;
    static constexpr int K = 3;
    static constexpr int TRAIL = 128;

    enum ParamId  { RATE_PARAM, SCALE_PARAM, CENTER_PARAM, FREEZE_PARAM, RESET_PARAM, NUM_PARAMS };
    enum InputId  { IN_1, IN_2, IN_3, IN_4, IN_5, IN_6, NUM_INPUTS };
    enum OutputId { PC1_OUTPUT, PC2_OUTPUT, PC3_OUTPUT, NUM_OUTPUTS };
    enum LightId  { CENTER_LIGHT, FREEZE_LIGHT, RESET_LIGHT, NUM_LIGHTS };

    std::array<std::array<float, N>, K> W{};
    std::array<float, N> mean{};

    // For visualization. Stored as the *clamped* PC values so the on-panel
    // scatter and trail can never map to canvas coordinates outside the viz
    // box, even when SCALE is cranked. The raw unclamped value is implicit —
    // detect saturation via `lastClipped` below.
    float lastY[K] = {0.f, 0.f, 0.f};
    float trail1[TRAIL] = {};
    float trail2[TRAIL] = {};
    int   trailIdx = 0;
    int   scopeFrameCounter = 0;
    bool  lastClipped = false;   // true when any |yk| would have exceeded ±10 V

    dsp::SchmittTrigger resetSchmitt;

    Factor() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RATE_PARAM,  0.f, 1.f, 0.35f, "Learning rate");
        configParam(SCALE_PARAM, 0.f, 1.f, 0.50f, "Output scale");
        configSwitch(CENTER_PARAM, 0.f, 1.f, 1.f, "Centre inputs", {"Off", "On"});
        configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze basis",  {"Off", "On"});
        configButton(RESET_PARAM, "Reset basis to identity");
        for (int n = 0; n < N; ++n)
            configInput(IN_1 + n, "Channel " + std::to_string(n + 1));
        configOutput(PC1_OUTPUT, "Principal component 1");
        configOutput(PC2_OUTPUT, "Principal component 2");
        configOutput(PC3_OUTPUT, "Principal component 3");
        resetBasis();
    }

    void resetBasis() {
        for (int k = 0; k < K; ++k) {
            W[k].fill(0.f);
            W[k][k] = 1.f;
        }
        mean.fill(0.f);
        for (int i = 0; i < TRAIL; ++i) trail1[i] = trail2[i] = 0.f;
        trailIdx = 0;
    }

    void onReset() override { resetBasis(); }

    static float dot(const std::array<float, N>& a, const std::array<float, N>& b) {
        float s = 0.f;
        for (int n = 0; n < N; ++n) s += a[n] * b[n];
        return s;
    }

    static void normalise(std::array<float, N>& v) {
        float n2 = 0.f;
        for (int i = 0; i < N; ++i) n2 += v[i] * v[i];
        float n = std::sqrt(n2);
        if (n > 1e-12f) {
            float inv = 1.f / n;
            for (int i = 0; i < N; ++i) v[i] *= inv;
        }
    }

    void process(const ProcessArgs& args) override {
        if (resetSchmitt.process(params[RESET_PARAM].getValue())) resetBasis();

        bool centre = params[CENTER_PARAM].getValue() > 0.5f;
        bool freeze = params[FREEZE_PARAM].getValue() > 0.5f;
        lights[CENTER_LIGHT].setBrightness(centre ? 1.f : 0.f);
        lights[FREEZE_LIGHT].setBrightness(freeze ? 1.f : 0.f);
        lights[RESET_LIGHT].setBrightness(params[RESET_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        std::array<float, N> x;
        for (int n = 0; n < N; ++n) x[n] = inputs[IN_1 + n].getVoltage();

        if (centre) {
            float meanA = 1.f - std::exp(-2.f * (float)M_PI * 0.5f / args.sampleRate);
            for (int n = 0; n < N; ++n) mean[n] += meanA * (x[n] - mean[n]);
        }

        std::array<float, N> xc;
        for (int n = 0; n < N; ++n) xc[n] = centre ? (x[n] - mean[n]) : x[n];

        std::array<float, K> y{};
        for (int k = 0; k < K; ++k) y[k] = dot(W[k], xc);

        if (!freeze) {
            float rateT = clamp(params[RATE_PARAM].getValue(), 0.f, 1.f);
            float eta = 1e-6f * std::pow(1000.f, rateT);
            std::array<float, N> residual = xc;
            for (int k = 0; k < K; ++k) {
                for (int n = 0; n < N; ++n) residual[n] -= y[k] * W[k][n];
                float coef = eta * y[k];
                for (int n = 0; n < N; ++n) W[k][n] += coef * residual[n];
                normalise(W[k]);
            }
        }

        float scaleT = clamp(params[SCALE_PARAM].getValue(), 0.f, 1.f);
        float scale = 0.1f * std::pow(100.f, scaleT);

        lastClipped = false;
        for (int k = 0; k < K; ++k) {
            float yk = dot(W[k], xc) * scale;
            if (std::fabs(yk) > 10.f) lastClipped = true;
            const float ykClamped = clamp(yk, -10.f, 10.f);
            // Store the clamped value for both the output port AND the on-
            // panel scatter/trail. The clamp guarantees the viz can never
            // map to coordinates outside the widget box, no matter how high
            // SCALE is cranked.
            lastY[k] = ykClamped;
            outputs[PC1_OUTPUT + k].setVoltage(ykClamped);
        }

        // Trail sampling at ~120 Hz
        int stride = std::max(1, (int)(args.sampleRate / 120.f));
        if (++scopeFrameCounter >= stride) {
            scopeFrameCounter = 0;
            trail1[trailIdx] = lastY[0];
            trail2[trailIdx] = lastY[1];
            trailIdx = (trailIdx + 1) % TRAIL;
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_t* warr = json_array();
        for (int k = 0; k < K; ++k) {
            json_t* row = json_array();
            for (int n = 0; n < N; ++n) json_array_append_new(row, json_real(W[k][n]));
            json_array_append_new(warr, row);
        }
        json_object_set_new(root, "W", warr);
        json_t* marr = json_array();
        for (int n = 0; n < N; ++n) json_array_append_new(marr, json_real(mean[n]));
        json_object_set_new(root, "mean", marr);
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* warr = json_object_get(root, "W");
        if (warr && json_is_array(warr)) {
            for (int k = 0; k < K && k < (int)json_array_size(warr); ++k) {
                json_t* row = json_array_get(warr, k);
                if (row && json_is_array(row)) {
                    for (int n = 0; n < N && n < (int)json_array_size(row); ++n) {
                        json_t* v = json_array_get(row, n);
                        if (json_is_number(v)) W[k][n] = (float)json_number_value(v);
                    }
                }
            }
        }
        json_t* marr = json_object_get(root, "mean");
        if (marr && json_is_array(marr)) {
            for (int n = 0; n < N && n < (int)json_array_size(marr); ++n) {
                json_t* v = json_array_get(marr, n);
                if (json_is_number(v)) mean[n] = (float)json_number_value(v);
            }
        }
    }
};

// ============================================================================
// XY scatter widget — PC1 (horiz) vs PC2 (vert), with fading trail.
// ============================================================================

struct FactorXY : LightWidget {
    Factor* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;
        const float W = box.size.x;
        const float H = box.size.y;

        // Clip the ENTIRE viz to the widget box. Belt and suspenders: the
        // trail/point are already clamped at storage, but a scissor here
        // guarantees that any future addition to the draw path also stays
        // inside the frame.
        nvgSave(vg);
        nvgScissor(vg, 0.f, 0.f, W, H);

        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        // Crosshair
        nvgBeginPath(vg);
        nvgMoveTo(vg, W/2, 0); nvgLineTo(vg, W/2, H);
        nvgMoveTo(vg, 0, H/2); nvgLineTo(vg, W, H/2);
        nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 130));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Axis labels
        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 3, 3, "PC2", nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        nvgText(vg, W - 3, H - 3, "PC1", nullptr);

        if (!module) {
            nvgFontSize(vg, 8.f);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 120));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, W/2, H/2, "XY", nullptr);
            nvgRestore(vg);
            return;
        }

        // Map ±10 V to canvas (matches output clamp range).
        // PC values can exceed ±10 V when SCALE is cranked — the output is
        // clamped to ±10 V at the port, but lastY / trail buffers store the
        // raw values, so without scissor clipping the trail line would draw
        // outside the widget box and overpaint adjacent panel art.
        auto px = [&](float v) { return (v + 10.f) / 20.f * W; };
        auto py = [&](float v) { return (1.f - (v + 10.f) / 20.f) * H; };

        // Trail with age-based alpha
        const int T = Factor::TRAIL;
        int wi = module->trailIdx;
        for (int i = 0; i < T - 1; ++i) {
            int idx1 = (wi + i) % T;
            int idx2 = (wi + i + 1) % T;
            float a = (float)i / T;          // 0 = oldest, ~1 = newest
            int alpha = (int)(40 + a * 200);
            nvgBeginPath(vg);
            nvgMoveTo(vg, px(module->trail1[idx1]), py(module->trail2[idx1]));
            nvgLineTo(vg, px(module->trail1[idx2]), py(module->trail2[idx2]));
            nvgStrokeColor(vg, nvgRGBA(232, 204, 89, alpha));
            nvgStrokeWidth(vg, 0.9f);
            nvgStroke(vg);
        }

        // Current point
        nvgBeginPath(vg);
        nvgCircle(vg, px(module->lastY[0]), py(module->lastY[1]), 2.5f);
        nvgFillColor(vg, nvgRGB(255, 220, 100));
        nvgFill(vg);

        // Saturation hint: when the latest PC value would clip outside the
        // viz, draw a small red ring around the current-point dot. lastY is
        // stored clamped, so the dot itself stays at the edge; without this
        // ring the saturation would be invisible. Teaching detail — students
        // see when they've cranked SCALE too far.
        if (module->lastClipped) {
            nvgBeginPath(vg);
            nvgCircle(vg, clamp(px(module->lastY[0]), 4.f, W - 4.f),
                          clamp(py(module->lastY[1]), 4.f, H - 4.f), 5.f);
            nvgStrokeColor(vg, nvgRGB(245, 90, 90));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
        }

        nvgRestore(vg);   // end of viz-wide scissor

        // Frame (drawn after restore so its stroke isn't clipped at edge px)
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, W - 1, H - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);
    }
};

// ============================================================================
// Loadings widget — bar chart of |W[k][n]| for k = 1..3 components,
// n = 1..6 channels. Three rows, each row groups of six bars.
// ============================================================================

struct FactorLoadings : LightWidget {
    Factor* module = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        const float W = box.size.x;
        const float H = box.size.y;
        const int K = Factor::K;
        const int N = Factor::N;

        // Clip the whole widget to its box. Loadings are normalised to unit
        // length so |W[k][n]| ≤ 1 and bars stay in-frame mathematically, but
        // the scissor protects against future additions and any transient
        // pre-normalise state from leaking onto the surrounding panel.
        nvgSave(vg);
        nvgScissor(vg, 0.f, 0.f, W, H);

        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);

        if (!module) {
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 120));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, W/2, H/2, "LOADINGS", nullptr);
            nvgRestore(vg);

            nvgBeginPath(vg);
            nvgRect(vg, 0.5f, 0.5f, W - 1, H - 1);
            nvgStrokeColor(vg, nvgRGB(43, 47, 66));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
            return;
        }

        const NVGcolor cols[3] = {
            nvgRGB(224, 196, 75),
            nvgRGB(79, 182, 207),
            nvgRGB(207, 95, 142)
        };
        const char* names[3] = { "PC1", "PC2", "PC3" };

        float rowH = H / 3.f;
        float labelW = 22.f;

        for (int k = 0; k < K; ++k) {
            float y0 = k * rowH;

            // Row label
            nvgFillColor(vg, cols[k]);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(vg, 4, y0 + rowH/2, names[k], nullptr);

            // Bars
            float plotX = labelW;
            float plotW = W - labelW - 4;
            float barW  = plotW / N;
            float zeroY = y0 + rowH * 0.55f;  // baseline (loadings can be ±)
            float maxBarH = rowH * 0.42f;

            for (int n = 0; n < N; ++n) {
                float w = module->W[k][n];
                float h = w * maxBarH;  // positive up, negative down
                float bx = plotX + n * barW + 1.f;
                float bw = barW - 2.f;
                float by = (h >= 0) ? (zeroY - h) : zeroY;
                float bh = std::abs(h);

                nvgBeginPath(vg);
                nvgRect(vg, bx, by, bw, std::max(1.f, bh));
                NVGcolor c = cols[k];
                c.a = (h >= 0) ? 0.85f : 0.55f;
                nvgFillColor(vg, c);
                nvgFill(vg);
            }

            // Zero line
            nvgBeginPath(vg);
            nvgMoveTo(vg, plotX, zeroY);
            nvgLineTo(vg, plotX + plotW, zeroY);
            nvgStrokeColor(vg, nvgRGBA(60, 68, 92, 200));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        nvgRestore(vg);   // end of widget-wide scissor

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

struct FactorWidget : ModuleWidget {
    FactorWidget(Factor* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Factor.svg")));
        addChild(new ModuleTitle("FACTOR", 270.f));

        // Factor uses a non-standard 18HP layout (XY + loadings sub-widgets,
        // 6-input row, 3 outputs). All labels placed via custom() at the
        // existing widget coordinates.
        auto* labels = new PanelLabels(270.f);
        const NVGcolor cLabel = nvgRGB(0x9a, 0xa0, 0xb4);
        const NVGcolor cSub   = nvgRGB(0x7c, 0x91, 0xd1);
        const NVGcolor cPC1   = nvgRGB(0xe0, 0xc4, 0x4b);
        const NVGcolor cPC2   = nvgRGB(0x4f, 0xb6, 0xcf);
        const NVGcolor cPC3   = nvgRGB(0xcf, 0x5f, 0x8e);
        const int C = NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE;
        // Sub-widget captions just below each scope (scope ends at y=184)
        labels->custom( 78.f, 200.f, 7.f, cSub,   C, 1.f, "PC1 ↔ PC2");
        labels->custom(209.f, 200.f, 7.f, cSub,   C, 1.f, "LOADINGS  |W|");
        // Input header + per-input names (jacks at y=258)
        labels->custom(135.f, 220.f, 9.f, cLabel, C, 2.f, "INPUTS");
        labels->custom( 35.f, 240.f, 8.f, cLabel, C, 0.f, "A1");
        labels->custom( 75.f, 240.f, 8.f, cLabel, C, 0.f, "A2");
        labels->custom(115.f, 240.f, 8.f, cLabel, C, 0.f, "A3");
        labels->custom(155.f, 240.f, 8.f, cLabel, C, 0.f, "A4");
        labels->custom(195.f, 240.f, 8.f, cLabel, C, 0.f, "A5");
        labels->custom(235.f, 240.f, 8.f, cLabel, C, 0.f, "A6");
        // Knob/button labels (widgets at y=320)
        labels->custom( 50.f, 298.f, 8.f, cLabel, C, 2.f, "RATE");
        labels->custom(105.f, 298.f, 8.f, cLabel, C, 2.f, "SCALE");
        labels->custom(160.f, 298.f, 8.f, cLabel, C, 2.f, "CENTER");
        labels->custom(200.f, 298.f, 8.f, cLabel, C, 2.f, "FREEZE");
        labels->custom(240.f, 298.f, 8.f, cLabel, C, 2.f, "RESET");
        // PC output labels — colored to match their trace colors
        labels->custom( 75.f, 362.f, 8.f, cPC1, C, 1.f, "PC1");
        labels->custom(135.f, 362.f, 8.f, cPC2, C, 1.f, "PC2");
        labels->custom(195.f, 362.f, 8.f, cPC3, C, 1.f, "PC3");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(240, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(240, 365)));

        // XY scope (left)
        auto* xy = new FactorXY;
        xy->module = module;
        xy->box.pos  = Vec(8, 44);
        xy->box.size = Vec(140, 140);
        addChild(xy);

        // Loadings (right)
        auto* ld = new FactorLoadings;
        ld->module = module;
        ld->box.pos  = Vec(156, 44);
        ld->box.size = Vec(106, 140);
        addChild(ld);

        // 6 inputs in one row
        const int xs[6] = {35, 75, 115, 155, 195, 235};
        for (int i = 0; i < 6; ++i) {
            addInput(createInputCentered<PJ301MPort>(
                Vec(xs[i], 258), module, Factor::IN_1 + i));
        }

        // Knobs and toggles
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(50,  320), module, Factor::RATE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(105, 320), module, Factor::SCALE_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            Vec(160, 320), module, Factor::CENTER_PARAM, Factor::CENTER_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            Vec(200, 320), module, Factor::FREEZE_PARAM, Factor::FREEZE_LIGHT));
        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(
            Vec(240, 320), module, Factor::RESET_PARAM, Factor::RESET_LIGHT));

        // Outputs
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(75,  345), module, Factor::PC1_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(135, 345), module, Factor::PC2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 345), module, Factor::PC3_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Factor",
            {"Real-time PCA on six CV inputs via Sanger's rule.",
             "Outputs the top three principal components —",
             "a factor-analysis mixer for modulation."},
            "Sample x6 (correlated noise), Tape (capture inputs)");
    }
};

Model* modelFactor = createModel<Factor, FactorWidget>("Factor");
