#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <osdialog.h>

// ============================================================================
// Tape — record-and-replay buffer for CV streams.
//
//   Three modes, selected by MODE knob:
//     REC   continuously records SIG into a ring buffer of size N. Acts
//           as a sliding-window snapshot of the most recent N samples.
//     PLAY  replays the buffer once from the start, outputting one
//           sample per clock tick. Stops at the end (ACTIVE drops).
//     LOOP  replays continuously, wrapping at the buffer end (WRAP fires).
//
//   The TRIG input restarts the operation: in REC it resets the write
//   cursor, in PLAY / LOOP it restarts playback from the beginning.
//
//   Polyphonic SIG is preserved channel-by-channel — record up to 16
//   parallel CV streams.
//
//   The point: capture one synthetic dataset and feed it into many
//   parallel analyses (Frame / Regress / Test / Boot / Lag) without the
//   data changing between runs.
// ============================================================================

struct Tape : Module {
    enum Mode { MODE_REC = 0, MODE_PLAY, MODE_LOOP, NUM_MODES };

    enum ParamId {
        MODE_PARAM,
        LENGTH_PARAM,
        SPEED_PARAM,
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
        SIG_OUTPUT,
        POSITION_OUTPUT,
        WRAP_OUTPUT,
        ACTIVE_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, ACTIVE_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxBuf       = 4096;
    static constexpr int   kMaxCh        = 16;
    static constexpr float kWrapPulseSec = 0.010f;

    std::array<std::array<float, kMaxBuf>, kMaxCh> buf{};
    int recCursor = 0;
    int totalRecorded = 0;
    int channels = 1;
    float playCursorF = 0.f;     // float to allow speed != 1.0
    float wrapPulse = 0.f;
    bool playFinished = false;

    dsp::SchmittTrigger clockTrig, resetTrig, trigTrig, shuffleBtn;
    float internalClockPhase = 0.f;       // free-run when CLOCK unpatched
    static constexpr float kInternalRateHz = 200.f;

    Tape() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, (float)(NUM_MODES - 1), 0.f, "Mode",
                     {"Record", "Play (one-shot)", "Loop"});
        configParam(LENGTH_PARAM, 4.f, (float)kMaxBuf, 256.f, "Buffer length (samples)");
        paramQuantities[LENGTH_PARAM]->snapEnabled = true;
        configParam(SPEED_PARAM, 0.25f, 4.f, 1.f, "Playback speed (× clock)");
        configButton(SHUFFLE_PARAM, "Clear buffer + reset cursors");
        configInput(CLOCK_INPUT, "Clock — drives REC writes and PLAY/LOOP reads");
        configInput(RESET_INPUT, "Reset (clear buffer + cursors)");
        configInput(SIG_INPUT, "Signal to record (polyphonic OK)");
        configInput(TRIG_INPUT, "Restart trigger");
        configOutput(SIG_OUTPUT, "Replayed signal (pass-through during REC)");
        configOutput(POSITION_OUTPUT, "Playback cursor position (0..10 V)");
        configOutput(WRAP_OUTPUT, "Loop-wrap trigger (LOOP mode)");
        configOutput(ACTIVE_OUTPUT, "Active gate (recording or playing)");
    }

    void clearBuffer() {
        for (auto& ch : buf) ch.fill(0.f);
        recCursor = 0;
        playCursorF = 0.f;
        totalRecorded = 0;
        channels = 1;
        wrapPulse = 0.f;
        playFinished = false;
    }

    void onReset() override { clearBuffer(); }

    int currentMode()   { return clamp((int)std::round(params[MODE_PARAM].getValue()), 0, NUM_MODES - 1); }
    int currentLength() { return clamp((int)std::round(params[LENGTH_PARAM].getValue()), 4, kMaxBuf); }
    float currentSpeed(){ return clamp(params[SPEED_PARAM].getValue(), 0.25f, 4.f); }

    int effectiveN() {
        int mode = currentMode();
        if (mode == MODE_REC) return std::min(currentLength(), kMaxBuf);
        // PLAY / LOOP read whatever has been recorded so far, up to the
        // current LENGTH knob value (so user can change LENGTH to play
        // back a shorter or longer slice of the recording)
        return std::min({currentLength(), totalRecorded, kMaxBuf});
    }

    void onClockTick() {
        int mode = currentMode();
        int N = currentLength();
        float speed = currentSpeed();

        if (mode == MODE_REC) {
            int ch = inputs[SIG_INPUT].isConnected()
                     ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
            channels = std::min(ch, kMaxCh);
            for (int c = 0; c < channels; ++c) {
                buf[c][recCursor] =
                    inputs[SIG_INPUT].isConnected() ? inputs[SIG_INPUT].getVoltage(c) : 0.f;
            }
            recCursor = (recCursor + 1) % N;
            if (totalRecorded < N) ++totalRecorded;
            // While recording, the play cursor sits at the latest sample
            playCursorF = (float)((recCursor + N - 1) % N);
            playFinished = false;
        } else {
            int nEff = effectiveN();
            if (nEff <= 0) return;
            playCursorF += speed;
            if (mode == MODE_LOOP) {
                while (playCursorF >= nEff) {
                    playCursorF -= nEff;
                    wrapPulse = kWrapPulseSec;
                }
                playFinished = false;
            } else {
                // PLAY one-shot — clamp at the end
                if (playCursorF >= nEff) {
                    playCursorF = (float)nEff - 1.f;
                    playFinished = true;
                }
            }
        }
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) clearBuffer();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) clearBuffer();
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        if (trigTrig.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) {
            int mode = currentMode();
            if (mode == MODE_REC) {
                recCursor = 0;
                totalRecorded = 0;
            } else {
                playCursorF = 0.f;
                playFinished = false;
            }
        }

        // Clock-driven update; free-run at kInternalRateHz when unpatched.
        bool tick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            tick = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
        } else {
            internalClockPhase += kInternalRateHz * args.sampleTime;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick = true;
            }
        }
        if (tick) onClockTick();

        // SIG output
        int mode = currentMode();
        if (mode == MODE_REC) {
            int ch = inputs[SIG_INPUT].isConnected()
                     ? std::max(1, inputs[SIG_INPUT].getChannels()) : 1;
            outputs[SIG_OUTPUT].setChannels(ch);
            for (int c = 0; c < ch; ++c) {
                outputs[SIG_OUTPUT].setVoltage(
                    inputs[SIG_INPUT].isConnected() ? inputs[SIG_INPUT].getVoltage(c) : 0.f, c);
            }
        } else {
            int nEff = effectiveN();
            outputs[SIG_OUTPUT].setChannels(channels);
            if (nEff <= 0) {
                for (int c = 0; c < channels; ++c) outputs[SIG_OUTPUT].setVoltage(0.f, c);
            } else {
                int idx = clamp((int)std::floor(playCursorF), 0, nEff - 1);
                for (int c = 0; c < channels; ++c) {
                    outputs[SIG_OUTPUT].setVoltage(buf[c][idx], c);
                }
            }
        }

        // Position output (0..10 V proportional to play cursor / length)
        int N = std::max(1, currentLength());
        float pos = std::min(playCursorF, (float)N) / (float)N;
        outputs[POSITION_OUTPUT].setVoltage(clamp(pos, 0.f, 1.f) * 10.f);

        // Wrap pulse (only in LOOP)
        if (wrapPulse > 0.f) wrapPulse -= args.sampleTime;
        outputs[WRAP_OUTPUT].setVoltage(wrapPulse > 0.f ? 10.f : 0.f);

        // Active gate
        bool active = (mode == MODE_REC) ||
                      (mode == MODE_LOOP) ||
                      (mode == MODE_PLAY && !playFinished && totalRecorded > 0);
        outputs[ACTIVE_OUTPUT].setVoltage(active ? 10.f : 0.f);
        lights[ACTIVE_LIGHT].setBrightness(active ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — waveform of the recorded buffer (first channel), with the
// record and play cursors drawn over it.
// ============================================================================

struct TapeView : LightWidget {
    Tape* module = nullptr;

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
                    "RECORDED BUFFER", nullptr);
            return;
        }

        const int N = module->currentLength();
        const int nRec = module->totalRecorded;
        const int mode = module->currentMode();

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 14.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad, y0 = pad + topStripH;

        // Find buffer amplitude range over recorded data (first channel)
        float lo = -0.1f, hi = 0.1f;
        for (int i = 0; i < nRec; ++i) {
            float v = module->buf[0][i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        float range = std::max(0.1f, hi - lo);
        lo -= range * 0.05f;
        hi += range * 0.05f;

        auto mapY = [&](float v) {
            return y0 + H * (1.f - (v - lo) / (hi - lo));
        };

        // Centre line (V = 0) if visible
        if (lo <= 0.f && hi >= 0.f) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x0, mapY(0.f));
            nvgLineTo(vg, x0 + W, mapY(0.f));
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, 130));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        // Waveform — first channel
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        nvgBeginPath(vg);
        bool first = true;
        for (int i = 0; i < N; ++i) {
            float xp = x0 + W * ((float)i / std::max(1, N - 1));
            float yp = mapY(i < nRec ? module->buf[0][i] : 0.f);
            if (first) { nvgMoveTo(vg, xp, yp); first = false; }
            else        nvgLineTo(vg, xp, yp);
        }
        nvgStrokeColor(vg, nvgRGB(80, 165, 220));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        // Record cursor — green vertical line (only in REC mode)
        if (mode == Tape::MODE_REC) {
            float xc = x0 + W * ((float)module->recCursor / std::max(1, N));
            nvgBeginPath(vg);
            nvgMoveTo(vg, xc, y0);
            nvgLineTo(vg, xc, y0 + H);
            nvgStrokeColor(vg, nvgRGB(120, 220, 140));
            nvgStrokeWidth(vg, 1.4f);
            nvgStroke(vg);
        }

        // Play cursor — amber vertical line (in PLAY / LOOP modes)
        if (mode != Tape::MODE_REC && nRec > 0) {
            float xp = x0 + W * (module->playCursorF / std::max(1, N));
            nvgBeginPath(vg);
            nvgMoveTo(vg, xp, y0);
            nvgLineTo(vg, xp, y0 + H);
            nvgStrokeColor(vg, nvgRGB(245, 200, 90));
            nvgStrokeWidth(vg, 1.6f);
            nvgStroke(vg);
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip
        const char* modeNames[Tape::NUM_MODES] = {"REC", "PLAY", "LOOP"};
        char buf[64];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "%s  N=%d  rec=%d  ch=%d",
                      modeNames[mode], N, nRec, module->channels);
        nvgText(vg, 4, 3, buf, nullptr);

        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "%.2f×", module->currentSpeed());
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom strip — readouts
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGBA(110, 120, 140, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f V", lo);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "%.2f V", hi);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct TapeWidget : ModuleWidget {
    TapeWidget(Tape* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Tape.svg")));
        addChild(new ModuleTitle("TAPE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "MODE");  labels->k1(1, "LENGTH");
        labels->k1(2, "SPEED");
        labels->k2(3, "CLEAR");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "SIG");   labels->in(3, "TRIG");
        labels->outSection();
        labels->out(0, "SIG");  labels->out(1, "POS");
        labels->out(2, "WRAP"); labels->out(3, "ACT");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new TapeView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<CKSSThree>(
            Vec(45,  258), module, Tape::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Tape::LENGTH_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Tape::SPEED_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Tape::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Tape::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Tape::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Tape::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Tape::SIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(270, 327), module, Tape::TRIG_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Tape::SIG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Tape::POSITION_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Tape::WRAP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Tape::ACTIVE_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(243, 358), module, Tape::ACTIVE_LIGHT));
    }

    // Right-click menu: data export. Writes the currently-recorded buffer
    // to a CSV file the user picks. Format is one row per recorded sample,
    // columns are `sample` (zero-based index) followed by one column per
    // active polyphonic channel (`ch1`, `ch2`, …). The file is plain ASCII
    // with `.csv` extension, immediately loadable by R's `read.csv()` or
    // Python's `pandas.read_csv()`.
    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Tape*>(this->module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Data export"));

        struct CsvExportItem : MenuItem {
            Tape* m = nullptr;
            void onAction(const event::Action&) override {
                if (!m || m->totalRecorded <= 0) return;
                char* path = osdialog_file(OSDIALOG_SAVE, "", "tape.csv", nullptr);
                if (!path) return;
                std::string p = path;
                std::free(path);
                // Ensure a .csv extension.
                if (p.size() < 4 ||
                    p.substr(p.size() - 4) != ".csv") {
                    p += ".csv";
                }
                std::FILE* f = std::fopen(p.c_str(), "w");
                if (!f) return;

                const int N = m->totalRecorded;
                const int C = std::max(1, m->channels);
                // Header
                std::fprintf(f, "sample");
                for (int c = 0; c < C; ++c)
                    std::fprintf(f, ",ch%d", c + 1);
                std::fprintf(f, "\n");
                // Data — write samples in chronological order (oldest first).
                // recCursor points to the *next* write slot; if totalRecorded
                // < kMaxBuf, recording hasn't wrapped, so data is at
                // indices [0..recCursor). Otherwise the ring has wrapped,
                // and chronological order starts at recCursor and wraps.
                const bool wrapped = (m->totalRecorded >= m->currentLength());
                const int  bufN    = m->currentLength();
                for (int i = 0; i < N; ++i) {
                    int idx;
                    if (wrapped) idx = (m->recCursor + i) % bufN;
                    else         idx = i;
                    std::fprintf(f, "%d", i);
                    for (int c = 0; c < C; ++c) {
                        std::fprintf(f, ",%.6f", m->buf[c][idx]);
                    }
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        };

        auto* item = new CsvExportItem;
        item->text = "Export buffer to CSV…";
        item->rightText = (m->totalRecorded > 0)
            ? string::f("%d samples × %d ch", m->totalRecorded,
                        std::max(1, m->channels))
            : "(buffer empty)";
        item->disabled = (m->totalRecorded <= 0);
        item->m = m;
        menu->addChild(item);

        appendAboutMenu(menu, "Tape",
            {"Record-and-replay buffer for CV streams. Three modes:",
             "REC (continuous recording), PLAY (replay once), LOOP (forever).",
             "Capture one dataset and feed it into many parallel analyses."},
            "Sample (record output), Frame / Regress / Test (replay into)");
    }
};

Model* modelTape = createModel<Tape, TapeWidget>("Tape");
