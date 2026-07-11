#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

// ============================================================================
// DDM — drift-diffusion model (Ratcliff 1978).
//
//   Two-alternative forced-choice decision model. An "evidence accumulator"
//   x_t integrates noisy samples until it crosses one of two absorbing
//   boundaries at ±a:
//
//     dx = v · dt + σ · √dt · ξ_t       (ξ_t ~ N(0, 1))
//
//   How to use it
//   -------------
//   The visualization shows ONE trial at a time. The trace grows from left
//   (trial start, accumulator at z·a) to right (boundary hit). On a boundary
//   hit the chosen boundary flashes (green = upper, red = lower) and the
//   trial holds visible for ~750 ms so you can see what happened. Then a new
//   trial starts.
//
//   Knobs (all live, take effect from the *next* trial):
//     DRIFT v     signal strength. v = 0 → pure random walk (50/50 choice,
//                 long RT). |v| > 0 → consistent drift toward one boundary.
//     a           threshold. Higher a → slower but more accurate decisions
//                 (the speed–accuracy trade-off).
//     σ           within-trial noise. σ = 0 + v > 0 → straight ramp (no
//                 indecision). Larger σ → wandering trajectories.
//     BIAS z      starting-point bias. z = 0 starts at zero; z = +0.5 starts
//                 halfway to the upper bound (response prejudice).
//
//   Outputs:
//     EVID     live evidence trace x_t (±a → ±10 V mapped)
//     CHOICE   gate that pulses high when upper boundary is hit (one trial)
//     RT       reaction time of the last completed trial (V scaled)
//     ACC      running mean accuracy = fraction of trials hitting the
//              boundary in the *correct* direction (i.e. upper if v > 0)
// ============================================================================

struct DDM : Module {
    enum ParamId {
        DRIFT_PARAM,      // v
        THRESHOLD_PARAM,  // a
        NOISE_PARAM,      // σ
        BIAS_PARAM,       // z ∈ [−1, 1]
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        DRIFT_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        EVID_OUTPUT,
        CHOICE_OUTPUT,
        RT_OUTPUT,
        ACC_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, UPPER_LIGHT, LOWER_LIGHT, NUM_LIGHTS };

    static constexpr float kInternalHz = 120.f;   // integration steps per sec
    static constexpr float kPulseSec   = 0.040f;
    static constexpr float kHoldSec    = 0.75f;   // freeze the trace on hit
    static constexpr int   kTraceLen   = 512;

    float x = 0.f;                // current accumulator value
    float dt = 1.f / 120.f;
    int trialSteps = 0;
    int trials = 0;
    int correct = 0;
    float lastRT = 0.f;
    float upperPulse = 0.f, lowerPulse = 0.f;

    // Per-trial trace: cleared at trial start, written into 0..trialSteps-1
    // as the trial unfolds, held for kHoldSec after boundary hit.
    std::array<float, kTraceLen> trace{};
    int   traceLen        = 0;          // valid samples 0..traceLen-1
    bool  trialHolding    = false;      // true during the post-hit hold
    float holdRemaining   = 0.f;
    bool  lastHitUpper    = false;
    int   lastTrialLen    = 0;

    float internalPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;
    std::mt19937 rng;
    uint32_t seedVal = 0x4ABCDEF1u;

    DDM() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(DRIFT_PARAM, -2.f, 2.f, 0.6f, "Drift rate v");
        configParam(THRESHOLD_PARAM, 0.2f, 5.f, 1.5f, "Threshold a");
        configParam(NOISE_PARAM, 0.f, 3.f, 1.0f, "Within-trial noise σ");
        configParam(BIAS_PARAM, -1.f, 1.f, 0.f, "Starting-point bias z");
        configButton(SHUFFLE_PARAM, "Reset trace + accuracy counter");
        configInput(CLOCK_INPUT, "Clock — integration step rate (internal 120 Hz)");
        configInput(RESET_INPUT, "Reset (restart trial + clear accuracy)");
        configInput(DRIFT_CV_INPUT, "Drift CV (±10 V → ±1 added to v)");
        configOutput(EVID_OUTPUT, "Live evidence x_t scaled to ±a → ±10 V");
        configOutput(CHOICE_OUTPUT, "Choice gate (pulses when upper boundary hit)");
        configOutput(RT_OUTPUT, "Reaction time of last completed trial (2 V/second)");
        configOutput(ACC_OUTPUT, "Running accuracy (0..10 V)");
        restartTrial();
    }

    void onReset() override {
        trials = 0; correct = 0; lastRT = 0.f;
        trace.fill(0.f);
        traceLen = 0;
        trialHolding = false;
        holdRemaining = 0.f;
        seedVal = 0x4ABCDEF1u;
        rng.seed(seedVal);
        restartTrial();
    }

    float currentDrift() {
        float v = clamp(params[DRIFT_PARAM].getValue(), -3.f, 3.f);
        if (inputs[DRIFT_CV_INPUT].isConnected())
            v += inputs[DRIFT_CV_INPUT].getVoltage() * 0.1f;
        return v;
    }

    void restartTrial() {
        float a = clamp(params[THRESHOLD_PARAM].getValue(), 0.05f, 6.f);
        float z = clamp(params[BIAS_PARAM].getValue(), -1.f, 1.f);
        x = z * a;
        trialSteps = 0;
        traceLen = 0;
        trace[0] = x;
        trialHolding = false;
        holdRemaining = 0.f;
    }

    void substep() {
        // While holding a completed trial, freeze x and trace; don't integrate.
        if (trialHolding) return;

        float v = currentDrift();
        float a = clamp(params[THRESHOLD_PARAM].getValue(), 0.05f, 6.f);
        float sigma = clamp(params[NOISE_PARAM].getValue(), 0.f, 5.f);
        std::normal_distribution<float> nd(0.f, 1.f);
        x += v * dt + sigma * std::sqrt(dt) * nd(rng);
        ++trialSteps;

        // Record into the trace buffer. When the buffer fills up, decimate
        // in place — drop every other sample, halving the temporal
        // resolution — and keep going. This lets a low-drift trial run as
        // long as it needs while still showing the full path; previously a
        // hard cap force-resolved the trial after kTraceLen steps, which
        // was bad DDM modelling.
        if (traceLen < kTraceLen) {
            trace[traceLen] = x;
            ++traceLen;
        } else {
            for (int i = 0; i < kTraceLen / 2; ++i) {
                trace[i] = trace[2 * i];
            }
            traceLen = kTraceLen / 2;
            trace[traceLen] = x;
            ++traceLen;
        }

        // Trial ends only on a genuine boundary hit. No force-close.
        if (std::fabs(x) >= a) {
            // Clamp x to the boundary for a clean visual
            x = (x >= 0.f) ? a : -a;
            trace[std::max(0, traceLen - 1)] = x;
            lastRT = trialSteps * dt;
            ++trials;
            lastHitUpper = (x > 0.f);
            bool correctChoice = (v >= 0.f && lastHitUpper) || (v < 0.f && !lastHitUpper);
            if (correctChoice) ++correct;
            if (lastHitUpper) upperPulse = kPulseSec; else lowerPulse = kPulseSec;
            lastTrialLen = traceLen;
            trialHolding = true;
            holdRemaining = kHoldSec;
        }
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            trials = 0; correct = 0; lastRT = 0.f;
            seedVal = seedVal * 1664525u + 1013904223u;
            rng.seed(seedVal);
            restartTrial();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            trials = 0; correct = 0; lastRT = 0.f;
            restartTrial();
        }
        lights[SHUFFLE_LIGHT].setBrightness(
            params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        // Trial-hold timer counts down in real time so the user can SEE the
        // completed trial for ~750 ms before the next one starts.
        if (trialHolding) {
            holdRemaining -= args.sampleTime;
            if (holdRemaining <= 0.f) restartTrial();
        }

        bool tick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick = true;
            dt = 1.f / kInternalHz;
        } else {
            internalPhase += args.sampleTime * kInternalHz;
            if (internalPhase >= 1.f) { internalPhase -= 1.f; tick = true; }
            dt = 1.f / kInternalHz;
        }
        if (tick) substep();

        float a = clamp(params[THRESHOLD_PARAM].getValue(), 0.05f, 6.f);
        outputs[EVID_OUTPUT].setVoltage(clamp(x / a * 10.f, -12.f, 12.f));

        if (upperPulse > 0.f) upperPulse -= args.sampleTime;
        if (lowerPulse > 0.f) lowerPulse -= args.sampleTime;
        outputs[CHOICE_OUTPUT].setVoltage(upperPulse > 0.f ? 10.f : 0.f);
        lights[UPPER_LIGHT].setBrightness(upperPulse > 0.f ? 1.f : 0.f);
        lights[LOWER_LIGHT].setBrightness(lowerPulse > 0.f ? 1.f : 0.f);

        outputs[RT_OUTPUT].setVoltage(clamp(lastRT * 2.f, 0.f, 12.f));
        float acc = (trials > 0) ? (float)correct / trials : 0.f;
        outputs[ACC_OUTPUT].setVoltage(clamp(acc, 0.f, 1.f) * 10.f);
    }
};

struct DDMView : LightWidget {
    DDM* module = nullptr;

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
                    "DRIFT-DIFFUSION", nullptr);
            return;
        }

        float pad = 6.f, topStripH = 12.f, botStripH = 12.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad, y0 = pad + topStripH;

        float a = clamp(module->params[DDM::THRESHOLD_PARAM].getValue(), 0.05f, 6.f);
        auto mapY = [&](float v) { return y0 + H * (1.f - (v + a) / (2.f * a)); };

        nvgSave(vg);
        nvgScissor(vg, x0, y0, W, H);

        // Boundaries
        nvgStrokeColor(vg, nvgRGB(120, 200, 140));
        nvgStrokeWidth(vg, 1.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, mapY(a)); nvgLineTo(vg, x0 + W, mapY(a));
        nvgStroke(vg);
        nvgStrokeColor(vg, nvgRGB(245, 120, 120));
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, mapY(-a)); nvgLineTo(vg, x0 + W, mapY(-a));
        nvgStroke(vg);

        // Zero line
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 130));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, mapY(0.f)); nvgLineTo(vg, x0 + W, mapY(0.f));
        nvgStroke(vg);

        // Highlight the boundary that was hit during the current hold.
        if (module->trialHolding) {
            NVGcolor flash = module->lastHitUpper ? nvgRGBA(120, 220, 140, 70)
                                                  : nvgRGBA(245, 120, 120, 70);
            nvgBeginPath(vg);
            if (module->lastHitUpper) nvgRect(vg, x0, mapY(a) - 2.f, W, 4.f);
            else                       nvgRect(vg, x0, mapY(-a) - 2.f, W, 4.f);
            nvgFillColor(vg, flash);
            nvgFill(vg);
        }

        // Per-trial trace: paint left-to-right across the plot. The horizontal
        // axis is the trial's elapsed time, normalized so the full panel
        // shows the trial from start to finish (or the most recent kTraceLen
        // steps if it's still running).
        int N = module->traceLen;
        if (N > 0) {
            // Use the FULL trace span when a trial is still active, but freeze
            // the same span during the hold so the completed path stays in
            // place.
            int spanLen = std::max(N, 2);
            nvgStrokeColor(vg, module->trialHolding
                                ? (module->lastHitUpper ? nvgRGB(170, 230, 160)
                                                        : nvgRGB(245, 160, 160))
                                : nvgRGB(110, 200, 220));
            nvgStrokeWidth(vg, 1.2f);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);
            nvgBeginPath(vg);
            for (int i = 0; i < N; ++i) {
                float xp = x0 + W * ((float)i / (float)(spanLen - 1));
                float yp = mapY(clamp(module->trace[i], -a, a));
                if (i == 0) nvgMoveTo(vg, xp, yp);
                else        nvgLineTo(vg, xp, yp);
            }
            nvgStroke(vg);

            // Current head — small dot at the last point
            float headX = x0 + W * ((float)(N - 1) / (float)(spanLen - 1));
            float headY = mapY(clamp(module->trace[N - 1], -a, a));
            nvgBeginPath(vg);
            nvgCircle(vg, headX, headY, 2.6f);
            nvgFillColor(vg, nvgRGB(245, 200, 90));
            nvgFill(vg);
        }

        nvgRestore(vg);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        char buf[80];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "v=%.2f  a=%.2f  σ=%.2f",
                      module->currentDrift(),
                      module->params[DDM::THRESHOLD_PARAM].getValue(),
                      module->params[DDM::NOISE_PARAM].getValue());
        nvgText(vg, 4, 3, buf, nullptr);

        float acc = (module->trials > 0) ? (float)module->correct / module->trials : 0.f;
        std::snprintf(buf, sizeof(buf), "n=%d  acc=%.0f%%  RT=%.2fs",
                      module->trials, acc * 100.f, module->lastRT);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

struct DDMWidget : ModuleWidget {
    DDMWidget(DDM* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/DDM.svg")));
        addChild(new ModuleTitle("DDM", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "DRIFT"); labels->k1(1, "a");
        labels->k1(2, "σ");     labels->k1(3, "BIAS");
        labels->k2(0, "SHUF");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "v·CV");
        labels->outSection();
        labels->out(0, "EVID"); labels->out(1, "CHO");
        labels->out(2, "RT");   labels->out(3, "ACC");
        addChild(labels);

        // Corners flush at {0, 285} (family 300px convention) so the bottom-right
        // screw clears the x=270 output-jack column.
        addChild(createWidget<ScrewSilver>(Vec(0, 0)));
        addChild(createWidget<ScrewSilver>(Vec(285, 0)));
        addChild(createWidget<ScrewSilver>(Vec(0, 365)));
        addChild(createWidget<ScrewSilver>(Vec(285, 365)));

        auto* view = new DDMView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, DDM::DRIFT_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, DDM::THRESHOLD_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, DDM::NOISE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, DDM::BIAS_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(45, 294), module, DDM::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(45, 280), module, DDM::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, DDM::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, DDM::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, DDM::DRIFT_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, DDM::EVID_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, DDM::CHOICE_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(105, 358), module, DDM::UPPER_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(135, 358), module, DDM::LOWER_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, DDM::RT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, DDM::ACC_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "DDM",
            {"Ratcliff drift-diffusion model of decision-making.",
             "A noisy evidence accumulator drifts until it hits an upper",
             "or lower boundary — outputs choice, response time and trace."},
            "Gauge (preset 'RT (ms)' for RT output), Frame (mean RT)");
    }
};

Model* modelDDM = createModel<DDM, DDMWidget>("DDM");
