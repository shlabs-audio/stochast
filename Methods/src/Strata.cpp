#include "plugin.hpp"
#include <vector>
#include <cmath>

// ============================================================================
// Strata — online STL-style decomposition of a CV/audio signal.
//
//   trend     ← 1-pole low-pass (cutoff in Hz, log-scaled by TREND knob)
//   detrended ← signal − trend
//   seasonal  ← EMA-smoothed mean of detrended values at each phase of a
//               user-specified period P. Operates on a ring buffer indexed
//               by sampleCounter % P.
//   residual  ← signal − trend − seasonal
// ============================================================================

struct Strata : Module {
    enum ParamId  { TREND_PARAM, PERIOD_PARAM, MEMORY_PARAM, NUM_PARAMS };
    enum InputId  { PERIOD_CV_INPUT, SIGNAL_INPUT, NUM_INPUTS };
    enum OutputId { TREND_OUTPUT, SEASONAL_OUTPUT, RESIDUAL_OUTPUT, NUM_OUTPUTS };

    static constexpr float kMaxPeriodSec = 4.0f;
    static constexpr int   SCOPE_BUF = 256;

    // DSP state
    float trendY = 0.f;
    std::vector<float> seasonalTable;   // pre-allocated at max size; active [0,currentTableSize)
    std::vector<float> resampleScratch; // pre-allocated scratch for period changes (RT-safe)
    int phase = 0;
    int currentTableSize = 0;

    // Scope buffers — read by widget on UI thread; tearing is invisible.
    float inBuf[SCOPE_BUF]    = {};
    float trendBuf[SCOPE_BUF] = {};
    float seasBuf[SCOPE_BUF]  = {};
    float residBuf[SCOPE_BUF] = {};
    int scopeWriteIdx = 0;
    int scopeFrameCounter = 0;

    Strata() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
        configParam(TREND_PARAM,  0.f, 1.f, 0.35f, "Trend cutoff");
        configParam(PERIOD_PARAM, 0.f, 1.f, 0.40f, "Seasonal period");
        configParam(MEMORY_PARAM, 0.f, 1.f, 0.50f, "Seasonal memory");
        configInput(PERIOD_CV_INPUT, "Period CV (±10 V → ×0.25..×4)");
        configInput(SIGNAL_INPUT, "Signal");
        configOutput(TREND_OUTPUT, "Trend");
        configOutput(SEASONAL_OUTPUT, "Seasonal");
        configOutput(RESIDUAL_OUTPUT, "Residual");
        allocTables();
        currentTableSize = 1;
    }

    // Size the seasonal table (and its resample scratch) to the largest period
    // possible at the current sample rate, once, so process() never reallocates.
    void allocTables() {
        float sr = APP->engine->getSampleRate();
        int maxSize = std::max(2, (int)std::ceil(kMaxPeriodSec * sr) + 1);
        seasonalTable.assign(maxSize, 0.f);
        resampleScratch.assign(maxSize, 0.f);
    }

    void onReset() override {
        trendY = 0.f;
        std::fill(seasonalTable.begin(), seasonalTable.end(), 0.f);
        phase = 0;
        if (currentTableSize < 1) currentTableSize = 1;
        for (int i = 0; i < SCOPE_BUF; ++i) {
            inBuf[i] = trendBuf[i] = seasBuf[i] = residBuf[i] = 0.f;
        }
        scopeWriteIdx = 0;
    }

    void onSampleRateChange() override {
        allocTables();
        currentTableSize = 1;
        onReset();
    }

    static float logMap(float t, float lo, float hi) {
        return lo * std::pow(hi / lo, t);
    }

    void process(const ProcessArgs& args) override {
        float trendT = clamp(params[TREND_PARAM].getValue(), 0.f, 1.f);
        float fc = logMap(trendT, 0.01f, 200.f);

        float perT = clamp(params[PERIOD_PARAM].getValue(), 0.f, 1.f);
        float pSec = logMap(perT, 0.01f, kMaxPeriodSec);
        if (inputs[PERIOD_CV_INPUT].isConnected()) {
            float cv = clamp(inputs[PERIOD_CV_INPUT].getVoltage() / 5.f, -2.f, 2.f);
            pSec *= std::pow(2.f, cv);
        }
        pSec = clamp(pSec, 0.005f, kMaxPeriodSec);

        int maxSize = (int)seasonalTable.size();
        int newSize = clamp((int)std::round(pSec * args.sampleRate), 1, maxSize);
        if (newSize != currentTableSize) {
            // Resample the active region into the pre-allocated scratch buffer
            // and copy back — no heap allocation/free on the audio thread.
            for (int i = 0; i < newSize; ++i) {
                int src = (int)((float)i / newSize * currentTableSize);
                if (src >= currentTableSize) src = currentTableSize - 1;
                resampleScratch[i] = seasonalTable[src];
            }
            for (int i = 0; i < newSize; ++i) seasonalTable[i] = resampleScratch[i];
            phase = phase % newSize;
            currentTableSize = newSize;
        }

        float memT = clamp(params[MEMORY_PARAM].getValue(), 0.f, 1.f);
        float alpha = logMap(memT, 0.0005f, 0.5f);

        float x = inputs[SIGNAL_INPUT].getVoltage();

        float a = 1.f - std::exp(-2.f * (float)M_PI * fc / args.sampleRate);
        trendY += a * (x - trendY);
        float trend = trendY;

        float detrended = x - trend;

        float prev = seasonalTable[phase];
        float newSeasonal = prev + alpha * (detrended - prev);
        seasonalTable[phase] = newSeasonal;
        float seasonal = newSeasonal;
        if (++phase >= currentTableSize) phase = 0;

        float residual = x - trend - seasonal;

        outputs[TREND_OUTPUT].setVoltage(clamp(trend, -12.f, 12.f));
        outputs[SEASONAL_OUTPUT].setVoltage(clamp(seasonal, -12.f, 12.f));
        outputs[RESIDUAL_OUTPUT].setVoltage(clamp(residual, -12.f, 12.f));

        // Scope sampling at ~120 Hz
        int stride = std::max(1, (int)(args.sampleRate / 120.f));
        if (++scopeFrameCounter >= stride) {
            scopeFrameCounter = 0;
            inBuf[scopeWriteIdx]    = x;
            trendBuf[scopeWriteIdx] = trend;
            seasBuf[scopeWriteIdx]  = seasonal;
            residBuf[scopeWriteIdx] = residual;
            scopeWriteIdx = (scopeWriteIdx + 1) % SCOPE_BUF;
        }
    }
};

// ============================================================================
// Scope widget — stacked traces for trend (with input overlay), seasonal,
// residual. Each strip auto-scales independently.
// ============================================================================

struct StrataScope : LightWidget {
    Strata* module = nullptr;

    void drawTrace(NVGcontext* vg, float y0, float stripH, const float* buf,
                   int wIdx, float range, NVGcolor col, float thick) {
        auto valY = [&](float v) {
            return y0 + (0.5f - v / (2.f * range)) * stripH;
        };
        nvgBeginPath(vg);
        const int B = Strata::SCOPE_BUF;
        for (int i = 0; i < B; ++i) {
            int idx = (wIdx + i) % B;
            float x = (float)i / (B - 1) * box.size.x;
            float y = valY(buf[idx]);
            if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
        }
        nvgStrokeColor(vg, col);
        nvgStrokeWidth(vg, thick);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        nvgStroke(vg);
    }

    static float bufRange(const float* buf, const float* alt = nullptr) {
        float m = 0.1f;
        for (int i = 0; i < Strata::SCOPE_BUF; ++i) {
            if (std::abs(buf[i]) > m) m = std::abs(buf[i]);
            if (alt && std::abs(alt[i]) > m) m = std::abs(alt[i]);
        }
        return m * 1.15f;
    }

    void drawStrip(NVGcontext* vg, float y0, float stripH, const char* label,
                   const float* faintBuf, const float* mainBuf,
                   NVGcolor mainCol, int wIdx) {
        nvgSave(vg);
        nvgScissor(vg, 0, y0, box.size.x, stripH);

        // Mid line
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0, y0 + stripH / 2);
        nvgLineTo(vg, box.size.x, y0 + stripH / 2);
        nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 100));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        float range = bufRange(mainBuf, faintBuf);
        if (faintBuf) {
            drawTrace(vg, y0, stripH, faintBuf, wIdx, range,
                      nvgRGBA(160, 170, 190, 90), 0.7f);
        }
        drawTrace(vg, y0, stripH, mainBuf, wIdx, range, mainCol, 1.3f);

        // Label in top-left of strip
        nvgFontSize(vg, 8.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, y0 + 3, label, nullptr);

        nvgRestore(vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        NVGcontext* vg = args.vg;

        // Outline / dark fill (over the SVG rect, so we control color)
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(8, 10, 16));
        nvgFill(vg);

        if (!module) {
            // Browser preview — flat dark with placeholder text
            nvgFontSize(vg, 9.f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(120, 130, 150, 120));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x / 2, box.size.y / 2,
                    "TREND  ·  SEASONAL  ·  RESIDUAL", nullptr);
            return;
        }

        float stripH = box.size.y / 3.f;
        int wIdx = module->scopeWriteIdx;

        drawStrip(vg, 0,           stripH, "TREND",
                  module->inBuf, module->trendBuf, nvgRGB(207, 138, 79), wIdx);
        drawStrip(vg, stripH,      stripH, "SEASONAL",
                  nullptr, module->seasBuf, nvgRGB(80, 182, 129), wIdx);
        drawStrip(vg, 2.f*stripH,  stripH, "RESIDUAL",
                  nullptr, module->residBuf, nvgRGB(140, 108, 210), wIdx);

        // Separators
        nvgBeginPath(vg);
        for (int i = 1; i < 3; ++i) {
            nvgMoveTo(vg, 0, i * stripH);
            nvgLineTo(vg, box.size.x, i * stripH);
        }
        nvgStrokeColor(vg, nvgRGBA(40, 46, 64, 200));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Subtle frame
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

struct StrataWidget : ModuleWidget {
    StrataWidget(Strata* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Strata.svg")));
        addChild(new ModuleTitle("STRATA", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "TREND");  labels->k1(1, "PERIOD");
        labels->k1(2, "MEMORY");
        labels->inSection();
        labels->in(0, "P·CV"); labels->in(1, "SIG");
        labels->outSection();
        // Output labels use module-specific colors to match the trace colors
        labels->custom(45.f,  344.f, 7.5f, nvgRGB(0xcf, 0x8a, 0x4f),
                       NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, "TREND");
        labels->custom(120.f, 344.f, 7.5f, nvgRGB(0x50, 0xb6, 0x81),
                       NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, "SEAS");
        labels->custom(195.f, 344.f, 7.5f, nvgRGB(0x8c, 0x6c, 0xd2),
                       NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, "RESD");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* scope = new StrataScope;
        scope->module = module;
        scope->box.pos  = Vec(10, 44);
        scope->box.size = Vec(280, 190);
        addChild(scope);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Strata::TREND_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Strata::PERIOD_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Strata::MEMORY_PARAM));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Strata::PERIOD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Strata::SIGNAL_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Strata::TREND_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Strata::SEASONAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Strata::RESIDUAL_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Strata",
            {"Splits a CV/audio stream into three layers: trend (low-pass",
             "drift), seasonal (period-locked recurrence), and residual",
             "(everything else). The classic STL decomposition."},
            "Tape (record a stream first), Frame / Test (analyse residual)");
    }
};

Model* modelStrata = createModel<Strata, StrataWidget>("Strata");
