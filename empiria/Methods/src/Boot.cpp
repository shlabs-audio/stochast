#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

// ============================================================================
// Boot — bootstrap resampling.
//
//   Collects a sample of size n from SIG (polyphonic OK), then on each clock
//   tick (or RSAMP pulse) draws B resamples WITH REPLACEMENT and computes a
//   selectable statistic on each resample. The resulting empirical bootstrap
//   distribution IS the sampling distribution of the estimator — made visible
//   without parametric assumptions.
//
//   Statistics:  MEAN | MEDIAN | SD | VARIANCE
//
//   Outputs:
//     EST    — statistic computed on the original sample
//     SE     — bootstrap standard error = SD of the bootstrap distribution
//     CI_LO  — lower BCa bound
//     CI_HI  — upper BCa bound  (CI level selected by CI knob)
//
//   CIs use the bias-corrected and accelerated (BCa) interval of Efron (1987),
//   which adjusts for median bias and skewness in the bootstrap distribution.
//   Reduces exactly to a percentile CI when the bootstrap distribution is
//   symmetric and unbiased.
// ============================================================================

struct Boot : Module {
    enum Mode   { MODE_SNAP = 0, MODE_RUN, MODE_GROW, NUM_MODES };
    enum Stat   { STAT_MEAN = 0, STAT_MEDIAN, STAT_SD, STAT_VAR, NUM_STATS };
    enum CIKind { CI_80 = 0, CI_90, CI_95, CI_99, NUM_CIS };

    enum ParamId {
        MODE_PARAM,
        N_PARAM,
        B_PARAM,
        STAT_PARAM,
        CI_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        SIG_INPUT,
        RESAMPLE_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        ESTIMATE_OUTPUT,
        SE_OUTPUT,
        CI_LO_OUTPUT,
        CI_HI_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf          = 2048;
    static constexpr int   kMaxB            = 2000;
    static constexpr float kInternalClockHz = 30.f;

    std::array<float, kMaxBuf> samples{};
    int writeIdx = 0;
    int totalCollected = 0;
    int samplesSinceBootstrap = 0;
    bool snapshotComplete = false;
    float internalClockPhase = 0.f;

    // Bootstrap state
    std::array<float, kMaxB> bootStats{};
    int activeBoots = 0;
    float originalEstimate = 0.f;
    float bootSE = 0.f;
    float ciLo = 0.f, ciHi = 0.f;
    float bootMin = 0.f, bootMax = 0.f;
    float bcaZ0 = 0.f;       // bias correction Φ⁻¹(#{θ̂*<θ̂}/B)
    float bcaAccel = 0.f;    // acceleration from jackknife skewness

    std::mt19937 rng{0xB0075u};
    dsp::SchmittTrigger clockTrig, resetTrig, resampleTrig, shuffleBtn;

    Boot() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 0.f, "Mode",
                     {"Snapshot", "Running", "Growing"});
        configParam(N_PARAM, 4.f, (float)kMaxBuf, 128.f, "Sample size n");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configParam(B_PARAM, 50.f, (float)kMaxB, 500.f, "Bootstrap iterations B");
        paramQuantities[B_PARAM]->snapEnabled = true;
        configSwitch(STAT_PARAM, 0.f, (float)(NUM_STATS - 1), 0.f, "Statistic",
                     {"Mean", "Median", "SD", "Variance"});
        configSwitch(CI_PARAM, 0.f, (float)(NUM_CIS - 1), 2.f, "CI level",
                     {"80%", "90%", "95%", "99%"});
        configButton(SHUFFLE_PARAM, "Clear buffer + re-seed");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset buffer");
        configInput(SIG_INPUT, "Signal to sample (polyphonic OK)");
        configInput(RESAMPLE_INPUT, "Re-bootstrap trigger (same data, fresh resamples)");
        configOutput(ESTIMATE_OUTPUT, "Point estimate (statistic on original sample)");
        configOutput(SE_OUTPUT, "Bootstrap SE (SD of bootstrap distribution)");
        configOutput(CI_LO_OUTPUT, "CI lower percentile bound");
        configOutput(CI_HI_OUTPUT, "CI upper percentile bound");
        clearBuffer();
    }

    void clearBuffer() {
        samples.fill(0.f);
        writeIdx = 0;
        totalCollected = 0;
        samplesSinceBootstrap = 0;
        snapshotComplete = false;
        activeBoots = 0;
        originalEstimate = 0.f;
        bootSE = 0.f;
        ciLo = ciHi = 0.f;
        bootMin = bootMax = 0.f;
        bcaZ0 = 0.f;
        bcaAccel = 0.f;
    }

    void onReset() override { clearBuffer(); }

    int   currentMode() { return clamp((int)std::round(params[MODE_PARAM].getValue()), 0, NUM_MODES - 1); }
    int   currentStat() { return clamp((int)std::round(params[STAT_PARAM].getValue()), 0, NUM_STATS - 1); }
    int   currentCI()   { return clamp((int)std::round(params[CI_PARAM].getValue()),   0, NUM_CIS - 1); }
    int   currentN()    { return clamp((int)std::round(params[N_PARAM].getValue()),    4, kMaxBuf); }
    int   currentB()    { return clamp((int)std::round(params[B_PARAM].getValue()),    50, kMaxB); }

    float currentCILevel() {
        switch (currentCI()) {
            case CI_80: return 0.80f;
            case CI_90: return 0.90f;
            case CI_95: return 0.95f;
            case CI_99: return 0.99f;
        }
        return 0.95f;
    }

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

    // Standard normal CDF Φ(z), used by BCa CI computation.
    static double normalCDF(double z) {
        return 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
    }

    // Inverse standard normal CDF (probit), Acklam's rational approximation.
    // Accurate to ~1e-9 — vastly more than enough for a CI cutoff.
    static double normalInvCDF(double p) {
        if (p <= 0.0) return -1e9;
        if (p >= 1.0) return  1e9;
        static const double a[6] = {
            -3.969683028665376e+01,  2.209460984245205e+02,
            -2.759285104469687e+02,  1.383577518672690e+02,
            -3.066479806614716e+01,  2.506628277459239e+00 };
        static const double b[5] = {
            -5.447609879822406e+01,  1.615858368580409e+02,
            -1.556989798598866e+02,  6.680131188771972e+01,
            -1.328068155288572e+01 };
        static const double c[6] = {
            -7.784894002430293e-03, -3.223964580411365e-01,
            -2.400758277161838e+00, -2.549732539343734e+00,
             4.374664141464968e+00,  2.938163982698783e+00 };
        static const double d[4] = {
             7.784695709041462e-03,  3.224671290700398e-01,
             2.445134137142996e+00,  3.754408661907416e+00 };
        double q, r, x;
        const double plow  = 0.02425;
        const double phigh = 1.0 - plow;
        if (p < plow) {
            q = std::sqrt(-2.0 * std::log(p));
            x = (((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
                ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
        } else if (p <= phigh) {
            q = p - 0.5; r = q * q;
            x = (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5])*q /
                (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
        } else {
            q = std::sqrt(-2.0 * std::log(1.0 - p));
            x = -(((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
                 ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
        }
        return x;
    }

    static float statOn(const std::vector<float>& v, int statKind) {
        int n = (int)v.size();
        if (n <= 0) return 0.f;
        switch (statKind) {
            case STAT_MEAN: {
                double s = 0.0;
                for (float x : v) s += x;
                return (float)(s / n);
            }
            case STAT_MEDIAN: {
                std::vector<float> c = v;
                std::sort(c.begin(), c.end());
                if (n % 2 == 1) return c[n / 2];
                return 0.5f * (c[n / 2 - 1] + c[n / 2]);
            }
            case STAT_SD: {
                if (n < 2) return 0.f;
                double m = 0; for (float x : v) m += x; m /= n;
                double ss = 0; for (float x : v) { double d = x - m; ss += d * d; }
                return (float)std::sqrt(ss / (n - 1));
            }
            case STAT_VAR: {
                if (n < 2) return 0.f;
                double m = 0; for (float x : v) m += x; m /= n;
                double ss = 0; for (float x : v) { double d = x - m; ss += d * d; }
                return (float)(ss / (n - 1));
            }
        }
        return 0.f;
    }

    void performBootstrap() {
        int n = effectiveN();
        if (n < 2) {
            activeBoots = 0;
            originalEstimate = 0.f;
            bootSE = 0.f;
            ciLo = ciHi = 0.f;
            bootMin = bootMax = 0.f;
            bcaZ0 = 0.f;
            bcaAccel = 0.f;
            return;
        }

        int B = currentB();
        int statKind = currentStat();

        // Gather original sample
        std::vector<float> orig(n);
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            orig[i] = samples[idx];
        }
        originalEstimate = statOn(orig, statKind);

        std::uniform_int_distribution<int> uid(0, n - 1);
        std::vector<float> resample(n);
        for (int b = 0; b < B; ++b) {
            for (int i = 0; i < n; ++i) resample[i] = orig[uid(rng)];
            bootStats[b] = statOn(resample, statKind);
        }
        activeBoots = B;

        // SE = SD of bootstrap stats
        double m = 0;
        for (int b = 0; b < B; ++b) m += bootStats[b];
        m /= B;
        double ss = 0;
        for (int b = 0; b < B; ++b) { double d = bootStats[b] - m; ss += d * d; }
        bootSE = (B > 1) ? (float)std::sqrt(ss / (B - 1)) : 0.f;

        // BCa confidence interval (Efron 1987).
        //
        // Bias-corrected and accelerated bootstrap CI. Two ingredients:
        //
        //   z0 = Φ⁻¹( (#{θ̂*_b < θ̂}) / B )  — bias correction (median bias of
        //        the bootstrap distribution relative to the original estimate)
        //
        //   â  = Σ_i (θ̂_(·) − θ̂_(i))³  /  6 · (Σ_i (θ̂_(·) − θ̂_(i))²)^(3/2)
        //        — acceleration (skewness of the leave-one-out jackknife)
        //
        // Adjusted percentile cutoffs replace the naive α/2 and 1−α/2:
        //   α₁ = Φ( z0 + (z0 + z_{α/2})   / (1 − â (z0 + z_{α/2}))   )
        //   α₂ = Φ( z0 + (z0 + z_{1−α/2}) / (1 − â (z0 + z_{1−α/2})) )
        //
        // This corrects for bias and skewness in the bootstrap distribution.
        // When z0 = 0 and â = 0 it reduces to the percentile CI exactly.

        std::array<float, kMaxB> sorted;
        std::copy(bootStats.begin(), bootStats.begin() + B, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + B);
        bootMin = sorted[0];
        bootMax = sorted[B - 1];

        const float cilv  = currentCILevel();
        const double alpha = 1.0 - (double)cilv;

        // --- Bias correction z0 from bootstrap distribution ---
        int below = 0;
        for (int b = 0; b < B; ++b)
            if (bootStats[b] < originalEstimate) ++below;
        // Avoid the degenerate Φ⁻¹(0) / Φ⁻¹(1) endpoints
        double frac = (double)below / (double)B;
        frac = std::clamp(frac, 0.5 / B, 1.0 - 0.5 / B);
        double z0 = normalInvCDF(frac);

        // --- Acceleration â from leave-one-out jackknife ---
        double accel = 0.0;
        if (n >= 3) {
            std::vector<float> jack(n);
            std::vector<float> loo;
            loo.reserve(n - 1);
            for (int i = 0; i < n; ++i) {
                loo.clear();
                for (int j = 0; j < n; ++j) if (j != i) loo.push_back(orig[j]);
                jack[i] = statOn(loo, statKind);
            }
            double jmean = 0.0;
            for (float x : jack) jmean += x;
            jmean /= n;
            double num = 0.0, den = 0.0;
            for (float x : jack) {
                double diff = jmean - x;       // sign convention: θ̂_(·) − θ̂_(i)
                num += diff * diff * diff;
                den += diff * diff;
            }
            double denPow = std::pow(den, 1.5);
            accel = (denPow > 1e-18) ? num / (6.0 * denPow) : 0.0;
        }

        // --- Adjusted percentiles ---
        double zLoStd = normalInvCDF(alpha / 2.0);
        double zHiStd = normalInvCDF(1.0 - alpha / 2.0);

        auto bcaAlpha = [&](double zStd) {
            double s = z0 + zStd;
            double denom = 1.0 - accel * s;
            if (std::fabs(denom) < 1e-12) denom = (denom < 0 ? -1e-12 : 1e-12);
            return normalCDF(z0 + s / denom);
        };

        double alphaLo = std::clamp(bcaAlpha(zLoStd), 1.0 / (B + 1), 1.0 - 1.0 / (B + 1));
        double alphaHi = std::clamp(bcaAlpha(zHiStd), 1.0 / (B + 1), 1.0 - 1.0 / (B + 1));

        int loIdx = clamp((int)std::round(alphaLo * (B - 1)), 0, B - 1);
        int hiIdx = clamp((int)std::round(alphaHi * (B - 1)), 0, B - 1);
        ciLo = sorted[loIdx];
        ciHi = sorted[hiIdx];

        bcaZ0    = (float)z0;
        bcaAccel = (float)accel;
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

        bool needBootstrap = false;
        int mode = currentMode();
        if (tick) {
            int ch = inputs[SIG_INPUT].isConnected()
                     ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
            int added = 0;
            if (!inputs[SIG_INPUT].isConnected()) {
                addSample(0.f);
                added = 1;
            } else {
                for (int c = 0; c < ch; ++c) { addSample(inputs[SIG_INPUT].getVoltage(c)); ++added; }
            }
            samplesSinceBootstrap += added;

            // Throttle: in RUN/GROW, only re-bootstrap when ~N/4 new samples have
            // landed (or every 30 samples, whichever is smaller). In SNAP, fire
            // once when the snapshot first completes.
            if (mode == MODE_SNAP) {
                if (snapshotComplete && activeBoots == 0) needBootstrap = true;
            } else {
                int N = currentN();
                int threshold = std::max(8, std::min(30, N / 4));
                if (samplesSinceBootstrap >= threshold) needBootstrap = true;
            }
        }
        if (resampleTrig.process(inputs[RESAMPLE_INPUT].getVoltage(), 0.1f, 1.f)) {
            needBootstrap = true;
        }
        if (needBootstrap) {
            performBootstrap();
            samplesSinceBootstrap = 0;
        }

        outputs[ESTIMATE_OUTPUT].setVoltage(clamp(originalEstimate, -12.f, 12.f));
        outputs[SE_OUTPUT].setVoltage(clamp(bootSE, 0.f, 12.f));
        outputs[CI_LO_OUTPUT].setVoltage(clamp(ciLo, -12.f, 12.f));
        outputs[CI_HI_OUTPUT].setVoltage(clamp(ciHi, -12.f, 12.f));
    }
};

// ============================================================================
// Visualization — histogram of bootstrap statistics with vertical line at the
// original estimate and a shaded percentile-CI band.
// ============================================================================

struct BootView : LightWidget {
    Boot* module = nullptr;

    static constexpr int kBins = 40;

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
                    "BOOTSTRAP DISTRIBUTION", nullptr);
            return;
        }

        int B = module->activeBoots;
        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 18.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        drawHeader(vg, B);
        drawFrameLine(vg);

        if (B < 2) {
            nvgFontSize(vg, 8.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(110, 120, 140, 140));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "(collecting data…)", nullptr);
            return;
        }

        // X range from bootstrap stats
        float xMin = module->bootMin;
        float xMax = module->bootMax;
        // Include original estimate in the range
        if (module->originalEstimate < xMin) xMin = module->originalEstimate;
        if (module->originalEstimate > xMax) xMax = module->originalEstimate;
        if (xMax - xMin < 1e-4f) { xMax = xMin + 0.5f; xMin -= 0.5f; }
        float xPad = (xMax - xMin) * 0.05f;
        xMin -= xPad; xMax += xPad;

        auto mapX = [&](float v) { return x0 + W * (v - xMin) / (xMax - xMin); };

        // Histogram bins
        std::array<int, kBins> counts{};
        for (int b = 0; b < B; ++b) {
            float v = module->bootStats[b];
            int bin = (int)((v - xMin) / (xMax - xMin) * kBins);
            bin = clamp(bin, 0, kBins - 1);
            ++counts[bin];
        }
        int maxCount = 1;
        for (int b = 0; b < kBins; ++b) if (counts[b] > maxCount) maxCount = counts[b];

        // CI band (behind everything)
        float cxLo = clamp(mapX(module->ciLo), x0, x0 + W);
        float cxHi = clamp(mapX(module->ciHi), x0, x0 + W);
        nvgBeginPath(vg);
        nvgRect(vg, cxLo, y0, cxHi - cxLo, H);
        nvgFillColor(vg, nvgRGBA(230, 175, 60, 38));
        nvgFill(vg);

        // Histogram bars
        for (int b = 0; b < kBins; ++b) {
            float barH = H * ((float)counts[b] / maxCount);
            float bx = x0 + W * ((float)b / kBins);
            float bw = W / kBins - 0.6f;
            if (bw < 0.6f) bw = W / kBins;
            float by = y0 + H - barH;
            nvgBeginPath(vg);
            nvgRect(vg, bx, by, bw, barH);
            nvgFillColor(vg, nvgRGBA(80, 165, 220, 200));
            nvgFill(vg);
        }

        // CI bracket below histogram
        {
            float cy = y0 + H + 4.f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, cxLo, cy);
            nvgLineTo(vg, cxHi, cy);
            nvgStrokeColor(vg, nvgRGB(230, 175, 60));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
            for (float cx : {cxLo, cxHi}) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx, cy - 2.f);
                nvgLineTo(vg, cx, cy + 2.f);
                nvgStroke(vg);
            }
        }

        // Original-estimate vertical line
        float ex = clamp(mapX(module->originalEstimate), x0, x0 + W);
        nvgBeginPath(vg);
        nvgMoveTo(vg, ex, y0);
        nvgLineTo(vg, ex, y0 + H);
        nvgStrokeColor(vg, nvgRGB(245, 90, 90));
        nvgStrokeWidth(vg, 1.6f);
        nvgStroke(vg);

        // Bottom strip — results readout (no more x-range labels; those are
        // implicit from the chart). Three slots: est / CI / SE.
        char buf[64];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);

        // Big EST readout on the left
        std::snprintf(buf, sizeof(buf), "%.2f", module->originalEstimate);
        nvgFontSize(vg, 18.f);
        nvgFillColor(vg, nvgRGB(245, 90, 90));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgTextLetterSpacing(vg, 0.5f);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        nvgText(vg, 4 + 0.4f, box.size.y - 3, buf, nullptr);  // faux-bold
        nvgTextLetterSpacing(vg, 0.f);
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgText(vg, 4, box.size.y - 21, "EST", nullptr);

        // CI in the centre, amber
        nvgFontSize(vg, 8.5f);
        std::snprintf(buf, sizeof(buf), "[%.2f, %.2f]", module->ciLo, module->ciHi);
        nvgFillColor(vg, nvgRGB(230, 175, 60));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        nvgText(vg, box.size.x / 2, box.size.y - 3, buf, nullptr);

        // SE on the right
        std::snprintf(buf, sizeof(buf), "SE=%.2f", module->bootSE);
        nvgFillColor(vg, nvgRGBA(180, 190, 210, 220));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }

    void drawHeader(NVGcontext* vg, int B) {
        const char* statNames[Boot::NUM_STATS] = {"MEAN", "MEDIAN", "SD", "VAR"};
        const char* ciNames[Boot::NUM_CIS]     = {"80%", "90%", "95%", "99%"};
        const char* modeShort[Boot::NUM_MODES] = {"SNAP", "RUN", "GROW"};
        int n = module->effectiveN();
        int stat = module->currentStat();
        int ci   = module->currentCI();
        int mode = module->currentMode();

        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        // Left: "boot of MEAN  n=128  B=500" — names what's plotted
        char buf[96];
        std::snprintf(buf, sizeof(buf), "boot of %s  n=%d  B=%d",
                      statNames[stat], n, B);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 220));
        nvgText(vg, 4, 3, buf, nullptr);

        // Right: short mode + CI level (+ BCa label)
        std::snprintf(buf, sizeof(buf), "%s · %s · BCa",
                      modeShort[mode], ciNames[ci]);
        nvgFillColor(vg, nvgRGBA(120, 130, 150, 180));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Middle-top: BCa diagnostics z₀ and â — exposes the correction
        // visually so students can see what BCa is actually doing.
        std::snprintf(buf, sizeof(buf), "z₀=%+.2f  â=%+.2f",
                      module->bcaZ0, module->bcaAccel);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 160));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x / 2, 3, buf, nullptr);
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

struct BootWidget : ModuleWidget {
    BootWidget(Boot* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Boot.svg")));
        addChild(new ModuleTitle("BOOT", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE"); labels->k1(1, "N");
        labels->k1(2, "B");    labels->k1(3, "STAT");
        labels->k2(2, "CI");   labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");   labels->in(3, "RSAMP");
        labels->outSection();
        labels->out(0, "EST");  labels->out(1, "SE");
        labels->out(2, "CI_LO");labels->out(3, "CI_HI");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new BootView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Boot::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Boot::N_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Boot::B_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Boot::STAT_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(195, 294), module, Boot::CI_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Boot::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Boot::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Boot::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Boot::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Boot::SIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Boot::RESAMPLE_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Boot::ESTIMATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Boot::SE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Boot::CI_LO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Boot::CI_HI_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Boot",
            {"Resamples a buffer with replacement B times and computes",
             "a statistic (mean, median, SD, variance) on each resample.",
             "Outputs point estimate, bootstrap SE and percentile-CI bounds."},
            "Frame or Tape (buffer source), Seed (reproducible resamples)");
    }
};

Model* modelBoot = createModel<Boot, BootWidget>("Boot");
