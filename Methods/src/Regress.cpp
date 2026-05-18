#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// ============================================================================
// Regress — online OLS regression of Y on X.
//
//   Each clock tick reads (X_i, Y_i) from the inputs and adds them to a ring
//   buffer. The line Y = α + β·X is fit by closed-form OLS over the buffer:
//       β = Σ(x − x̄)(y − ȳ) / Σ(x − x̄)²
//       α = ȳ − β·x̄
//   R² and MSE are computed alongside; MSE drives the confidence band on the
//   visualization.
//
//   Polyphonic X / Y add min(channels) pairs per tick — so a 16-voice Polis
//   output paired with another can drive 16 observations per clock.
//
//   Three modes (Snapshot / Running / Growing) mirror Frame.
// ============================================================================

struct Regress : Module {
    enum Mode { MODE_SNAP = 0, MODE_RUN, MODE_GROW, NUM_MODES };
    enum CI   { CI_80 = 0, CI_90, CI_95, CI_99, NUM_CIS };

    enum ParamId {
        MODE_PARAM,
        N_PARAM,
        CI_PARAM,
        SUB_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        X_INPUT,
        Y_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        BETA_OUTPUT,
        ALPHA_OUTPUT,
        R2_OUTPUT,
        RESID_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf          = 2048;
    static constexpr float kInternalClockHz = 30.f;

    struct Pair { float x, y; };
    std::array<Pair, kMaxBuf> pairs{};
    int writeIdx = 0;
    int totalCollected = 0;
    int subCounter = 0;
    int lastBatchSize = 0;
    bool snapshotComplete = false;
    float internalClockPhase = 0.f;

    // Cached regression statistics — recomputed at each clock tick
    float beta  = 0.f;
    float alpha = 0.f;
    float r2    = 0.f;
    float mse   = 0.f;
    float meanX = 0.f;
    float meanY = 0.f;
    float sumSqDevX = 0.f;

    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Regress() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 1.f, "Mode",
                     {"Snapshot", "Running", "Growing"});
        configParam(N_PARAM, 4.f, (float)kMaxBuf, 128.f, "Buffer size N");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configSwitch(CI_PARAM, 0.f, (float)(NUM_CIS - 1), 2.f, "Confidence level",
                     {"80%", "90%", "95%", "99%"});
        configParam(SUB_PARAM, 1.f, 16.f, 1.f, "Sub-sample (take every Kth clock)");
        paramQuantities[SUB_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Clear buffer + restart");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear buffer)");
        configInput(X_INPUT, "X (predictor, polyphonic OK)");
        configInput(Y_INPUT, "Y (response, polyphonic OK)");
        configOutput(BETA_OUTPUT, "Slope β");
        configOutput(ALPHA_OUTPUT, "Intercept α");
        configOutput(R2_OUTPUT, "R² (0..10 V)");
        configOutput(RESID_OUTPUT, "Current residual: Y - (α + β·X)");
        clearBuffer();
    }

    void clearBuffer() {
        for (int i = 0; i < kMaxBuf; ++i) pairs[i] = {0.f, 0.f};
        writeIdx = 0;
        totalCollected = 0;
        subCounter = 0;
        lastBatchSize = 0;
        snapshotComplete = false;
        beta = alpha = r2 = mse = 0.f;
        meanX = meanY = sumSqDevX = 0.f;
    }

    void onReset() override { clearBuffer(); }

    int currentMode() {
        return clamp((int)std::round(params[MODE_PARAM].getValue()), 0, NUM_MODES - 1);
    }
    int currentN() {
        return clamp((int)std::round(params[N_PARAM].getValue()), 4, kMaxBuf);
    }
    int currentSub() {
        return clamp((int)std::round(params[SUB_PARAM].getValue()), 1, 16);
    }
    float currentZ() {
        int ci = clamp((int)std::round(params[CI_PARAM].getValue()), 0, NUM_CIS - 1);
        switch (ci) {
            case CI_80: return 1.282f;
            case CI_90: return 1.645f;
            case CI_95: return 1.96f;
            case CI_99: return 2.576f;
        }
        return 1.96f;
    }

    int effectiveN() {
        int mode = currentMode();
        if (mode == MODE_RUN)  return std::min(currentN(),  totalCollected);
        if (mode == MODE_GROW) return std::min(kMaxBuf,     totalCollected);
        return std::min(currentN(), totalCollected);
    }

    void addPair(float x, float y) {
        int mode = currentMode();
        if (mode == MODE_SNAP && snapshotComplete) return;
        if (mode == MODE_GROW && totalCollected >= kMaxBuf) return;

        pairs[writeIdx] = {x, y};
        writeIdx = (writeIdx + 1) % kMaxBuf;
        if (totalCollected < kMaxBuf) ++totalCollected;

        if (mode == MODE_SNAP && totalCollected >= currentN()) {
            snapshotComplete = true;
        }
    }

    void takeClockTick() {
        ++subCounter;
        int sub = currentSub();
        if (subCounter < sub) return;
        subCounter = 0;

        int xCh = inputs[X_INPUT].isConnected()
                  ? std::max(1, inputs[X_INPUT].getChannels()) : 1;
        int yCh = inputs[Y_INPUT].isConnected()
                  ? std::max(1, inputs[Y_INPUT].getChannels()) : 1;
        int ch = std::min(xCh, yCh);

        int before = totalCollected;
        for (int c = 0; c < ch; ++c) {
            float x = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage(c) : 0.f;
            float y = inputs[Y_INPUT].isConnected() ? inputs[Y_INPUT].getVoltage(c) : 0.f;
            addPair(x, y);
        }
        lastBatchSize = totalCollected - before;
        if (lastBatchSize < 0) lastBatchSize = 0;

        computeRegression();
    }

    void computeRegression() {
        int n = effectiveN();
        if (n < 2) {
            beta = alpha = r2 = mse = 0.f;
            meanX = meanY = sumSqDevX = 0.f;
            return;
        }

        double sumX = 0, sumY = 0, sumXX = 0, sumYY = 0, sumXY = 0;
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            double x = pairs[idx].x;
            double y = pairs[idx].y;
            sumX += x;  sumY += y;
            sumXX += x * x;  sumYY += y * y;
            sumXY += x * y;
        }
        double mX = sumX / n;
        double mY = sumY / n;
        double ssX = sumXX - (double)n * mX * mX;   // Σ(x - x̄)²
        double ssY = sumYY - (double)n * mY * mY;
        double spXY = sumXY - (double)n * mX * mY;  // Σ(x - x̄)(y - ȳ)

        if (ssX > 1e-12) {
            beta = (float)(spXY / ssX);
        } else {
            beta = 0.f;
        }
        alpha = (float)(mY - (double)beta * mX);

        if (ssX > 1e-12 && ssY > 1e-12) {
            float r = (float)((spXY * spXY) / (ssX * ssY));
            r2 = clamp(r, 0.f, 1.f);
        } else {
            r2 = 0.f;
        }

        if (n > 2) {
            double ssr = std::max(0.0, ssY - (double)beta * (double)beta * ssX);
            mse = (float)(ssr / (n - 2));
        } else {
            mse = 0.f;
        }
        meanX = (float)mX;
        meanY = (float)mY;
        sumSqDevX = (float)ssX;
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearBuffer();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearBuffer();
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        bool tick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick = true;
        } else {
            internalClockPhase += args.sampleTime * kInternalClockHz;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick = true;
            }
        }
        if (tick) takeClockTick();

        outputs[BETA_OUTPUT].setVoltage(clamp(beta, -12.f, 12.f));
        outputs[ALPHA_OUTPUT].setVoltage(clamp(alpha, -12.f, 12.f));
        outputs[R2_OUTPUT].setVoltage(clamp(r2, 0.f, 1.f) * 10.f);

        // Live residual on first-channel X / Y
        float xLive = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage(0) : 0.f;
        float yLive = inputs[Y_INPUT].isConnected() ? inputs[Y_INPUT].getVoltage(0) : 0.f;
        float resid = yLive - (alpha + beta * xLive);
        outputs[RESID_OUTPUT].setVoltage(clamp(resid, -12.f, 12.f));
    }
};

// ============================================================================
// Visualization — live scatterplot of (X, Y) pairs with the fitted line and
// a confidence band whose width grows toward the X-extremes (the canonical
// "trumpet" silhouette of the prediction interval).
// ============================================================================

struct RegressView : LightWidget {
    Regress* module = nullptr;

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
                    "SCATTER  ·  Y ~ α + β·X", nullptr);
            return;
        }

        const int n = module->effectiveN();
        const int mode = module->currentMode();

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 12.f;
        float W_chart = box.size.x - 2 * pad;
        float H_chart = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        drawFrameLine(vg);
        drawHeader(vg, mode, n);

        if (n < 2) {
            nvgFontSize(vg, 8.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(110, 120, 140, 140));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "(need ≥ 2 points)", nullptr);
            return;
        }

        // Compute (X, Y) bounds
        float xMin =  std::numeric_limits<float>::infinity();
        float xMax = -std::numeric_limits<float>::infinity();
        float yMin =  std::numeric_limits<float>::infinity();
        float yMax = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < n; ++i) {
            int idx = (module->writeIdx - 1 - i + Regress::kMaxBuf) % Regress::kMaxBuf;
            const auto& p = module->pairs[idx];
            if (p.x < xMin) xMin = p.x;
            if (p.x > xMax) xMax = p.x;
            if (p.y < yMin) yMin = p.y;
            if (p.y > yMax) yMax = p.y;
        }
        if (xMax - xMin < 1e-4f) { xMax = xMin + 0.5f; xMin -= 0.5f; }
        if (yMax - yMin < 1e-4f) { yMax = yMin + 0.5f; yMin -= 0.5f; }
        float xPad = (xMax - xMin) * 0.05f;
        float yPad = (yMax - yMin) * 0.05f;
        xMin -= xPad; xMax += xPad;
        yMin -= yPad; yMax += yPad;

        auto mapX = [&](float xv) { return x0 + W_chart * (xv - xMin) / (xMax - xMin); };
        auto mapY = [&](float yv) { return y0 + H_chart * (1.f - (yv - yMin) / (yMax - yMin)); };

        // Clip every data-coordinate drawing (band, reference lines, scatter,
        // fitted line) to the chart sub-area. The band especially can extend
        // far above and below the visible Y range at the X extremes (the
        // trumpet shape grows as 1/√(n) · √(1/n + (x−x̄)² / Σ(x−x̄)²)), so
        // without scissor clipping it would paint outside the widget frame.
        nvgSave(vg);
        nvgIntersectScissor(vg, x0, y0, W_chart, H_chart);

        // Confidence band (drawn first, behind everything)
        if (n > 2 && module->mse > 0.f && module->sumSqDevX > 1e-9f) {
            const int kBandN = 40;
            std::array<float, kBandN> bx, bLo, bHi;
            float z = module->currentZ();
            for (int i = 0; i < kBandN; ++i) {
                float xv = xMin + (xMax - xMin) * i / (float)(kBandN - 1);
                float yHat = module->alpha + module->beta * xv;
                float se = std::sqrt(module->mse *
                                     (1.f / n + (xv - module->meanX) * (xv - module->meanX) / module->sumSqDevX));
                bx[i] = xv;
                bLo[i] = yHat - z * se;
                bHi[i] = yHat + z * se;
            }
            nvgBeginPath(vg);
            nvgMoveTo(vg, mapX(bx[0]), mapY(bLo[0]));
            for (int i = 1; i < kBandN; ++i) nvgLineTo(vg, mapX(bx[i]), mapY(bLo[i]));
            for (int i = kBandN - 1; i >= 0; --i) nvgLineTo(vg, mapX(bx[i]), mapY(bHi[i]));
            nvgClosePath(vg);
            nvgFillColor(vg, nvgRGBA(230, 175, 60, 40));
            nvgFill(vg);
        }

        // x = 0 and y = 0 reference lines (faint, only if visible)
        if (xMin <= 0.f && xMax >= 0.f) {
            float zx = mapX(0.f);
            nvgBeginPath(vg);
            nvgMoveTo(vg, zx, y0);
            nvgLineTo(vg, zx, y0 + H_chart);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 70));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }
        if (yMin <= 0.f && yMax >= 0.f) {
            float zy = mapY(0.f);
            nvgBeginPath(vg);
            nvgMoveTo(vg, x0, zy);
            nvgLineTo(vg, x0 + W_chart, zy);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 70));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        // Scatter — most recent batch highlighted
        int displayMax = 256;
        int step = std::max(1, n / displayMax);
        for (int i = n - 1; i >= 0; i -= step) {
            int idx = (module->writeIdx - 1 - i + Regress::kMaxBuf) % Regress::kMaxBuf;
            const auto& p = module->pairs[idx];
            float px = mapX(p.x);
            float py = mapY(p.y);
            float age = (float)i / std::max(1, n);
            bool fresh = (i < module->lastBatchSize);

            nvgBeginPath(vg);
            nvgCircle(vg, px, py, fresh ? 2.6f : 1.6f);
            if (fresh) {
                nvgFillColor(vg, nvgRGB(245, 90, 90));
            } else {
                int a = 50 + (int)(140 * (1.f - age));
                nvgFillColor(vg, nvgRGBA(80, 165, 220, a));
            }
            nvgFill(vg);
        }

        // Fitted line — extends across visible X range
        {
            float xa = xMin, ya = module->alpha + module->beta * xa;
            float xb = xMax, yb = module->alpha + module->beta * xb;
            nvgBeginPath(vg);
            nvgMoveTo(vg, mapX(xa), mapY(ya));
            nvgLineTo(vg, mapX(xb), mapY(yb));
            nvgStrokeColor(vg, nvgRGB(230, 175, 60));
            nvgStrokeWidth(vg, 1.4f);
            nvgStroke(vg);
        }

        nvgRestore(vg);   // end of chart-area scissor

        // Bottom-left: x range. Bottom-right: y range.
        char buf[64];
        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "x:[%.2f, %.2f]", xMin, xMax);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "y:[%.2f, %.2f]", yMin, yMax);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }

    void drawHeader(NVGcontext* vg, int mode, int n) {
        const char* modeNames[Regress::NUM_MODES] = {"SNAPSHOT", "RUNNING", "GROWING"};
        const char* ciNames[Regress::NUM_CIS]     = {"80%", "90%", "95%", "99%"};
        int ci = clamp((int)std::round(module->params[Regress::CI_PARAM].getValue()),
                       0, Regress::NUM_CIS - 1);

        char buf[96];
        int N = module->currentN();
        if (mode == Regress::MODE_SNAP && !module->snapshotComplete) {
            std::snprintf(buf, sizeof(buf), "%s  %d/%d  %sCI",
                          modeNames[mode], n, N, ciNames[ci]);
        } else {
            std::snprintf(buf, sizeof(buf), "%s  n=%d  %sCI",
                          modeNames[mode], n, ciNames[ci]);
        }
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, buf, nullptr);

        if (n >= 2) {
            // Big β readout on top right
            nvgFontSize(vg, 18.f);
            nvgFillColor(vg, nvgRGBA(220, 230, 245, 240));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgTextLetterSpacing(vg, 0.5f);
            std::snprintf(buf, sizeof(buf), "%.3f", module->beta);
            nvgText(vg, box.size.x - 4, 12, buf, nullptr);
            nvgText(vg, box.size.x - 4 + 0.4f, 12, buf, nullptr);  // faux-bold
            nvgTextLetterSpacing(vg, 0.f);
            // Label and subordinate stats
            nvgFontSize(vg, 7.f);
            nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
            nvgText(vg, box.size.x - 4, 3, "β  (slope)", nullptr);
            std::snprintf(buf, sizeof(buf), "α=%.2f  R²=%.2f", module->alpha, module->r2);
            nvgText(vg, box.size.x - 4, 33, buf, nullptr);
        }
    }

    void drawFrameLine(NVGcontext* vg) {
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

struct RegressWidget : ModuleWidget {
    RegressWidget(Regress* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Regress.svg")));
        addChild(new ModuleTitle("REGRESS", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE"); labels->k1(1, "N");
        labels->k1(2, "CI");   labels->k1(3, "SUB");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "X");     labels->in(3, "Y");
        labels->outSection();
        labels->out(0, "β");    labels->out(1, "α");
        labels->out(2, "R²");   labels->out(3, "RESID");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new RegressView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Regress::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Regress::N_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Regress::CI_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Regress::SUB_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Regress::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Regress::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Regress::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Regress::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Regress::X_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Regress::Y_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Regress::BETA_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Regress::ALPHA_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Regress::R2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Regress::RESID_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Regress",
            {"Online OLS regression on (X, Y) pairs sampled at a clock.",
             "Outputs slope β, intercept α, R², and current residual.",
             "Live scatterplot with fitted line and confidence band."},
            "Sample x2 (paired noise), Tape (record both streams)");
    }
};

Model* modelRegress = createModel<Regress, RegressWidget>("Regress");
