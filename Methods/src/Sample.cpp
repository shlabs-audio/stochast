#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

// ============================================================================
// Sample — data-generating process. Draws from a selectable parametric
// distribution at clock rate. Parameters P1 / P2 are remapped per-mode:
//
//   NORMAL       P1 = μ ∈ [-5, +5]      P2 = σ ∈ [0.01, 5]   (linear / log)
//   UNIFORM      P1 = centre ∈ [-5, +5] P2 = width ∈ [0, 10] (linear)
//   EXPONENTIAL  P1 = rate λ ∈ [0.1, 10]  (log)  P2 unused
//   BETA         P1 = α ∈ [0.1, 10] (log)  P2 = β ∈ [0.1, 10] (log)
//                — outputs on [0, 1]
//
//   P*_CV adds knob-equivalent offset (±5 V → ±0.5 of normalized knob range).
//   The window of recent samples drives the empirical histogram and running
//   mean / SD outputs. The visualization overlays the theoretical PDF on the
//   empirical histogram so the user sees them coincide as N grows.
// ============================================================================

struct Sample : Module {
    enum Dist { DIST_NORMAL = 0, DIST_UNIFORM, DIST_EXP, DIST_BETA, NUM_DISTS };

    enum ParamId {
        DIST_PARAM,
        P1_PARAM,
        P2_PARAM,
        WINDOW_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        P1_CV_INPUT,
        P2_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        SAMPLE_OUTPUT,
        MEAN_OUTPUT,
        SD_OUTPUT,
        TRIG_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxWindow = 1024;
    static constexpr float kInternalClockHz = 30.f;
    static constexpr float kTrigPulseSec    = 0.002f;

    std::array<float, kMaxWindow> samples{};
    int sampleIdx = 0;
    int totalSampled = 0;     // running count up to kMaxWindow
    int windowSize = 128;
    float currentSample = 0.f;

    float internalClockPhase = 0.f;
    float trigPulse = 0.f;

    std::mt19937 rng{0xD107D107u};

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger shuffleBtn;

    Sample() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(DIST_PARAM, 0.f, (float)(NUM_DISTS - 1), 0.f,
                     "Distribution", {"Normal", "Uniform", "Exponential", "Beta"});
        configParam(P1_PARAM, 0.f, 1.f, 0.5f,
                    "Parameter 1 (μ / centre / λ / α — depends on distribution)");
        configParam(P2_PARAM, 0.f, 1.f, 0.3f,
                    "Parameter 2 (σ / width / — / β — depends on distribution)");
        configParam(WINDOW_PARAM, 4.f, (float)kMaxWindow, 128.f, "Window size (samples)");
        paramQuantities[WINDOW_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Re-seed RNG and clear window");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset window + re-seed");
        configInput(P1_CV_INPUT, "Parameter 1 CV (±5 V adds to knob)");
        configInput(P2_CV_INPUT, "Parameter 2 CV (±5 V adds to knob)");
        configOutput(SAMPLE_OUTPUT, "Sample (most recent draw)");
        configOutput(MEAN_OUTPUT, "Empirical mean over window");
        configOutput(SD_OUTPUT, "Empirical SD over window");
        configOutput(TRIG_OUTPUT, "Sync trigger (pulse on each new sample)");
        clearWindow();
    }

    int currentDist() {
        return clamp((int)std::round(params[DIST_PARAM].getValue()), 0, NUM_DISTS - 1);
    }

    void clearWindow() {
        samples.fill(0.f);
        sampleIdx = 0;
        totalSampled = 0;
    }

    void onReset() override {
        clearWindow();
        rng.seed(0xD107D107u);
    }

    // Normalized knob (0..1) including CV
    float p1Norm() {
        float k = params[P1_PARAM].getValue();
        if (inputs[P1_CV_INPUT].isConnected()) k += inputs[P1_CV_INPUT].getVoltage() / 10.f;
        return clamp(k, 0.f, 1.f);
    }
    float p2Norm() {
        float k = params[P2_PARAM].getValue();
        if (inputs[P2_CV_INPUT].isConnected()) k += inputs[P2_CV_INPUT].getVoltage() / 10.f;
        return clamp(k, 0.f, 1.f);
    }

    // Map (p1Norm, p2Norm) → actual distribution parameters
    void mapParams(float& p1, float& p2) {
        float k1 = p1Norm(), k2 = p2Norm();
        switch (currentDist()) {
            case DIST_NORMAL:
                p1 = -5.f + 10.f * k1;                          // μ ∈ [-5, +5]
                p2 = 0.01f * std::pow(500.f, k2);               // σ ∈ [0.01, 5] log
                break;
            case DIST_UNIFORM:
                p1 = -5.f + 10.f * k1;                          // centre
                p2 = 10.f * k2;                                 // width
                break;
            case DIST_EXP:
                p1 = 0.1f * std::pow(100.f, k1);                // λ ∈ [0.1, 10] log
                p2 = 0.f;
                break;
            case DIST_BETA:
                p1 = 0.1f * std::pow(100.f, k1);                // α log
                p2 = 0.1f * std::pow(100.f, k2);                // β log
                break;
        }
    }

    float drawSample() {
        float p1, p2;
        mapParams(p1, p2);
        switch (currentDist()) {
            case DIST_NORMAL: {
                std::normal_distribution<float> d(p1, std::max(0.001f, p2));
                return d(rng);
            }
            case DIST_UNIFORM: {
                float lo = p1 - p2 * 0.5f;
                float hi = p1 + p2 * 0.5f;
                if (hi <= lo) return lo;
                std::uniform_real_distribution<float> d(lo, hi);
                return d(rng);
            }
            case DIST_EXP: {
                std::exponential_distribution<float> d(std::max(0.001f, p1));
                return d(rng);
            }
            case DIST_BETA: {
                std::gamma_distribution<float> ga(std::max(0.01f, p1), 1.f);
                std::gamma_distribution<float> gb(std::max(0.01f, p2), 1.f);
                float x = ga(rng);
                float y = gb(rng);
                return (x + y) > 1e-9f ? x / (x + y) : 0.5f;
            }
        }
        return 0.f;
    }

    void tick() {
        currentSample = drawSample();
        samples[sampleIdx] = currentSample;
        sampleIdx = (sampleIdx + 1) % kMaxWindow;
        if (totalSampled < kMaxWindow) ++totalSampled;
        trigPulse = kTrigPulseSec;
    }

    // Returns running stats over the last min(windowSize, totalSampled) samples
    void runningStats(float& mean, float& sd) {
        int W = std::min(windowSize, totalSampled);
        if (W <= 0) { mean = 0.f; sd = 0.f; return; }
        float m = 0.f;
        for (int i = 0; i < W; ++i) {
            int idx = (sampleIdx - 1 - i + kMaxWindow) % kMaxWindow;
            m += samples[idx];
        }
        mean = m / W;
        float v = 0.f;
        for (int i = 0; i < W; ++i) {
            int idx = (sampleIdx - 1 - i + kMaxWindow) % kMaxWindow;
            float d = samples[idx] - mean;
            v += d * d;
        }
        sd = std::sqrt(v / W);
    }

    void process(const ProcessArgs& args) override {
        windowSize = clamp((int)std::round(params[WINDOW_PARAM].getValue()), 4, kMaxWindow);

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            clearWindow();
            rng.seed(0xD107D107u);
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            clearWindow();
            rng.seed(rng() ^ 0xA53F71u);
        }
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick();
        } else {
            internalClockPhase += args.sampleTime * kInternalClockHz;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick();
            }
        }

        float mean = 0.f, sd = 0.f;
        runningStats(mean, sd);

        outputs[SAMPLE_OUTPUT].setVoltage(clamp(currentSample, -12.f, 12.f));
        outputs[MEAN_OUTPUT].setVoltage(clamp(mean, -12.f, 12.f));
        outputs[SD_OUTPUT].setVoltage(clamp(sd, 0.f, 12.f));

        if (trigPulse > 0.f) trigPulse -= args.sampleTime;
        outputs[TRIG_OUTPUT].setVoltage(trigPulse > 0.f ? 10.f : 0.f);
    }
};

// ============================================================================
// Visualization — empirical histogram with theoretical PDF overlay.
// ============================================================================

struct SampleView : LightWidget {
    Sample* module = nullptr;

    static constexpr int kBins = 40;

    // Returns x-range to display for the current distribution + parameters
    void displayRange(float p1, float p2, int dist, float& xMin, float& xMax) {
        switch (dist) {
            case Sample::DIST_NORMAL:
                xMin = p1 - 3.5f * p2;
                xMax = p1 + 3.5f * p2;
                break;
            case Sample::DIST_UNIFORM:
                xMin = p1 - p2 * 0.5f - 0.4f;
                xMax = p1 + p2 * 0.5f + 0.4f;
                break;
            case Sample::DIST_EXP:
                xMin = 0.f;
                xMax = std::max(0.1f, 5.f / std::max(0.01f, p1));
                break;
            case Sample::DIST_BETA:
                xMin = 0.f;
                xMax = 1.f;
                break;
        }
        if (xMax - xMin < 1e-3f) xMax = xMin + 1.f;
    }

    float evalPDF(float x, float p1, float p2, int dist) {
        switch (dist) {
            case Sample::DIST_NORMAL: {
                float z = (x - p1) / p2;
                return std::exp(-0.5f * z * z) / (p2 * std::sqrt(2.f * (float)M_PI));
            }
            case Sample::DIST_UNIFORM: {
                float lo = p1 - p2 * 0.5f;
                float hi = p1 + p2 * 0.5f;
                if (x < lo || x > hi || hi <= lo) return 0.f;
                return 1.f / (hi - lo);
            }
            case Sample::DIST_EXP: {
                if (x < 0.f) return 0.f;
                return p1 * std::exp(-p1 * x);
            }
            case Sample::DIST_BETA: {
                if (x <= 0.f || x >= 1.f) return 0.f;
                // Unnormalized kernel — exact normalization requires Β(α,β);
                // we rescale by max value below so the curve fits the panel
                // anyway. The kernel x^(α-1) (1-x)^(β-1) diverges at 0 and 1
                // for α<1 or β<1 (e.g. the "coin flip" preset α=β=0.05). To
                // keep the visualization legible we clamp the evaluation
                // away from the endpoints; the sampler itself is unaffected.
                const float xc = clamp(x, 0.01f, 0.99f);
                return std::pow(xc, p1 - 1.f) * std::pow(1.f - xc, p2 - 1.f);
            }
        }
        return 0.f;
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
                    "PDF  ·  EMPIRICAL", nullptr);
            return;
        }

        const int dist = module->currentDist();
        float p1, p2;
        module->mapParams(p1, p2);

        float xMin, xMax;
        displayRange(p1, p2, dist, xMin, xMax);

        // Histogram counts over the last min(window, total) samples in [xMin, xMax]
        int W = std::min(module->windowSize, module->totalSampled);
        std::array<int, kBins> counts{};
        for (int i = 0; i < W; ++i) {
            int idx = (module->sampleIdx - 1 - i + Sample::kMaxWindow) % Sample::kMaxWindow;
            float s = module->samples[idx];
            if (s < xMin || s > xMax) continue;
            int b = (int)((s - xMin) / (xMax - xMin) * kBins);
            b = clamp(b, 0, kBins - 1);
            ++counts[b];
        }
        float binWidth = (xMax - xMin) / kBins;

        // PDF samples and max-density for vertical scaling
        std::array<float, kBins + 1> pdfVals{};
        float maxDensity = 0.f;
        for (int i = 0; i <= kBins; ++i) {
            float x = xMin + ((float)i / kBins) * (xMax - xMin);
            pdfVals[i] = evalPDF(x, p1, p2, dist);
            if (pdfVals[i] > maxDensity) maxDensity = pdfVals[i];
        }
        // Beta uses unnormalized kernel; renormalize so plot scale is comparable
        // to histogram density. For other dists, the normalized PDF is already
        // correct, so we just take whichever max is larger to fit both.
        float histDensityMax = 0.f;
        if (W > 0) {
            for (int b = 0; b < kBins; ++b) {
                float d = counts[b] / (binWidth * (float)W);
                if (d > histDensityMax) histDensityMax = d;
            }
        }
        float yMax = std::max(maxDensity, histDensityMax) * 1.10f;
        if (yMax < 1e-6f) yMax = 1.f;

        // Drawing region
        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 12.f;
        float W_chart = box.size.x - 2 * pad;
        float H_chart = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        // x = 0 reference vertical line, if visible
        if (xMin <= 0.f && xMax >= 0.f) {
            float zx = x0 + W_chart * (-xMin) / (xMax - xMin);
            nvgBeginPath(vg);
            nvgMoveTo(vg, zx, y0);
            nvgLineTo(vg, zx, y0 + H_chart);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 80));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        // Histogram bars
        if (W > 0) {
            for (int b = 0; b < kBins; ++b) {
                float density = counts[b] / (binWidth * (float)W);
                float barH = H_chart * (density / yMax);
                float bx = x0 + W_chart * ((float)b / kBins);
                float bw = W_chart / kBins - 0.6f;
                if (bw < 0.6f) bw = W_chart / kBins;
                float by = y0 + H_chart - barH;
                nvgBeginPath(vg);
                nvgRect(vg, bx, by, bw, barH);
                nvgFillColor(vg, nvgRGBA(80, 165, 220, 170));
                nvgFill(vg);
            }
        }

        // PDF curve (smooth line)
        nvgBeginPath(vg);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        bool first = true;
        for (int i = 0; i <= kBins; ++i) {
            float v = pdfVals[i] / yMax;
            float xp = x0 + W_chart * ((float)i / kBins);
            float yp = y0 + H_chart * (1.f - clamp(v, 0.f, 1.f));
            if (first) { nvgMoveTo(vg, xp, yp); first = false; }
            else        nvgLineTo(vg, xp, yp);
        }
        nvgStrokeColor(vg, nvgRGB(230, 175, 60));
        nvgStrokeWidth(vg, 1.4f);
        nvgStroke(vg);

        // Current sample tick mark
        float curX = module->currentSample;
        if (curX >= xMin && curX <= xMax) {
            float cx = x0 + W_chart * (curX - xMin) / (xMax - xMin);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, y0 + H_chart);
            nvgLineTo(vg, cx, y0 + H_chart - 6.f);
            nvgStrokeColor(vg, nvgRGB(245, 90, 90));
            nvgStrokeWidth(vg, 1.4f);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, y0 + H_chart - 6.f, 1.8f);
            nvgFillColor(vg, nvgRGB(245, 90, 90));
            nvgFill(vg);
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — distribution + parameter readout
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        const char* distNames[Sample::NUM_DISTS] = {"NORMAL", "UNIFORM", "EXP", "BETA"};
        nvgText(vg, 4, 3, distNames[dist], nullptr);

        char buf[64];
        switch (dist) {
            case Sample::DIST_NORMAL:  std::snprintf(buf, sizeof(buf), "μ=%.2f  σ=%.2f", p1, p2); break;
            case Sample::DIST_UNIFORM: std::snprintf(buf, sizeof(buf), "c=%.2f  w=%.2f", p1, p2); break;
            case Sample::DIST_EXP:     std::snprintf(buf, sizeof(buf), "λ=%.2f", p1); break;
            case Sample::DIST_BETA:    std::snprintf(buf, sizeof(buf), "α=%.2f  β=%.2f", p1, p2); break;
        }
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x / 2, 3, buf, nullptr);

        std::snprintf(buf, sizeof(buf), "n=%d", W);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom strip — x-axis min/max labels
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f", xMin);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f", xMax);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct SampleWidget : ModuleWidget {
    SampleWidget(Sample* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Sample.svg")));
        addChild(new ModuleTitle("SAMPLE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "DIST"); labels->k1(1, "P1");
        labels->k1(2, "P2");   labels->k1(3, "WIN");
        labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "P1·CV"); labels->in(3, "P2·CV");
        labels->outSection();
        labels->out(0, "SAMPLE"); labels->out(1, "MEAN");
        labels->out(2, "SD");     labels->out(3, "TRIG");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new SampleView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Sample::DIST_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Sample::P1_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Sample::P2_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Sample::WINDOW_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Sample::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Sample::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Sample::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Sample::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Sample::P1_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Sample::P2_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Sample::SAMPLE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Sample::MEAN_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Sample::SD_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Sample::TRIG_OUTPUT));
    }

    // Real-world distribution presets — each entry sets DIST, P1, P2.
    // The voltage range is dimensionless within VCV; presets pick a
    // shape that matches the real-world variable's typical empirical
    // distribution. Where a real variable's mean falls outside the
    // module's voltage range (e.g. cm-scale heights, 100-scale IQs),
    // the preset uses a normalized stand-in (e.g. height in metres,
    // IQ / 100).
    struct PresetItem : MenuItem {
        Sample* sample;
        int dist;
        float p1, p2;
        PresetItem(Sample* s, const std::string& label, int d, float a, float b)
            : sample(s), dist(d), p1(a), p2(b) {
            text = label;
        }
        void onAction(const event::Action& e) override {
            if (!sample) return;
            sample->params[Sample::DIST_PARAM].setValue((float)dist);
            sample->params[Sample::P1_PARAM].setValue(clamp(p1, 0.f, 1.f));
            sample->params[Sample::P2_PARAM].setValue(clamp(p2, 0.f, 1.f));
        }
    };

    void appendContextMenu(Menu* menu) override {
        auto* s = dynamic_cast<Sample*>(this->module);
        if (!s) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Real-world distribution presets"));
        // dist normalized P1/P2 values map back to the actual μ/σ via
        // Sample::mapParams (linear for centre, log for σ). The numbers
        // below were chosen so the resulting distribution shape matches
        // a typical empirical distribution.
        menu->addChild(new PresetItem(s, "Adult height (Normal-like)",
                                      Sample::DIST_NORMAL, 0.55f, 0.40f));
        menu->addChild(new PresetItem(s, "IQ scores (Normal μ=100, σ=15)",
                                      Sample::DIST_NORMAL, 0.50f, 0.45f));
        menu->addChild(new PresetItem(s, "Reaction times (Exponential)",
                                      Sample::DIST_EXP, 0.45f, 0.0f));
        menu->addChild(new PresetItem(s, "Survey responses (Uniform on [-1,1])",
                                      Sample::DIST_UNIFORM, 0.50f, 0.20f));
        menu->addChild(new PresetItem(s, "U-shaped opinion (Beta α=β=0.2)",
                                      Sample::DIST_BETA, 0.20f, 0.20f));
        menu->addChild(new PresetItem(s, "Right-skewed income (Beta α=1, β=4)",
                                      Sample::DIST_BETA, 0.42f, 0.74f));
        menu->addChild(new PresetItem(s, "Bell-shaped, narrow (Normal σ=0.3)",
                                      Sample::DIST_NORMAL, 0.50f, 0.30f));
        menu->addChild(new PresetItem(s, "Coin flip (Beta α=β=0.05, near-Bernoulli)",
                                      Sample::DIST_BETA, 0.10f, 0.10f));

        appendAboutMenu(menu, "Sample",
            {"Draws random samples from a parametric distribution",
             "(Normal / Uniform / Exponential / Beta) at clock rate.",
             "Reports running empirical mean and SD."},
            "Frame (sampling frame), Tape (record), Seed (reproducibility)");
    }
};

Model* modelSample = createModel<Sample, SampleWidget>("Sample");
