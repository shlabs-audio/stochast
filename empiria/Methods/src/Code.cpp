#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
// Code — continuous-to-categorical encoder.
//
//   Quantizes the SIG voltage into K ∈ [2, 7] ordinal categories by uniform
//   cutpoints over [LOW, HIGH]. The bridge from continuous DGP to survey-like
//   categorical observations.
//
//   Category mapping (clamped to 1..K):
//       k = 1 + floor( K · (x − LOW) / (HIGH − LOW) )
//
//   Outputs:
//     CAT     polyphonic — 1 V per category index (cat 1 → 1 V, cat K → K V)
//     GATES   polyphonic K-channel — channel k is high if any input sample
//             is currently in category k
//     MEAN    running mean category across the buffer (1..K mapped to 1..K V)
//     ENT     Shannon entropy of empirical category distribution, scaled to
//             0..10 V (10 V = log2(K), i.e. maximum entropy = uniform)
// ============================================================================

struct Code : Module {
    enum ParamId {
        K_PARAM,
        LOW_PARAM,
        HIGH_PARAM,
        N_PARAM,
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
        CAT_OUTPUT,
        GATES_OUTPUT,
        MEAN_OUTPUT,
        ENTROPY_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxK             = 7;
    static constexpr int   kMaxN             = 4096;
    static constexpr float kInternalClockHz  = 60.f;

    int K = 5;
    int N = 128;

    // Ring buffer of recent category counts and current-frame gate flags
    std::array<int, kMaxN> buf{};
    int writeIdx = 0;
    int totalCollected = 0;

    // Per-frame state
    std::array<bool, kMaxK> activeThisFrame{};
    std::array<int, kMaxK>  histCount{};
    float runningMeanK = 1.f;
    float entropyBits  = 0.f;
    int   lastCat = 1;

    float internalClockPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Code() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(K_PARAM, 2.f, (float)kMaxK, 5.f, "Categories K");
        paramQuantities[K_PARAM]->snapEnabled = true;
        configParam(LOW_PARAM,  -12.f, 12.f, -5.f, "Lower bound");
        configParam(HIGH_PARAM, -12.f, 12.f,  5.f, "Upper bound");
        configParam(N_PARAM, 8.f, (float)kMaxN, 128.f, "Window size for stats");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Clear histogram + stats");
        configInput(CLOCK_INPUT, "Clock (sample on each tick; free-runs at 60 Hz)");
        configInput(RESET_INPUT, "Reset (clear histogram)");
        configInput(SIG_INPUT, "Signal to encode (polyphonic OK)");
        configOutput(CAT_OUTPUT, "Category (polyphonic, 1 V per category)");
        configOutput(GATES_OUTPUT, "Per-category presence gates (polyphonic K)");
        configOutput(MEAN_OUTPUT, "Running mean category (1..K V)");
        configOutput(ENTROPY_OUTPUT, "Empirical entropy (0..10 V, max = log2(K))");
        clearStats();
    }

    void clearStats() {
        buf.fill(0);
        writeIdx = 0;
        totalCollected = 0;
        histCount.fill(0);
        activeThisFrame.fill(false);
        runningMeanK = 1.f;
        entropyBits  = 0.f;
        lastCat = 1;
    }

    void onReset() override { clearStats(); }

    int  currentK() { return clamp((int)std::round(params[K_PARAM].getValue()), 2, kMaxK); }
    int  currentN() { return clamp((int)std::round(params[N_PARAM].getValue()), 8, kMaxN); }

    int encodeCategory(float v, float lo, float hi, int k) {
        if (hi - lo < 1e-6f) return 1;
        float t = (v - lo) / (hi - lo);
        int cat = 1 + (int)std::floor(t * k);
        return clamp(cat, 1, k);
    }

    int effectiveN() {
        return std::min(currentN(), totalCollected);
    }

    void addCategory(int cat) {
        if (totalCollected >= kMaxN) {
            // Decrement old bucket
            int old = buf[writeIdx];
            if (old >= 1 && old <= kMaxK) histCount[old - 1] = std::max(0, histCount[old - 1] - 1);
        }
        buf[writeIdx] = cat;
        writeIdx = (writeIdx + 1) % kMaxN;
        if (totalCollected < kMaxN) ++totalCollected;
        if (cat >= 1 && cat <= kMaxK) ++histCount[cat - 1];
    }

    void recomputeStats() {
        int k = currentK();
        int n = effectiveN();
        if (n <= 0) { runningMeanK = 1.f; entropyBits = 0.f; return; }

        // Mean
        double sum = 0;
        int total = 0;
        for (int c = 0; c < k; ++c) {
            sum += (double)(c + 1) * histCount[c];
            total += histCount[c];
        }
        runningMeanK = (total > 0) ? (float)(sum / total) : 1.f;

        // Shannon entropy in bits
        if (total > 0) {
            double H = 0;
            for (int c = 0; c < k; ++c) {
                if (histCount[c] == 0) continue;
                double p = (double)histCount[c] / total;
                H -= p * std::log2(p);
            }
            entropyBits = (float)H;
        } else {
            entropyBits = 0.f;
        }
    }

    void onClockTick() {
        // Re-read parameters (in case K changed since last reset)
        int k = currentK();
        if (k != K) {
            // K change: clear stats; old categories are mis-binned otherwise
            K = k;
            clearStats();
        }

        float lo = params[LOW_PARAM].getValue();
        float hi = params[HIGH_PARAM].getValue();

        // Process all SIG channels
        int ch = inputs[SIG_INPUT].isConnected()
                 ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
        activeThisFrame.fill(false);
        for (int c = 0; c < ch; ++c) {
            float v = inputs[SIG_INPUT].isConnected() ? inputs[SIG_INPUT].getVoltage(c) : 0.f;
            int cat = encodeCategory(v, lo, hi, k);
            addCategory(cat);
            if (cat >= 1 && cat <= kMaxK) activeThisFrame[cat - 1] = true;
            lastCat = cat;
        }

        recomputeStats();
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearStats();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearStats();
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

        // Live CAT output (polyphonic, same width as SIG, encoded continuously)
        int k = currentK();
        float lo = params[LOW_PARAM].getValue();
        float hi = params[HIGH_PARAM].getValue();
        int ch = inputs[SIG_INPUT].isConnected()
                 ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
        outputs[CAT_OUTPUT].setChannels(ch);
        for (int c = 0; c < ch; ++c) {
            float v = inputs[SIG_INPUT].isConnected() ? inputs[SIG_INPUT].getVoltage(c) : 0.f;
            int cat = encodeCategory(v, lo, hi, k);
            outputs[CAT_OUTPUT].setVoltage((float)cat, c);
        }

        // Per-category gates
        outputs[GATES_OUTPUT].setChannels(k);
        for (int c = 0; c < k; ++c) {
            outputs[GATES_OUTPUT].setVoltage(activeThisFrame[c] ? 10.f : 0.f, c);
        }

        // MEAN — 1..K V
        outputs[MEAN_OUTPUT].setVoltage(clamp(runningMeanK, 0.f, 12.f));

        // ENTROPY — 0..10 V (10 V = log2(K) bits)
        float maxH = std::log2((float)k);
        float scaled = (maxH > 0.f) ? entropyBits / maxH * 10.f : 0.f;
        outputs[ENTROPY_OUTPUT].setVoltage(clamp(scaled, 0.f, 10.f));
    }
};

// ============================================================================
// Visualization — histogram of category counts, with the most-recent category
// highlighted in red. Cutpoints from LOW..HIGH are noted below the histogram.
// ============================================================================

struct CodeView : LightWidget {
    Code* module = nullptr;

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
                    "CATEGORY  HISTOGRAM", nullptr);
            return;
        }

        int k = module->currentK();
        int n = module->effectiveN();
        float lo = module->params[Code::LOW_PARAM].getValue();
        float hi = module->params[Code::HIGH_PARAM].getValue();

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 16.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad;
        float y0 = pad + topStripH;

        int maxC = 1;
        for (int c = 0; c < k; ++c) if (module->histCount[c] > maxC) maxC = module->histCount[c];

        // Bars
        float slot = W / k;
        for (int c = 0; c < k; ++c) {
            float t  = (float)module->histCount[c] / maxC;
            float bh = H * t;
            float bx = x0 + slot * c + slot * 0.12f;
            float bw = slot * 0.76f;
            float by = y0 + H - bh;
            bool current = (c + 1 == module->lastCat);
            nvgBeginPath(vg);
            nvgRect(vg, bx, by, bw, bh);
            nvgFillColor(vg, current ? nvgRGBA(245, 90, 90, 230) : nvgRGBA(80, 165, 220, 210));
            nvgFill(vg);

            // Category number under bar
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", c + 1);
            nvgFontSize(vg, 8.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(vg, bx + bw * 0.5f, y0 + H + 2.f, buf, nullptr);

            // Count atop bar (only if reasonably tall and there's room)
            if (module->histCount[c] > 0 && bh > 12.f) {
                std::snprintf(buf, sizeof(buf), "%d", module->histCount[c]);
                nvgFontSize(vg, 7.f);
                nvgFillColor(vg, nvgRGB(255, 255, 255));
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
                nvgText(vg, bx + bw * 0.5f, by - 1.f, buf, nullptr);
            }
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip
        char buf[64];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "K=%d  n=%d  [%.1f, %.1f]", k, n, lo, hi);
        nvgText(vg, 4, 3, buf, nullptr);

        std::snprintf(buf, sizeof(buf), "μ̂=%.2f  H=%.2f", module->runningMeanK, module->entropyBits);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom strip — cutpoint values, every k-1 spaces
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        if (k <= 7) {
            float span = hi - lo;
            for (int c = 0; c <= k; ++c) {
                float cut = lo + span * c / k;
                float xPos = x0 + slot * c;
                std::snprintf(buf, sizeof(buf), "%.1f", cut);
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
                nvgText(vg, xPos, box.size.y - 3, buf, nullptr);
            }
        }
    }
};

// ============================================================================
// Widget
// ============================================================================

struct CodeWidget : ModuleWidget {
    CodeWidget(Code* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Code.svg")));
        addChild(new ModuleTitle("CODE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "K");    labels->k1(1, "LOW");
        labels->k1(2, "HIGH"); labels->k1(3, "N");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");
        labels->outSection();
        labels->out(0, "CAT");  labels->out(1, "GATES");
        labels->out(2, "MEAN"); labels->out(3, "ENT");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new CodeView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Code::K_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Code::LOW_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Code::HIGH_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Code::N_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Code::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Code::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Code::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Code::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Code::SIG_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Code::CAT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Code::GATES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Code::MEAN_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Code::ENTROPY_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Code",
            {"Continuous → categorical encoder. Maps a CV stream into",
             "K ordinal categories (2..7) by uniform cutpoints over",
             "[LOW, HIGH]. Outputs category, gates, running mean, entropy."},
            "Tab (cross-tabulate two Code outputs), Cohort (data-driven cuts)");
    }
};

Model* modelCode = createModel<Code, CodeWidget>("Code");
