#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// ============================================================================
// Frame — sampling frame. Collects samples from a signal at clock rate,
// reports running mean, SD, and standard error of the mean.
//
//   Three modes:
//     SNAPSHOT  collect N samples then freeze. READY goes high until reset.
//               Cross-sectional view — the classic "take a sample".
//     RUNNING   ring buffer of the latest N samples. READY pulses on each
//               new sample. Moving window — the live measurement.
//     GROWING   accumulate up to kMaxBuf without overwriting. Visible
//               Law of Large Numbers: watch SE shrink as n grows.
//
//   Polyphonic SIG: every connected channel contributes a sample per tick.
//   So a 16-voice Polis output → Frame collects 16 samples per clock.
//
//   CI sets the z-multiplier on the SE band drawn in the visualization
//   (80% / 90% / 95% / 99%).
// ============================================================================

struct Frame : Module {
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
        SIG_INPUT,
        TRIG_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        MEAN_OUTPUT,
        SD_OUTPUT,
        SE_OUTPUT,
        READY_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, READY_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf         = 4096;
    static constexpr float kInternalClockHz = 30.f;
    static constexpr float kReadyPulseSec   = 0.002f;
    static constexpr float kSnapPulseSec    = 0.05f;

    std::array<float, kMaxBuf> samples{};
    int writeIdx = 0;
    int totalSampled = 0;
    int subCounter = 0;
    bool snapshotComplete = false;
    float readyPulse = 0.f;
    float internalClockPhase = 0.f;

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger trigTrig;
    dsp::SchmittTrigger shuffleBtn;

    Frame() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 1.f, "Mode",
                     {"Snapshot", "Running", "Growing"});
        configParam(N_PARAM, 4.f, (float)kMaxBuf, 128.f, "Frame size N");
        paramQuantities[N_PARAM]->snapEnabled = true;
        configSwitch(CI_PARAM, 0.f, (float)(NUM_CIS - 1), 2.f, "Confidence level",
                     {"80%", "90%", "95%", "99%"});
        configParam(SUB_PARAM, 1.f, 16.f, 1.f, "Sub-sample (take every Kth clock)");
        paramQuantities[SUB_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Clear buffer + restart");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear buffer)");
        configInput(SIG_INPUT, "Signal to measure (polyphonic OK)");
        configInput(TRIG_INPUT, "Manual snapshot trigger (SNAP mode)");
        configOutput(MEAN_OUTPUT, "Sample mean");
        configOutput(SD_OUTPUT, "Sample SD (unbiased)");
        configOutput(SE_OUTPUT, "Standard error of mean (SD/√n)");
        configOutput(READY_OUTPUT, "Ready gate");
        clearBuffer();
    }

    void clearBuffer() {
        samples.fill(0.f);
        writeIdx = 0;
        totalSampled = 0;
        subCounter = 0;
        snapshotComplete = false;
        readyPulse = 0.f;
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
        if (mode == MODE_RUN)  return std::min(currentN(),  totalSampled);
        if (mode == MODE_GROW) return std::min(kMaxBuf,     totalSampled);
        return std::min(currentN(), totalSampled); // SNAP
    }

    void addSample(float v) {
        int mode = currentMode();
        if (mode == MODE_SNAP && snapshotComplete) return;
        if (mode == MODE_GROW && totalSampled >= kMaxBuf) return;

        samples[writeIdx] = v;
        writeIdx = (writeIdx + 1) % kMaxBuf;
        if (totalSampled < kMaxBuf) ++totalSampled;

        if (mode == MODE_SNAP && totalSampled >= currentN()) {
            snapshotComplete = true;
            readyPulse = kSnapPulseSec;
        }
    }

    void takeClockTick() {
        ++subCounter;
        int sub = currentSub();
        if (subCounter < sub) return;
        subCounter = 0;

        if (!inputs[SIG_INPUT].isConnected()) {
            addSample(0.f);
        } else {
            int ch = std::max(1, inputs[SIG_INPUT].getChannels());
            for (int c = 0; c < ch; ++c) addSample(inputs[SIG_INPUT].getVoltage(c));
        }

        int mode = currentMode();
        if (mode == MODE_RUN || mode == MODE_GROW) readyPulse = kReadyPulseSec;
    }

    void computeStats(float& mean, float& sd) {
        int n = effectiveN();
        if (n <= 0) { mean = 0.f; sd = 0.f; return; }
        float m = 0.f;
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            m += samples[idx];
        }
        mean = m / n;
        if (n < 2) { sd = 0.f; return; }
        float v = 0.f;
        for (int i = 0; i < n; ++i) {
            int idx = (writeIdx - 1 - i + kMaxBuf) % kMaxBuf;
            float d = samples[idx] - mean;
            v += d * d;
        }
        sd = std::sqrt(v / (n - 1)); // unbiased sample SD
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearBuffer();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearBuffer();
        if (trigTrig.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) {
            if (currentMode() == MODE_SNAP) clearBuffer();
        }
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

        float mean = 0.f, sd = 0.f;
        computeStats(mean, sd);
        int n = effectiveN();
        float se = (n > 1) ? sd / std::sqrt((float)n) : 0.f;

        outputs[MEAN_OUTPUT].setVoltage(clamp(mean, -12.f, 12.f));
        outputs[SD_OUTPUT].setVoltage(clamp(sd, 0.f, 12.f));
        outputs[SE_OUTPUT].setVoltage(clamp(se, 0.f, 12.f));

        if (readyPulse > 0.f) readyPulse -= args.sampleTime;
        int mode = currentMode();
        bool readyOut, readyLit;
        if (mode == MODE_SNAP) {
            readyOut = snapshotComplete;
            readyLit = snapshotComplete;
        } else {
            readyOut = readyPulse > 0.f;
            readyLit = readyOut;
        }
        outputs[READY_OUTPUT].setVoltage(readyOut ? 10.f : 0.f);
        lights[READY_LIGHT].setBrightness(readyLit ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — histogram of buffer contents with vertical mean line and a
// shaded confidence-interval band (mean ± z·SE).
// ============================================================================

struct FrameView : LightWidget {
    Frame* module = nullptr;

    static constexpr int kBins = 32;

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
                    "MEAN  ·  CI  ·  HISTOGRAM", nullptr);
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

        if (n <= 0) {
            nvgFontSize(vg, 8.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(110, 120, 140, 140));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "(no samples yet)", nullptr);
            drawHeader(vg, mode, n);
            drawFrame(vg);
            return;
        }

        // Determine min/max of buffer
        float lo = std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < n; ++i) {
            int idx = (module->writeIdx - 1 - i + Frame::kMaxBuf) % Frame::kMaxBuf;
            float v = module->samples[idx];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi - lo < 1e-4f) { hi = lo + 0.5f; lo -= 0.5f; }
        float pad_v = (hi - lo) * 0.05f;
        float xMin = lo - pad_v;
        float xMax = hi + pad_v;

        // Histogram counts
        std::array<int, kBins> counts{};
        for (int i = 0; i < n; ++i) {
            int idx = (module->writeIdx - 1 - i + Frame::kMaxBuf) % Frame::kMaxBuf;
            float v = module->samples[idx];
            int b = (int)((v - xMin) / (xMax - xMin) * kBins);
            b = clamp(b, 0, kBins - 1);
            ++counts[b];
        }
        int maxCount = 1;
        for (int b = 0; b < kBins; ++b) if (counts[b] > maxCount) maxCount = counts[b];

        // Stats for the CI band
        float mean = 0.f, sd = 0.f;
        module->computeStats(mean, sd);
        float se = (n > 1) ? sd / std::sqrt((float)n) : 0.f;
        float z = module->currentZ();
        float ciLo = mean - z * se;
        float ciHi = mean + z * se;

        auto mapX = [&](float v) {
            return x0 + W_chart * (v - xMin) / (xMax - xMin);
        };

        // CI band (drawn behind histogram so bars overlay it)
        if (se > 0.f) {
            float cxLo = clamp(mapX(ciLo), x0, x0 + W_chart);
            float cxHi = clamp(mapX(ciHi), x0, x0 + W_chart);
            nvgBeginPath(vg);
            nvgRect(vg, cxLo, y0, cxHi - cxLo, H_chart);
            nvgFillColor(vg, nvgRGBA(230, 175, 60, 40));
            nvgFill(vg);
        }

        // Histogram bars
        for (int b = 0; b < kBins; ++b) {
            float barH = H_chart * ((float)counts[b] / maxCount);
            float bx = x0 + W_chart * ((float)b / kBins);
            float bw = W_chart / kBins - 0.6f;
            if (bw < 0.6f) bw = W_chart / kBins;
            float by = y0 + H_chart - barH;
            nvgBeginPath(vg);
            nvgRect(vg, bx, by, bw, barH);
            nvgFillColor(vg, nvgRGBA(80, 165, 220, 200));
            nvgFill(vg);
        }

        // Mean line
        float mx = mapX(mean);
        if (mx >= x0 && mx <= x0 + W_chart) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, mx, y0);
            nvgLineTo(vg, mx, y0 + H_chart);
            nvgStrokeColor(vg, nvgRGB(230, 175, 60));
            nvgStrokeWidth(vg, 1.4f);
            nvgStroke(vg);
        }

        // CI bracket below histogram
        if (se > 0.f) {
            float cy = y0 + H_chart + 4.f;
            float cxLo = clamp(mapX(ciLo), x0, x0 + W_chart);
            float cxHi = clamp(mapX(ciHi), x0, x0 + W_chart);
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

        drawFrame(vg);
        drawHeader(vg, mode, n);

        char buf[64];
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        // Big mean readout (top right of viz)
        std::snprintf(buf, sizeof(buf), "%.2f", mean);
        nvgFontSize(vg, 18.f);
        nvgFillColor(vg, nvgRGBA(220, 230, 245, 240));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgTextLetterSpacing(vg, 0.5f);
        nvgText(vg, box.size.x - 4, 12, buf, nullptr);
        nvgText(vg, box.size.x - 4 + 0.4f, 12, buf, nullptr);  // faux-bold
        nvgTextLetterSpacing(vg, 0.f);
        // Small label above and SE below
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgText(vg, box.size.x - 4, 3, "MEAN", nullptr);
        std::snprintf(buf, sizeof(buf), "SE %.3f", se);
        nvgText(vg, box.size.x - 4, 33, buf, nullptr);

        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f", xMin);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f", xMax);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }

    void drawHeader(NVGcontext* vg, int mode, int n) {
        const char* modeNames[Frame::NUM_MODES] = {"SNAPSHOT", "RUNNING", "GROWING"};
        const char* ciNames[Frame::NUM_CIS]     = {"80%", "90%", "95%", "99%"};
        int ci = clamp((int)std::round(module->params[Frame::CI_PARAM].getValue()),
                       0, Frame::NUM_CIS - 1);
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        char buf[64];
        int N = module->currentN();
        if (mode == Frame::MODE_SNAP && !module->snapshotComplete) {
            std::snprintf(buf, sizeof(buf), "%s  %d/%d  %sCI",
                          modeNames[mode], n, N, ciNames[ci]);
        } else {
            std::snprintf(buf, sizeof(buf), "%s  n=%d  %sCI",
                          modeNames[mode], n, ciNames[ci]);
        }
        nvgText(vg, 4, 3, buf, nullptr);
    }

    void drawFrame(NVGcontext* vg) {
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

struct FrameWidget : ModuleWidget {
    FrameWidget(Frame* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Frame.svg")));
        addChild(new ModuleTitle("FRAME", 300.f));

        // Panel labels (rendered via NanoVG; NanoSVG drops SVG <text>)
        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE"); labels->k1(1, "N");
        labels->k1(2, "CI");   labels->k1(3, "SUB");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");   labels->in(3, "TRIG");
        labels->outSection();
        labels->out(0, "MEAN"); labels->out(1, "SD");
        labels->out(2, "SE");   labels->out(3, "READY");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new FrameView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        // Main knob row: MODE (snap), N, CI (snap), SUB (snap)
        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Frame::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Frame::N_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Frame::CI_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Frame::SUB_PARAM));

        // Secondary row: CLEAR button (right column only)
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Frame::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Frame::SHUFFLE_LIGHT));

        // Inputs
        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Frame::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Frame::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Frame::SIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Frame::TRIG_INPUT));

        // Outputs; READY light next to its port
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Frame::MEAN_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Frame::SD_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Frame::SE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Frame::READY_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(243, 358), module, Frame::READY_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Frame",
            {"Sampling frame: collects samples on a clock and reports",
             "mean, SD and standard error. Three modes: SNAPSHOT, RUNNING",
             "window, or GROWING accumulator (visible Law of Large Numbers)."},
            "Sample (data source), Boot (bootstrap the same buffer)");
    }
};

Model* modelFrame = createModel<Frame, FrameWidget>("Frame");
