#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// ============================================================================
// Lag — time-series autocorrelation analysis.
//
//   Buffers a signal (with the usual Snapshot / Running / Growing modes) and
//   estimates the autocorrelation function ρ(k) at lags k = 1..K via
//
//       ρ(k) = Σ_{t=k}^{n-1} (x_t − x̄)(x_{t-k} − x̄) / Σ_t (x_t − x̄)²
//
//   The AR(1) coefficient φ = ρ(1) is exposed as a dedicated CV output, and
//   residual variance σ²ε = σ²(1 − φ²) is computed alongside.
//
//   DETREND options:
//     OFF      — use raw signal
//     MEAN     — subtract sample mean (default; standard ACF estimator)
//     LINEAR   — subtract least-squares linear trend (handles drift)
//
//   The ACF bar plot shows ±1.96/√n Bartlett confidence bands. A bar that
//   pokes outside the band is significantly autocorrelated at α ≈ 0.05.
//   When all displayed lags lie within the bands, the WHITE gate fires —
//   the data is consistent with white noise at the chosen K.
// ============================================================================

struct Lag : Module {
    enum Mode     { MODE_SNAP = 0, MODE_RUN, MODE_GROW, NUM_MODES };
    enum Detrend  { DETREND_OFF = 0, DETREND_MEAN, DETREND_LINEAR, NUM_DETRENDS };

    enum ParamId {
        MODE_PARAM,
        N_PARAM,
        LAGS_PARAM,
        DETREND_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        SIG_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        PHI_OUTPUT,
        ACF_OUTPUT,        // polyphonic ρ(1)..ρ(K)
        RESID_VAR_OUTPUT,
        WHITE_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, WHITE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf          = 2048;
    static constexpr int   kMaxLags         = 16;
    static constexpr float kInternalClockHz = 30.f;

    std::array<float, kMaxBuf> samples{};
    int writeIdx = 0;
    int totalCollected = 0;
    bool snapshotComplete = false;
    float internalClockPhase = 0.f;

    // Cached results
    std::array<float, kMaxLags> acf{};
    float phi = 0.f;
    float seriesVar = 0.f;
    float residVar = 0.f;
    bool isWhite = false;
    float ciBand = 0.f; // 1.96 / sqrt(n) — drawn in the viz

    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Lag() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 1.f, "Mode",
                     {"Snapshot", "Running", "Growing"});
        configParam(N_PARAM, 8.f, (float)kMaxBuf, 128.f, "Buffer size N");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configParam(LAGS_PARAM, 1.f, (float)kMaxLags, 8.f, "Lags K shown in ACF");
        paramQuantities[LAGS_PARAM]->snapEnabled = true;
        configSwitch(DETREND_PARAM, 0.f, (float)(NUM_DETRENDS - 1), 1.f, "Detrend",
                     {"Off", "Mean", "Linear"});
        configButton(SHUFFLE_PARAM, "Clear buffer");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear buffer)");
        configInput(SIG_INPUT, "Signal (polyphonic OK)");
        configOutput(PHI_OUTPUT, "AR(1) coefficient φ = ρ(1) (±10 V)");
        configOutput(ACF_OUTPUT, "ACF ρ(1)..ρ(K) (polyphonic, ±10 V)");
        configOutput(RESID_VAR_OUTPUT, "AR(1) residual variance σ²ε");
        configOutput(WHITE_OUTPUT, "Gate: all displayed lags within Bartlett band");
        clearBuffer();
    }

    void clearBuffer() {
        samples.fill(0.f);
        writeIdx = 0;
        totalCollected = 0;
        snapshotComplete = false;
        acf.fill(0.f);
        phi = 0.f;
        seriesVar = 0.f;
        residVar = 0.f;
        isWhite = false;
        ciBand = 0.f;
    }

    void onReset() override { clearBuffer(); }

    int currentMode()    { return clamp((int)std::round(params[MODE_PARAM].getValue()),    0, NUM_MODES - 1); }
    int currentN()       { return clamp((int)std::round(params[N_PARAM].getValue()),       8, kMaxBuf); }
    int currentLags()    { return clamp((int)std::round(params[LAGS_PARAM].getValue()),    1, kMaxLags); }
    int currentDetrend() { return clamp((int)std::round(params[DETREND_PARAM].getValue()), 0, NUM_DETRENDS - 1); }

    int effectiveN() {
        int mode = currentMode();
        if (mode == MODE_RUN)  return std::min(currentN(),  totalCollected);
        if (mode == MODE_GROW) return std::min(kMaxBuf,     totalCollected);
        return std::min(currentN(), totalCollected);
    }

    void addSample(float v) {
        int mode = currentMode();
        if (mode == MODE_SNAP && snapshotComplete) return;
        if (mode == MODE_GROW && totalCollected >= kMaxBuf) return;
        samples[writeIdx] = v;
        writeIdx = (writeIdx + 1) % kMaxBuf;
        if (totalCollected < kMaxBuf) ++totalCollected;
        if (mode == MODE_SNAP && totalCollected >= currentN()) snapshotComplete = true;
    }

    void computeAutocorrelations() {
        int n = effectiveN();
        if (n < 4) { acf.fill(0.f); phi = 0.f; seriesVar = 0.f; residVar = 0.f; isWhite = false; ciBand = 0.f; return; }

        // Gather buffer in chronological order (oldest first)
        std::vector<float> data(n);
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - n + i + kMaxBuf) % kMaxBuf;
            data[i] = samples[idx];
        }

        // Detrend
        int det = currentDetrend();
        if (det >= DETREND_MEAN) {
            double m = 0;
            for (float x : data) m += x;
            m /= n;
            for (auto& x : data) x -= (float)m;
        }
        if (det == DETREND_LINEAR) {
            // Mean-zero already; fit slope with x-index centred
            double sumXY = 0, sumXX = 0;
            for (int i = 0; i < n; ++i) {
                double xi = i - (n - 1) / 2.0;
                sumXY += xi * data[i];
                sumXX += xi * xi;
            }
            float slope = (sumXX > 1e-9) ? (float)(sumXY / sumXX) : 0.f;
            for (int i = 0; i < n; ++i) data[i] -= slope * (i - (n - 1) / 2.f);
        }

        // Series variance
        double s2 = 0;
        for (float x : data) s2 += (double)x * x;
        s2 /= n;
        seriesVar = (float)s2;
        if (s2 < 1e-12) { acf.fill(0.f); phi = 0.f; residVar = 0.f; isWhite = true; ciBand = 1.96f / std::sqrt((float)n); return; }

        // Autocorrelations at lags 1..K
        int K = currentLags();
        if (K > n / 2) K = std::max(1, n / 2);
        for (int k = 1; k <= K; ++k) {
            double acov = 0;
            for (int t = k; t < n; ++t) acov += (double)data[t] * data[t - k];
            acov /= n; // biased estimator (standard)
            acf[k - 1] = (float)(acov / s2);
        }
        for (int k = K; k < kMaxLags; ++k) acf[k] = 0.f;

        phi = acf[0];
        residVar = (float)(s2 * (1.0 - (double)phi * phi));
        ciBand = 1.96f / std::sqrt((float)n);

        bool allWithin = true;
        for (int k = 0; k < K; ++k) {
            if (std::fabs(acf[k]) > ciBand) { allWithin = false; break; }
        }
        isWhite = allWithin;
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
        if (tick) {
            int ch = inputs[SIG_INPUT].isConnected()
                     ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
            if (!inputs[SIG_INPUT].isConnected()) {
                addSample(0.f);
            } else {
                for (int c = 0; c < ch; ++c) addSample(inputs[SIG_INPUT].getVoltage(c));
            }
            computeAutocorrelations();
        }

        // Outputs
        outputs[PHI_OUTPUT].setVoltage(clamp(phi, -1.f, 1.f) * 10.f);

        int K = currentLags();
        outputs[ACF_OUTPUT].setChannels(K);
        for (int k = 0; k < K; ++k) {
            outputs[ACF_OUTPUT].setVoltage(clamp(acf[k], -1.f, 1.f) * 10.f, k);
        }

        outputs[RESID_VAR_OUTPUT].setVoltage(clamp(residVar, 0.f, 12.f));
        outputs[WHITE_OUTPUT].setVoltage(isWhite && totalCollected >= 4 ? 10.f : 0.f);
        lights[WHITE_LIGHT].setBrightness(isWhite && totalCollected >= 4 ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — ACF bar plot at lags 1..K with Bartlett ±1.96/√n bands.
// ============================================================================

struct LagView : LightWidget {
    Lag* module = nullptr;

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
                    "ACF  ·  LAGS  ·  CI", nullptr);
            return;
        }

        const int K = module->currentLags();
        const int n = module->effectiveN();

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 14.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        auto mapY = [&](float r) {
            return y0 + H * (1.f - (clamp(r, -1.f, 1.f) + 1.f) / 2.f);
        };

        // Zero line at ρ = 0
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, mapY(0.f));
        nvgLineTo(vg, x0 + W, mapY(0.f));
        nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 160));
        nvgStrokeWidth(vg, 0.6f);
        nvgStroke(vg);

        // ρ = ±1 reference (faint)
        for (float r : {1.f, -1.f}) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x0, mapY(r));
            nvgLineTo(vg, x0 + W, mapY(r));
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 90));
            nvgStrokeWidth(vg, 0.4f);
            nvgStroke(vg);
        }

        // Bartlett CI bands (dashed)
        if (module->ciBand > 0.f) {
            for (float r : {module->ciBand, -module->ciBand}) {
                nvgBeginPath(vg);
                float dash = 4.f;
                float y = mapY(r);
                for (float xx = x0; xx < x0 + W; xx += dash * 2.f) {
                    nvgMoveTo(vg, xx, y);
                    nvgLineTo(vg, std::min(xx + dash, x0 + W), y);
                }
                nvgStrokeColor(vg, nvgRGBA(230, 175, 60, 150));
                nvgStrokeWidth(vg, 0.7f);
                nvgStroke(vg);
            }
            // Shaded CI region (very faint)
            float yHi = mapY(module->ciBand);
            float yLo = mapY(-module->ciBand);
            nvgBeginPath(vg);
            nvgRect(vg, x0, yHi, W, yLo - yHi);
            nvgFillColor(vg, nvgRGBA(230, 175, 60, 25));
            nvgFill(vg);
        }

        // Bars
        if (K > 0 && n >= 4) {
            float slot = W / K;
            for (int k = 0; k < K; ++k) {
                float cx = x0 + slot * (k + 0.5f);
                float r = clamp(module->acf[k], -1.f, 1.f);
                float by = mapY(r);
                float bw = std::max(2.f, slot * 0.6f);
                float bx = cx - bw * 0.5f;
                // Bar from 0 to r
                float top = std::min(by, mapY(0.f));
                float h = std::fabs(by - mapY(0.f));
                bool significant = std::fabs(r) > module->ciBand;
                nvgBeginPath(vg);
                nvgRect(vg, bx, top, bw, h);
                nvgFillColor(vg, significant ? nvgRGBA(80, 165, 220, 230)
                                             : nvgRGBA(110, 140, 175, 150));
                nvgFill(vg);

                // Cap at ρ value
                nvgBeginPath(vg);
                nvgMoveTo(vg, bx, by);
                nvgLineTo(vg, bx + bw, by);
                nvgStrokeColor(vg, significant ? nvgRGB(120, 200, 245) : nvgRGB(140, 160, 190));
                nvgStrokeWidth(vg, 1.f);
                nvgStroke(vg);
            }
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Header
        const char* detNames[Lag::NUM_DETRENDS] = {"raw", "mean", "linear"};
        const char* modeShort[Lag::NUM_MODES] = {"SNAP", "RUN", "GROW"};
        char buf[80];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "ACF  %s  n=%d  K=%d  detrend:%s",
                      modeShort[module->currentMode()], n, K, detNames[module->currentDetrend()]);
        nvgText(vg, 4, 3, buf, nullptr);

        if (n >= 4) {
            std::snprintf(buf, sizeof(buf), "φ=%.2f", module->phi);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgText(vg, box.size.x - 4, 3, buf, nullptr);
        }

        // Bottom strip: lag numbers + white/correlated indicator
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        if (K <= 16) {
            float slot = W / K;
            for (int k = 0; k < K; ++k) {
                float cx = x0 + slot * (k + 0.5f);
                std::snprintf(buf, sizeof(buf), "%d", k + 1);
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
                nvgText(vg, cx, box.size.y - 3, buf, nullptr);
            }
        }
        // White / correlated indicator on left
        if (n >= 4) {
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
            if (module->isWhite) {
                nvgFillColor(vg, nvgRGB(120, 200, 140));
                nvgText(vg, 4, box.size.y - 3, "white", nullptr);
            } else {
                nvgFillColor(vg, nvgRGB(230, 175, 60));
                nvgText(vg, 4, box.size.y - 3, "autocorr", nullptr);
            }
        }
    }
};

// ============================================================================
// Widget
// ============================================================================

struct LagWidget : ModuleWidget {
    LagWidget(Lag* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Lag.svg")));
        addChild(new ModuleTitle("LAG", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE"); labels->k1(1, "N");
        labels->k1(2, "LAGS"); labels->k1(3, "DETREND");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");
        labels->outSection();
        labels->out(0, "φ");   labels->out(1, "ACF");
        labels->out(2, "σ²ε"); labels->out(3, "WHITE");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new LagView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Lag::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Lag::N_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Lag::LAGS_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Lag::DETREND_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Lag::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Lag::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Lag::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Lag::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Lag::SIG_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Lag::PHI_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Lag::ACF_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Lag::RESID_VAR_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Lag::WHITE_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(243, 358), module, Lag::WHITE_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Lag",
            {"Estimates the autocorrelation function ρ(k) at lags 1..K,",
             "plus AR(1) coefficient φ and residual variance σ²ε.",
             "ACF bar plot with Bartlett ±1.96/√n significance bands."},
            "Strata (analyse residuals), Tape (replay the same signal)");
    }
};

Model* modelLag = createModel<Lag, LagWidget>("Lag");
