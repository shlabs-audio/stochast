#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
// Tab — contingency table / cross-tabulation of two categorical CV streams.
//
//   Each tick, pair (X, Y) — polyphonic min(channels) per tick — is read,
//   rounded to integer categories ∈ [1, K1] / [1, K2], and counted in a
//   K1 × K2 frequency matrix.
//
//   Statistics from the table:
//     χ² = Σ_{i,j} (O_{ij} − E_{ij})² / E_{ij}   with E_{ij} = R_i · C_j / N
//     Cramér's V = √(χ² / (N · min(K1−1, K2−1)))     ∈ [0, 1] effect size
//
//   The INDEP gate fires when V < 0.1 — the categories are essentially
//   independent (no association). For typical teaching settings this is more
//   intuitive than the p-value, but it has the same flavour: small V ≈ "we
//   would fail to reject the null".
//
//   Inputs are typically the CAT outputs of two Code modules, where 1 V = cat
//   1, 2 V = cat 2, etc. Values outside [1, K] are clamped.
// ============================================================================

struct Tab : Module {
    enum ParamId {
        K1_PARAM,
        K2_PARAM,
        LOW_PARAM,
        HIGH_PARAM,
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
        CHI2_OUTPUT,
        V_OUTPUT,
        INDEP_OUTPUT,
        N_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, INDEP_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxK             = 7;
    static constexpr int   kMaxNTotal        = 100000; // arbitrary cap; counts can grow large
    static constexpr float kInternalClockHz  = 60.f;
    static constexpr float kIndependentVThr  = 0.10f;

    int K1 = 5, K2 = 5;
    std::array<std::array<int, kMaxK>, kMaxK> table{};
    int totalN = 0;

    float chi2   = 0.f;
    float cramV  = 0.f;
    float pValue = 1.f;       // exact χ² upper-tail p-value
    int   dfChi2 = 0;         // (K1 − 1)(K2 − 1)

    float internalClockPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Tab() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(K1_PARAM, 2.f, (float)kMaxK, 5.f, "K1 (rows / X categories)");
        paramQuantities[K1_PARAM]->snapEnabled = true;
        configParam(K2_PARAM, 2.f, (float)kMaxK, 5.f, "K2 (cols / Y categories)");
        paramQuantities[K2_PARAM]->snapEnabled = true;
        configParam(LOW_PARAM,  -12.f, 12.f, 0.5f, "Lower bound for category mapping");
        configParam(HIGH_PARAM, -12.f, 12.f, 7.5f, "Upper bound for category mapping");
        configButton(SHUFFLE_PARAM, "Clear table");
        configInput(CLOCK_INPUT, "Clock (free-runs at 60 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear table)");
        configInput(X_INPUT, "X categorical CV (polyphonic OK)");
        configInput(Y_INPUT, "Y categorical CV (polyphonic OK)");
        configOutput(CHI2_OUTPUT, "χ² statistic");
        configOutput(V_OUTPUT, "Cramér's V (0..10 V = 0..1)");
        configOutput(INDEP_OUTPUT, "Independence gate (V < 0.1)");
        configOutput(N_OUTPUT, "Cumulative sample size N (scaled by 100)");
        clearTable();
    }

    void clearTable() {
        for (auto& row : table) row.fill(0);
        totalN = 0;
        chi2 = 0.f;
        cramV = 0.f;
        pValue = 1.f;
        dfChi2 = 0;
    }

    // Series expansion for the regularized lower incomplete gamma P(a, x),
    // used when x < a + 1.
    static double gserP(double a, double x) {
        const int MAXIT = 200;
        const double EPS = 3.0e-12;
        if (x <= 0.0) return 0.0;
        double ap = a;
        double sum = 1.0 / a;
        double del = sum;
        for (int n = 1; n <= MAXIT; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::fabs(del) < std::fabs(sum) * EPS) break;
        }
        return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
    }

    // Continued fraction for the regularized upper incomplete gamma Q(a, x),
    // used when x >= a + 1.
    static double gcfQ(double a, double x) {
        const int MAXIT = 200;
        const double EPS = 3.0e-12;
        const double FPMIN = 1.0e-300;
        double b = x + 1.0 - a;
        double c = 1.0 / FPMIN;
        double d = 1.0 / b;
        double h = d;
        for (int i = 1; i <= MAXIT; ++i) {
            double an = -(double)i * ((double)i - a);
            b += 2.0;
            d = an * d + b; if (std::fabs(d) < FPMIN) d = FPMIN;
            c = b + an / c; if (std::fabs(c) < FPMIN) c = FPMIN;
            d = 1.0 / d;
            double del = d * c;
            h *= del;
            if (std::fabs(del - 1.0) < EPS) break;
        }
        return std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
    }

    // Upper-tail χ² probability P(X² >= chi2 | df).
    static float chi2UpperTail(float chi2v, int df) {
        if (df <= 0 || chi2v <= 0.f) return 1.f;
        double a = 0.5 * df;
        double x = 0.5 * (double)chi2v;
        if (x < a + 1.0) return (float)(1.0 - gserP(a, x));
        else             return (float)gcfQ(a, x);
    }

    void onReset() override { clearTable(); }

    int   currentK1() { return clamp((int)std::round(params[K1_PARAM].getValue()), 2, kMaxK); }
    int   currentK2() { return clamp((int)std::round(params[K2_PARAM].getValue()), 2, kMaxK); }
    float currentLo() { return params[LOW_PARAM].getValue(); }
    float currentHi() { return params[HIGH_PARAM].getValue(); }

    int categoryOf(float v, float lo, float hi, int k) {
        if (hi - lo < 1e-6f) return 1;
        int byRound = clamp((int)std::round(v), 1, k);
        // Code modules emit 1V-per-category: exact integer volts in 1..k.
        // Detect that from the value itself, independent of the LOW/HIGH knobs
        // (whose K-tuned default HIGH would otherwise mis-classify categorical
        // input at K != 7 and silently mis-bin the table).
        int   r = (int)std::round(v);
        bool valueIsCategorical = (std::fabs(v - (float)r) < 0.05f && r >= 1 && r <= k);
        // Continuous-mode bucket as a fallback for raw CV streams spanning
        // [lo, hi] that aren't already categorical.
        float t = (v - lo) / (hi - lo);
        int byBucket = clamp(1 + (int)std::floor(t * k), 1, k);
        // Also honour an explicit 0..K+1 categorical LOW/HIGH window.
        bool windowIsCategorical = (lo > -0.5f && lo < 1.5f && hi > k - 0.5f && hi < k + 1.5f);
        return (valueIsCategorical || windowIsCategorical) ? byRound : byBucket;
    }

    void recomputeStats() {
        int k1 = currentK1(), k2 = currentK2();
        dfChi2 = (k1 - 1) * (k2 - 1);
        if (totalN <= 0) { chi2 = 0.f; cramV = 0.f; pValue = 1.f; return; }

        // Row and column sums
        std::array<int, kMaxK> rowSum{}, colSum{};
        for (int i = 0; i < k1; ++i)
            for (int j = 0; j < k2; ++j) {
                rowSum[i] += table[i][j];
                colSum[j] += table[i][j];
            }

        double sumChi = 0.0;
        for (int i = 0; i < k1; ++i)
            for (int j = 0; j < k2; ++j) {
                if (rowSum[i] == 0 || colSum[j] == 0) continue;
                double E = (double)rowSum[i] * colSum[j] / totalN;
                if (E < 1e-12) continue;
                double diff = table[i][j] - E;
                sumChi += diff * diff / E;
            }
        chi2 = (float)sumChi;
        pValue = chi2UpperTail(chi2, dfChi2);

        int minK = std::min(k1, k2) - 1;
        if (minK <= 0) { cramV = 0.f; return; }
        cramV = (float)std::sqrt(chi2 / ((double)totalN * minK));
        cramV = clamp(cramV, 0.f, 1.f);
    }

    void onClockTick() {
        int k1 = currentK1(), k2 = currentK2();
        if (k1 != K1 || k2 != K2) { K1 = k1; K2 = k2; clearTable(); }
        if (totalN >= kMaxNTotal) return;

        float lo = currentLo(), hi = currentHi();
        int xCh = inputs[X_INPUT].isConnected()
                  ? std::max(1, inputs[X_INPUT].getChannels()) : 1;
        int yCh = inputs[Y_INPUT].isConnected()
                  ? std::max(1, inputs[Y_INPUT].getChannels()) : 1;
        int ch = std::min(xCh, yCh);

        for (int c = 0; c < ch; ++c) {
            float xv = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage(c) : 0.f;
            float yv = inputs[Y_INPUT].isConnected() ? inputs[Y_INPUT].getVoltage(c) : 0.f;
            int xCat = categoryOf(xv, lo, hi, k1);
            int yCat = categoryOf(yv, lo, hi, k2);
            ++table[xCat - 1][yCat - 1];
            ++totalN;
            if (totalN >= kMaxNTotal) break;
        }

        recomputeStats();
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearTable();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearTable();
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
        if (tick) onClockTick();

        bool indep = (cramV < kIndependentVThr) && (totalN >= 4);

        outputs[CHI2_OUTPUT].setVoltage(clamp(chi2, 0.f, 12.f));
        outputs[V_OUTPUT].setVoltage(clamp(cramV, 0.f, 1.f) * 10.f);
        outputs[INDEP_OUTPUT].setVoltage(indep ? 10.f : 0.f);
        outputs[N_OUTPUT].setVoltage(clamp((float)totalN / 100.f, 0.f, 12.f));
        lights[INDEP_LIGHT].setBrightness(indep ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — K1 × K2 heatmap; cell shading = relative frequency. Cells
// also annotated with their observed counts when there's room.
// ============================================================================

struct TabView : LightWidget {
    Tab* module = nullptr;

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
                    "K1 × K2  HEATMAP", nullptr);
            return;
        }

        int k1 = module->currentK1();
        int k2 = module->currentK2();
        int N = module->totalN;

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 16.f;
        float gridW = box.size.x - 2 * pad - 16.f; // leave room for Y axis labels
        float gridH = box.size.y - 2 * pad - topStripH - botStripH - 12.f; // bottom X labels
        float x0 = pad + 12.f;
        float y0 = pad + topStripH;

        float cellW = gridW / k2;
        float cellH = gridH / k1;

        // Find max count for shading
        int maxC = 1;
        for (int i = 0; i < k1; ++i)
            for (int j = 0; j < k2; ++j)
                if (module->table[i][j] > maxC) maxC = module->table[i][j];

        // Cells (row 0 = top, increases downward — but conceptually row 1 =
        // category 1 of X; we draw cat 1 at top so larger categories appear
        // below, which is a typical convention for ordinal categories).
        for (int i = 0; i < k1; ++i) {
            for (int j = 0; j < k2; ++j) {
                float cx = x0 + cellW * j;
                float cy = y0 + cellH * i;
                int cnt = module->table[i][j];
                float t = (maxC > 0) ? (float)cnt / maxC : 0.f;
                // Color: dark navy → bright cyan as frequency grows
                int alpha = 30 + (int)(220 * t);
                nvgBeginPath(vg);
                nvgRect(vg, cx + 1, cy + 1, cellW - 2, cellH - 2);
                nvgFillColor(vg, nvgRGBA(80, 165, 220, alpha));
                nvgFill(vg);

                // Count label
                if (cellW > 20.f && cnt > 0) {
                    char buf[12];
                    std::snprintf(buf, sizeof(buf), "%d", cnt);
                    nvgFontSize(vg, 7.5f);
                    nvgFontFaceId(vg, APP->window->uiFont->handle);
                    nvgFillColor(vg, t > 0.4f ? nvgRGB(255, 255, 255) : nvgRGB(180, 190, 210));
                    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgText(vg, cx + cellW * 0.5f, cy + cellH * 0.5f, buf, nullptr);
                }
            }
        }

        // Grid lines
        nvgBeginPath(vg);
        for (int i = 0; i <= k1; ++i) {
            float yy = y0 + cellH * i;
            nvgMoveTo(vg, x0, yy);
            nvgLineTo(vg, x0 + gridW, yy);
        }
        for (int j = 0; j <= k2; ++j) {
            float xx = x0 + cellW * j;
            nvgMoveTo(vg, xx, y0);
            nvgLineTo(vg, xx, y0 + gridH);
        }
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 200));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Axis labels (X categories on bottom, Y categories on left)
        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        for (int j = 0; j < k2; ++j) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "y=%d", j + 1);
            nvgText(vg, x0 + cellW * (j + 0.5f), y0 + gridH + 2.f, buf, nullptr);
        }
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < k1; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "x=%d", i + 1);
            nvgText(vg, x0 - 2.f, y0 + cellH * (i + 0.5f), buf, nullptr);
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — dimensions + N + df
        char buf[96];
        nvgFontSize(vg, 7.5f);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "%d×%d  N=%d  df=%d",
                      k1, k2, N, module->dfChi2);
        nvgText(vg, 4, 3, buf, nullptr);

        // Top-right — χ², V, indep / assoc
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        bool indep = module->cramV < 0.10f;
        nvgFillColor(vg, indep ? nvgRGB(120, 200, 140) : nvgRGB(230, 175, 60));
        std::snprintf(buf, sizeof(buf), "χ²=%.1f  V=%.2f  %s",
                      module->chi2, module->cramV,
                      indep ? "indep" : "assoc");
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom strip — exact χ² p-value with significance stars
        nvgFontSize(vg, 8.f);
        const char* star = "";
        if      (module->pValue < 0.001f) star = " ***";
        else if (module->pValue < 0.01f)  star = " **";
        else if (module->pValue < 0.05f)  star = " *";
        bool reject = (module->pValue < 0.05f);
        nvgFillColor(vg, reject ? nvgRGB(245, 100, 100) : nvgRGB(180, 190, 210));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "p = %.4f%s", module->pValue, star);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct TabWidget : ModuleWidget {
    TabWidget(Tab* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Tab.svg")));
        addChild(new ModuleTitle("TAB", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "K1");  labels->k1(1, "K2");
        labels->k1(2, "LOW"); labels->k1(3, "HIGH");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "X");     labels->in(3, "Y");
        labels->outSection();
        labels->out(0, "χ²");    labels->out(1, "V");
        labels->out(2, "INDEP"); labels->out(3, "N");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new TabView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Tab::K1_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Tab::K2_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Tab::LOW_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Tab::HIGH_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Tab::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Tab::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Tab::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Tab::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Tab::X_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Tab::Y_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Tab::CHI2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Tab::V_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Tab::INDEP_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(222, 358), module, Tab::INDEP_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Tab::N_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Tab",
            {"Cross-tabulates two categorical CV streams into a K1×K2",
             "frequency matrix. Reports χ² statistic, Cramér's V effect",
             "size, and an INDEP gate when V < 0.1."},
            "Code x2 (typically), Tape (frozen joint distribution)");
    }
};

Model* modelTab = createModel<Tab, TabWidget>("Tab");
