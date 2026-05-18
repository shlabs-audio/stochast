#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
// Test — two-tailed hypothesis testing.
//
//   ONE-SAMPLE: H₀: μ = μ₀  (μ₀ set by H0 knob).
//     t = (x̄ − μ₀) / (s/√n),    df = n − 1
//     d = (x̄ − μ₀) / s          (Cohen's d, standardized effect size)
//
//   TWO-SAMPLE (Welch's, unequal variances allowed):
//     t = (x̄₁ − x̄₂ − δ₀) / √(s₁²/n₁ + s₂²/n₂)
//     d = (x̄₁ − x̄₂) / s_pooled
//
//   p-value uses the exact two-tailed Student's t survival function, computed
//   via the regularized incomplete beta function (Lentz continued fraction).
//   Accurate for any df, and the null-distribution visualization uses the
//   matching t-PDF so the fattening of the tails at low df is visible.
//
//   Polyphonic SIG / SIG2 add one observation per channel per tick.
//
//   REJECT gate fires whenever |t| exceeds the critical value for the
//   selected α (0.01 / 0.05 / 0.10).
// ============================================================================

struct Test : Module {
    enum Mode      { MODE_SNAP = 0, MODE_RUN, MODE_GROW, NUM_MODES };
    enum TestKind  { TEST_ONE = 0, TEST_TWO, NUM_TESTS };
    enum AlphaKind { ALPHA_01 = 0, ALPHA_05, ALPHA_10, NUM_ALPHAS };

    enum ParamId {
        MODE_PARAM,
        N_PARAM,
        ALPHA_PARAM,
        H0_PARAM,
        TEST_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        SIG_INPUT,
        SIG2_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        T_OUTPUT,
        P_OUTPUT,
        REJECT_OUTPUT,
        EFFECT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, REJECT_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf          = 2048;
    static constexpr float kInternalClockHz = 30.f;

    std::array<float, kMaxBuf> buf1{}, buf2{};
    int writeIdx1 = 0, writeIdx2 = 0;
    int total1 = 0, total2 = 0;
    bool snapshotComplete = false;
    float internalClockPhase = 0.f;

    // Cached stats
    float tStat = 0.f;
    float pValue = 1.f;
    float effectSize = 0.f;
    float diff = 0.f;
    int   dfCached = 0;

    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Test() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 1.f, "Mode",
                     {"Snapshot", "Running", "Growing"});
        configParam(N_PARAM, 4.f, (float)kMaxBuf, 128.f, "Buffer size N");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configSwitch(ALPHA_PARAM, 0.f, (float)(NUM_ALPHAS - 1), 1.f, "Significance α",
                     {"0.01", "0.05", "0.10"});
        configParam(H0_PARAM, -5.f, 5.f, 0.f, "H₀ value (μ₀ one-sample, δ₀ two-sample)");
        configSwitch(TEST_PARAM, 0.f, (float)(NUM_TESTS - 1), 0.f, "Test kind",
                     {"One-sample", "Two-sample (Welch)"});
        configButton(SHUFFLE_PARAM, "Clear buffers + restart");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear buffers)");
        configInput(SIG_INPUT, "Signal (group 1, polyphonic OK)");
        configInput(SIG2_INPUT, "Signal 2 (group 2, two-sample only)");
        configOutput(T_OUTPUT, "t-statistic");
        configOutput(P_OUTPUT, "p-value (0..10 V = 0..1)");
        configOutput(REJECT_OUTPUT, "Reject-H₀ gate (|t| > critical)");
        configOutput(EFFECT_OUTPUT, "Cohen's d effect size");
        clearBuffers();
    }

    void clearBuffers() {
        buf1.fill(0.f); buf2.fill(0.f);
        writeIdx1 = writeIdx2 = 0;
        total1 = total2 = 0;
        snapshotComplete = false;
        tStat = 0.f; pValue = 1.f; effectSize = 0.f; diff = 0.f;
        dfCached = 0;
    }

    void onReset() override { clearBuffers(); }

    int   currentMode()  { return clamp((int)std::round(params[MODE_PARAM].getValue()),  0, NUM_MODES - 1); }
    int   currentTest()  { return clamp((int)std::round(params[TEST_PARAM].getValue()),  0, NUM_TESTS - 1); }
    int   currentAlpha() { return clamp((int)std::round(params[ALPHA_PARAM].getValue()), 0, NUM_ALPHAS - 1); }
    int   currentN()     { return clamp((int)std::round(params[N_PARAM].getValue()),     4, kMaxBuf); }
    float currentH0()    { return clamp(params[H0_PARAM].getValue(), -5.f, 5.f); }

    float criticalValue() {
        switch (currentAlpha()) {
            case ALPHA_01: return 2.576f;
            case ALPHA_05: return 1.96f;
            case ALPHA_10: return 1.645f;
        }
        return 1.96f;
    }
    float alphaValue() {
        switch (currentAlpha()) {
            case ALPHA_01: return 0.01f;
            case ALPHA_05: return 0.05f;
            case ALPHA_10: return 0.10f;
        }
        return 0.05f;
    }

    int effectiveN(int total) {
        int mode = currentMode();
        if (mode == MODE_RUN)  return std::min(currentN(),  total);
        if (mode == MODE_GROW) return std::min(kMaxBuf,     total);
        return std::min(currentN(), total);
    }

    void addToBuf(std::array<float, kMaxBuf>& buf, int& writeIdx, int& total, float v) {
        int mode = currentMode();
        if (mode == MODE_SNAP && snapshotComplete) return;
        if (mode == MODE_GROW && total >= kMaxBuf) return;
        buf[writeIdx] = v;
        writeIdx = (writeIdx + 1) % kMaxBuf;
        if (total < kMaxBuf) ++total;
    }

    void takeClockTick() {
        int test = currentTest();
        int ch1 = inputs[SIG_INPUT].isConnected()
                  ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
        for (int c = 0; c < ch1; ++c) {
            float v = inputs[SIG_INPUT].isConnected() ? inputs[SIG_INPUT].getVoltage(c) : 0.f;
            addToBuf(buf1, writeIdx1, total1, v);
        }
        if (test == TEST_TWO) {
            int ch2 = inputs[SIG2_INPUT].isConnected()
                      ? std::max(1, inputs[SIG2_INPUT].getChannels()) : 1;
            for (int c = 0; c < ch2; ++c) {
                float v = inputs[SIG2_INPUT].isConnected() ? inputs[SIG2_INPUT].getVoltage(c) : 0.f;
                addToBuf(buf2, writeIdx2, total2, v);
            }
        }

        // For SNAP mode, completion is when buf1 reaches N (and buf2 too for two-sample)
        int mode = currentMode();
        if (mode == MODE_SNAP) {
            int N = currentN();
            bool ok1 = total1 >= N;
            bool ok2 = (test == TEST_ONE) ? true : (total2 >= N);
            if (ok1 && ok2) snapshotComplete = true;
        }

        computeStats();
    }

    void meanSD(const std::array<float, kMaxBuf>& buf, int writeIdx, int n,
                float& mean, float& sd) {
        if (n <= 0) { mean = 0.f; sd = 0.f; return; }
        double m = 0.0;
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            m += buf[idx];
        }
        mean = (float)(m / n);
        if (n < 2) { sd = 0.f; return; }
        double ss = 0.0;
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            double d = buf[idx] - mean;
            ss += d * d;
        }
        sd = (float)std::sqrt(ss / (n - 1));
    }

    static float gaussCDF(float z) {
        return 0.5f * (1.f + std::erf(z / std::sqrt(2.f)));
    }

    // Lentz-modified continued fraction for the incomplete beta function.
    // Returns the CF that, multiplied by a normalisation prefactor, gives the
    // regularized incomplete beta I_x(a,b).
    static double betacf(double a, double b, double x) {
        const int MAXIT = 200;
        const double EPS = 3.0e-12;
        const double FPMIN = 1.0e-300;
        double qab = a + b, qap = a + 1.0, qam = a - 1.0;
        double c = 1.0;
        double d = 1.0 - qab * x / qap;
        if (std::fabs(d) < FPMIN) d = FPMIN;
        d = 1.0 / d;
        double h = d;
        for (int m = 1; m <= MAXIT; ++m) {
            int m2 = 2 * m;
            double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
            d = 1.0 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN;
            c = 1.0 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
            d = 1.0 / d; h *= d * c;
            aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
            d = 1.0 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN;
            c = 1.0 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
            d = 1.0 / d;
            double del = d * c;
            h *= del;
            if (std::fabs(del - 1.0) < EPS) break;
        }
        return h;
    }

    // Regularized incomplete beta I_x(a, b).
    static double betai(double a, double b, double x) {
        if (x <= 0.0) return 0.0;
        if (x >= 1.0) return 1.0;
        double bt = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b)
                             + a * std::log(x) + b * std::log(1.0 - x));
        if (x < (a + 1.0) / (a + b + 2.0))
            return bt * betacf(a, b, x) / a;
        else
            return 1.0 - bt * betacf(b, a, 1.0 - x) / b;
    }

    // Exact two-tailed p-value for Student's t with df degrees of freedom.
    //   P(|T| >= |t|) = I_{df/(df+t^2)}(df/2, 1/2)
    static float tTailTwoSided(float t, int df) {
        if (df <= 0) return 1.0f;
        double tt = (double)t;
        double v  = (double)df;
        double x  = v / (v + tt * tt);
        return (float)betai(0.5 * v, 0.5, x);
    }

    void computeStats() {
        int test = currentTest();
        if (test == TEST_ONE) {
            int n = effectiveN(total1);
            if (n < 2) { tStat = 0.f; pValue = 1.f; effectSize = 0.f; diff = 0.f; dfCached = 0; return; }
            float m, s;
            meanSD(buf1, writeIdx1, n, m, s);
            float h0 = currentH0();
            diff = m - h0;
            float se = (s > 0.f) ? s / std::sqrt((float)n) : 0.f;
            tStat = (se > 1e-9f) ? diff / se : 0.f;
            effectSize = (s > 1e-9f) ? diff / s : 0.f;
            dfCached = n - 1;
        } else {
            int n1 = effectiveN(total1);
            int n2 = effectiveN(total2);
            if (n1 < 2 || n2 < 2) {
                tStat = 0.f; pValue = 1.f; effectSize = 0.f; diff = 0.f; dfCached = 0;
                return;
            }
            float m1, s1, m2, s2;
            meanSD(buf1, writeIdx1, n1, m1, s1);
            meanSD(buf2, writeIdx2, n2, m2, s2);
            float h0 = currentH0();
            diff = (m1 - m2) - h0;
            float var1 = s1 * s1, var2 = s2 * s2;
            float seWelch = std::sqrt(var1 / n1 + var2 / n2);
            tStat = (seWelch > 1e-9f) ? diff / seWelch : 0.f;
            float pooledVar = ((n1 - 1) * var1 + (n2 - 1) * var2) / (n1 + n2 - 2);
            float pooledSD = (pooledVar > 1e-12f) ? std::sqrt(pooledVar) : 0.f;
            effectSize = (pooledSD > 1e-9f) ? (m1 - m2) / pooledSD : 0.f;
            dfCached = n1 + n2 - 2; // approximate
        }
        pValue = tTailTwoSided(tStat, dfCached);
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearBuffers();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearBuffers();
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

        bool reject = std::fabs(tStat) > criticalValue() && dfCached > 0;

        outputs[T_OUTPUT].setVoltage(clamp(tStat, -12.f, 12.f));
        outputs[P_OUTPUT].setVoltage(clamp(pValue, 0.f, 1.f) * 10.f);
        outputs[REJECT_OUTPUT].setVoltage(reject ? 10.f : 0.f);
        outputs[EFFECT_OUTPUT].setVoltage(clamp(effectSize, -12.f, 12.f));
        lights[REJECT_LIGHT].setBrightness(reject ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — null distribution (Gaussian approximation) with shaded
// rejection regions and the observed t marked as a vertical line.
// ============================================================================

struct TestView : LightWidget {
    Test* module = nullptr;

    // Standard normal PDF
    static float gaussPDF(float z) {
        return (1.f / std::sqrt(2.f * (float)M_PI)) * std::exp(-0.5f * z * z);
    }

    // Student's t PDF: visibly fatter tails than Gaussian for small df.
    //   f(t; df) = Γ((df+1)/2) / (√(df π) Γ(df/2)) · (1 + t²/df)^(-(df+1)/2)
    static float tPDF(float t, int df) {
        if (df <= 0) return gaussPDF(t);
        double v = (double)df;
        double logNorm = std::lgamma(0.5 * (v + 1.0))
                       - 0.5 * std::log(v * (double)M_PI)
                       - std::lgamma(0.5 * v);
        double logKern = -0.5 * (v + 1.0) * std::log(1.0 + (double)t * (double)t / v);
        return (float)std::exp(logNorm + logKern);
    }

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
                    "H₀  ·  REJECT REGIONS", nullptr);
            return;
        }

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 16.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        // Fixed t-axis: -4 to +4
        const float tMin = -4.f, tMax = 4.f;
        auto mapT = [&](float t) { return x0 + W * (t - tMin) / (tMax - tMin); };

        // Compute PDF values and max for y-scaling.
        // Use exact Student's t-PDF with the current df (fatter tails at small n).
        const int kCurveN = 100;
        const int df = module->dfCached;
        std::array<float, kCurveN + 1> pdfVals;
        float maxPdf = 0.f;
        for (int i = 0; i <= kCurveN; ++i) {
            float t = tMin + (tMax - tMin) * i / kCurveN;
            pdfVals[i] = tPDF(t, df);
            if (pdfVals[i] > maxPdf) maxPdf = pdfVals[i];
        }
        if (maxPdf < 1e-9f) maxPdf = 1.f;
        auto mapPDF = [&](float p) { return y0 + H * (1.f - 0.9f * p / maxPdf); };

        // Critical value & shaded rejection regions (drawn first, behind curve)
        float tCrit = module->criticalValue();
        // Right tail: t >= tCrit
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapT(tCrit), y0 + H);
        for (int i = 0; i <= kCurveN; ++i) {
            float t = tMin + (tMax - tMin) * i / kCurveN;
            if (t < tCrit) continue;
            nvgLineTo(vg, mapT(t), mapPDF(pdfVals[i]));
        }
        nvgLineTo(vg, mapT(tMax), y0 + H);
        nvgClosePath(vg);
        nvgFillColor(vg, nvgRGBA(220, 90, 90, 90));
        nvgFill(vg);
        // Left tail: t <= -tCrit
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapT(tMin), y0 + H);
        for (int i = 0; i <= kCurveN; ++i) {
            float t = tMin + (tMax - tMin) * i / kCurveN;
            if (t > -tCrit) break;
            nvgLineTo(vg, mapT(t), mapPDF(pdfVals[i]));
        }
        nvgLineTo(vg, mapT(-tCrit), y0 + H);
        nvgClosePath(vg);
        nvgFillColor(vg, nvgRGBA(220, 90, 90, 90));
        nvgFill(vg);

        // Critical value vertical lines
        for (float tc : {tCrit, -tCrit}) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, mapT(tc), y0);
            nvgLineTo(vg, mapT(tc), y0 + H);
            nvgStrokeColor(vg, nvgRGBA(180, 80, 80, 160));
            nvgStrokeWidth(vg, 0.6f);
            nvgStroke(vg);
        }

        // The null PDF curve
        nvgBeginPath(vg);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        for (int i = 0; i <= kCurveN; ++i) {
            float t = tMin + (tMax - tMin) * i / kCurveN;
            float xp = mapT(t);
            float yp = mapPDF(pdfVals[i]);
            if (i == 0) nvgMoveTo(vg, xp, yp);
            else        nvgLineTo(vg, xp, yp);
        }
        nvgStrokeColor(vg, nvgRGB(150, 170, 200));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        // Zero centerline at t=0
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapT(0.f), y0);
        nvgLineTo(vg, mapT(0.f), y0 + H);
        nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 120));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Observed t marker — only if we have enough samples
        bool haveStat = module->dfCached > 0;
        if (haveStat) {
            float tDraw = clamp(module->tStat, tMin, tMax);
            float xObs = mapT(tDraw);
            bool reject = std::fabs(module->tStat) > tCrit;

            // Vertical line through the chart
            nvgBeginPath(vg);
            nvgMoveTo(vg, xObs, y0);
            nvgLineTo(vg, xObs, y0 + H);
            nvgStrokeColor(vg, reject ? nvgRGB(245, 90, 90) : nvgRGB(230, 175, 60));
            nvgStrokeWidth(vg, 1.6f);
            nvgStroke(vg);

            // Filled dot at the curve
            float yObs = mapPDF(tPDF(tDraw, df));
            nvgBeginPath(vg);
            nvgCircle(vg, xObs, yObs, 2.6f);
            nvgFillColor(vg, reject ? nvgRGB(245, 90, 90) : nvgRGB(230, 175, 60));
            nvgFill(vg);

            // Arrow if observed t is off-chart
            if (module->tStat > tMax) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, x0 + W - 8.f, y0 + 10.f);
                nvgLineTo(vg, x0 + W - 2.f, y0 + 14.f);
                nvgLineTo(vg, x0 + W - 8.f, y0 + 18.f);
                nvgStrokeColor(vg, nvgRGB(245, 90, 90));
                nvgStrokeWidth(vg, 1.5f);
                nvgStroke(vg);
            } else if (module->tStat < tMin) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, x0 + 8.f, y0 + 10.f);
                nvgLineTo(vg, x0 + 2.f, y0 + 14.f);
                nvgLineTo(vg, x0 + 8.f, y0 + 18.f);
                nvgStrokeColor(vg, nvgRGB(245, 90, 90));
                nvgStrokeWidth(vg, 1.5f);
                nvgStroke(vg);
            }
        }

        drawFrameLine(vg);
        drawHeader(vg, haveStat);
        drawFooter(vg, haveStat);

        // x-axis ticks at -3,-2,-1,0,1,2,3
        nvgFontSize(vg, 6.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 160));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        char buf[8];
        for (int t = -3; t <= 3; ++t) {
            std::snprintf(buf, sizeof(buf), "%d", t);
            nvgText(vg, mapT((float)t), y0 + H + 9.f, buf, nullptr);
        }
    }

    void drawHeader(NVGcontext* vg, bool haveStat) {
        const char* testShort[Test::NUM_TESTS] = {"1-SAMP", "2-SAMP"};
        const char* alphaNames[Test::NUM_ALPHAS] = {"0.01", "0.05", "0.10"};
        int test  = module->currentTest();
        int alpha = module->currentAlpha();
        int mode  = module->currentMode();
        const char* modeShort[Test::NUM_MODES] = {"SNAP", "RUN", "GROW"};

        // Show effective n so MODE behaviour (especially GROW) is transparent
        int n1 = module->effectiveN(module->total1);
        char nBuf[24];
        if (test == Test::TEST_ONE) {
            std::snprintf(nBuf, sizeof(nBuf), "n=%d", n1);
        } else {
            int n2 = module->effectiveN(module->total2);
            std::snprintf(nBuf, sizeof(nBuf), "n=%d/%d", n1, n2);
        }

        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %s %s α=%s",
                      testShort[test], modeShort[mode], nBuf, alphaNames[alpha]);
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, buf, nullptr);

        if (haveStat) {
            const char* star = "";
            if (module->pValue < 0.001f) star = " ***";
            else if (module->pValue < 0.01f) star = " **";
            else if (module->pValue < 0.05f) star = " *";
            // Big t-statistic readout on top right
            bool reject = std::fabs(module->tStat) > module->criticalValue();
            nvgFontSize(vg, 18.f);
            nvgFillColor(vg, reject ? nvgRGB(245, 90, 90)
                                    : nvgRGBA(220, 230, 245, 240));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgTextLetterSpacing(vg, 0.5f);
            std::snprintf(buf, sizeof(buf), "%.2f", module->tStat);
            nvgText(vg, box.size.x - 4, 12, buf, nullptr);
            nvgText(vg, box.size.x - 4 + 0.4f, 12, buf, nullptr);  // faux-bold
            nvgTextLetterSpacing(vg, 0.f);
            // Label and p-value
            nvgFontSize(vg, 7.f);
            nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
            nvgText(vg, box.size.x - 4, 3, "t  (statistic)", nullptr);
            std::snprintf(buf, sizeof(buf), "p=%.3f%s", module->pValue, star);
            nvgText(vg, box.size.x - 4, 33, buf, nullptr);
        }
    }

    void drawFooter(NVGcontext* vg, bool haveStat) {
        nvgFontSize(vg, 8.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        if (!haveStat) {
            nvgFillColor(vg, nvgRGBA(110, 120, 140, 160));
            nvgText(vg, box.size.x / 2, box.size.y - 2,
                    "(collecting…)", nullptr);
            return;
        }
        bool reject = std::fabs(module->tStat) > module->criticalValue();
        char buf[96];
        if (reject) {
            std::snprintf(buf, sizeof(buf), "REJECT H₀   d=%.2f   df=%d",
                          module->effectSize, module->dfCached);
            nvgFillColor(vg, nvgRGB(245, 90, 90));
        } else {
            std::snprintf(buf, sizeof(buf), "n.s.   d=%.2f   df=%d",
                          module->effectSize, module->dfCached);
            nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        }
        nvgText(vg, box.size.x / 2, box.size.y - 2, buf, nullptr);
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

struct TestWidget : ModuleWidget {
    TestWidget(Test* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Test.svg")));
        addChild(new ModuleTitle("TEST", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE"); labels->k1(1, "N");
        labels->k1(2, "α");    labels->k1(3, "H₀");
        labels->k2(1, "TEST"); labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");   labels->in(3, "SIG₂");
        labels->outSection();
        labels->out(0, "t");   labels->out(1, "p");
        labels->out(2, "REJ"); labels->out(3, "d");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new TestView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Test::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Test::N_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Test::ALPHA_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Test::H0_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(120, 294), module, Test::TEST_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Test::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Test::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Test::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Test::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Test::SIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Test::SIG2_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Test::T_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Test::P_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Test::REJECT_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(222, 358), module, Test::REJECT_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Test::EFFECT_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Test",
            {"Two-tailed t-test, one- or two-sample (Welch's t).",
             "Outputs t, p, reject-H₀ gate, Cohen's d. Visualises the",
             "null distribution with shaded rejection regions."},
            "Frame (sample first), Boot (compare with bootstrap CI)");
    }
};

Model* modelTest = createModel<Test, TestWidget>("Test");
