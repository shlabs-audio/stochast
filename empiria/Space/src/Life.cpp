#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

// ============================================================================
// Life — 2-D cellular automaton on a 24 × 24 toroidal grid.
//
//   The default rule is Conway's Life (B3/S23):
//     - a dead cell with exactly 3 live Moore neighbours is born;
//     - a live cell with 2 or 3 live Moore neighbours survives;
//     - all other cells die.
//
//   Other rules are exposed as right-click presets:
//     - HighLife              B36/S23
//     - Day & Night           B3678/S34678
//     - Majority (smoothing)  B5678/S45678
//     - Voter model           B45678/S45678
//
//   The whole grid updates every tick (CLOCK or internal 8 Hz).
// ============================================================================

struct Life : Module {
    static constexpr int   kGrid     = 24;
    static constexpr int   kCells    = kGrid * kGrid;
    static constexpr float kInternalHz = 8.f;

    enum ParamId {
        DENSITY_PARAM,
        SPEED_PARAM,
        WRAP_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        DENSITY_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        ALIVE_FRAC_OUTPUT,
        DELTA_OUTPUT,
        OSC_OUTPUT,
        ROW_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, OSC_LIGHT, NUM_LIGHTS };

    // Rule encoding: 18-bit pairs of (birth-mask, survive-mask), each over
    // neighbour-counts 0..8. Bit k = neighbour count k.
    int birthMask   = (1 << 3);
    int surviveMask = (1 << 2) | (1 << 3);

    std::array<std::array<uint8_t, kGrid>, kGrid> grid{}, next{};
    int generation = 0;
    int aliveCount = 0;
    int prevAlive  = 0;
    float oscPulse = 0.f;

    // Cycle-detection: keep a tiny ring of recent aliveCount values to spot
    // period-2 / period-3 still lives and oscillators.
    static constexpr int kHistLen = 8;
    std::array<int, kHistLen> aliveHist{};
    int histHead = 0;

    float internalPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;
    std::mt19937 rng;
    uint32_t seedVal = 0x4ABCDEF1u;

    Life() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(DENSITY_PARAM, 0.f, 1.f, 0.30f,
                    "Random-seed density (fraction live)");
        configParam(SPEED_PARAM, 1.f, 32.f, 1.f, "Substeps per tick");
        paramQuantities[SPEED_PARAM]->snapEnabled = true;
        configSwitch(WRAP_PARAM, 0.f, 1.f, 1.f, "Boundary", {"Dead", "Toroidal (wrap)"});
        configButton(SHUFFLE_PARAM, "Re-seed grid at current density");
        configInput(CLOCK_INPUT, "Clock — drives generation steps (internal 8 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset to a fresh random seed");
        configInput(DENSITY_CV_INPUT, "Density CV (0..10 V offsets the knob)");
        configOutput(ALIVE_FRAC_OUTPUT, "Live fraction (0..10 V = 0..1)");
        configOutput(DELTA_OUTPUT, "Births − deaths this tick, scaled");
        configOutput(OSC_OUTPUT, "Pulse when an oscillation cycle is detected");
        configOutput(ROW_OUTPUT, "Per-row live count (polyphonic, 16 of 24 rows, scaled)");
        randomize(0.30f);
    }

    void onReset() override { seedVal = 0x4ABCDEF1u; randomize(currentDensity()); }

    float currentDensity() {
        float d = clamp(params[DENSITY_PARAM].getValue(), 0.f, 1.f);
        if (inputs[DENSITY_CV_INPUT].isConnected())
            d += inputs[DENSITY_CV_INPUT].getVoltage() / 10.f;
        return clamp(d, 0.f, 1.f);
    }

    void randomize(float density) {
        rng.seed(seedVal);
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x)
                grid[y][x] = (ud(rng) < density) ? 1 : 0;
        generation = 0;
        aliveHist.fill(-1);
        histHead = 0;
        recount();
    }

    void recount() {
        aliveCount = 0;
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x)
                aliveCount += grid[y][x];
    }

    int neighbours(int x, int y, bool wrap) const {
        int count = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int nx = x + dx, ny = y + dy;
                if (wrap) {
                    nx = (nx + kGrid) % kGrid;
                    ny = (ny + kGrid) % kGrid;
                } else if (nx < 0 || nx >= kGrid || ny < 0 || ny >= kGrid) {
                    continue;
                }
                count += grid[ny][nx];
            }
        return count;
    }

    void stepCA() {
        bool wrap = params[WRAP_PARAM].getValue() > 0.5f;
        prevAlive = aliveCount;
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                int n = neighbours(x, y, wrap);
                int mask = grid[y][x] ? surviveMask : birthMask;
                next[y][x] = (mask >> n) & 1;
            }
        }
        for (int y = 0; y < kGrid; ++y) grid[y] = next[y];
        ++generation;
        recount();

        // Detect short-period oscillation by repeated aliveCount values
        aliveHist[histHead] = aliveCount;
        histHead = (histHead + 1) % kHistLen;
        // Match: current aliveCount appears at offset 2 or 3 in recent history
        for (int p = 2; p <= 4; ++p) {
            int hit = 0;
            for (int k = 0; k < kHistLen - p; ++k) {
                int a = (histHead - 1 - k + kHistLen) % kHistLen;
                int b = (a - p + kHistLen) % kHistLen;
                if (aliveHist[a] == aliveHist[b] && aliveHist[a] >= 0) ++hit;
            }
            if (hit >= 2) { oscPulse = 0.025f; break; }
        }
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            seedVal = seedVal * 1664525u + 1013904223u;
            randomize(currentDensity());
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            seedVal = seedVal * 1664525u + 1013904223u;
            randomize(currentDensity());
        }
        lights[SHUFFLE_LIGHT].setBrightness(
            params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        bool tick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick = true;
        } else {
            internalPhase += args.sampleTime * kInternalHz;
            if (internalPhase >= 1.f) { internalPhase -= 1.f; tick = true; }
        }
        if (tick) {
            int substeps = clamp((int)std::round(params[SPEED_PARAM].getValue()), 1, 32);
            for (int s = 0; s < substeps; ++s) stepCA();
        }

        if (oscPulse > 0.f) oscPulse -= args.sampleTime;

        float fAlive = (float)aliveCount / kCells;
        outputs[ALIVE_FRAC_OUTPUT].setVoltage(clamp(fAlive, 0.f, 1.f) * 10.f);
        float delta = (float)(aliveCount - prevAlive) / kCells;
        outputs[DELTA_OUTPUT].setVoltage(clamp(delta * 10.f, -5.f, 5.f));
        outputs[OSC_OUTPUT].setVoltage(oscPulse > 0.f ? 10.f : 0.f);
        lights[OSC_LIGHT].setBrightness(oscPulse > 0.f ? 1.f : 0.f);

        // Per-row live counts on 16 of the 24 rows (every 1.5 rows)
        outputs[ROW_OUTPUT].setChannels(16);
        for (int c = 0; c < 16; ++c) {
            int y = (c * (kGrid - 1)) / 15;
            int rowAlive = 0;
            for (int x = 0; x < kGrid; ++x) rowAlive += grid[y][x];
            outputs[ROW_OUTPUT].setVoltage(10.f * (float)rowAlive / kGrid, c);
        }
    }

    // Right-click rule presets
    void setRule(int b, int s) { birthMask = b; surviveMask = s; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "birthMask",   json_integer(birthMask));
        json_object_set_new(root, "surviveMask", json_integer(surviveMask));
        json_object_set_new(root, "seedVal",     json_integer((json_int_t)seedVal));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "birthMask"))   birthMask   = json_integer_value(j);
        if (auto* j = json_object_get(root, "surviveMask")) surviveMask = json_integer_value(j);
        if (auto* j = json_object_get(root, "seedVal"))     seedVal     = (uint32_t)json_integer_value(j);
    }
};

// ============================================================================
// Visualization
// ============================================================================

struct LifeView : LightWidget {
    Life* module = nullptr;

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
                    "CELLULAR  AUTOMATON", nullptr);
            return;
        }

        float pad = 6.f, topStripH = 12.f, botStripH = 12.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float side = std::min(W, H);
        float x0 = pad + (W - side) / 2.f;
        float y0 = pad + topStripH + (H - side) / 2.f;
        float cell = side / Life::kGrid;

        // Live cells
        nvgFillColor(vg, nvgRGB(110, 200, 220));
        for (int y = 0; y < Life::kGrid; ++y) {
            for (int x = 0; x < Life::kGrid; ++x) {
                if (!module->grid[y][x]) continue;
                nvgBeginPath(vg);
                nvgRect(vg, x0 + x * cell + 0.5f, y0 + y * cell + 0.5f,
                        cell - 1.f, cell - 1.f);
                nvgFill(vg);
            }
        }

        // Subtle grid lines
        nvgStrokeColor(vg, nvgRGBA(40, 46, 64, 100));
        nvgStrokeWidth(vg, 0.3f);
        nvgBeginPath(vg);
        for (int i = 0; i <= Life::kGrid; ++i) {
            float xx = x0 + i * cell;
            float yy = y0 + i * cell;
            nvgMoveTo(vg, xx, y0);    nvgLineTo(vg, xx, y0 + side);
            nvgMoveTo(vg, x0, yy);    nvgLineTo(vg, x0 + side, yy);
        }
        nvgStroke(vg);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — rule + generation
        char buf[80];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        // Find rule name
        const char* ruleName = "custom";
        if (module->birthMask == (1 << 3) && module->surviveMask == ((1 << 2) | (1 << 3)))
            ruleName = "B3/S23  Life";
        else if (module->birthMask == ((1 << 3) | (1 << 6)) && module->surviveMask == ((1 << 2) | (1 << 3)))
            ruleName = "B36/S23  HighLife";
        else if (module->birthMask == ((1 << 3) | (1 << 6) | (1 << 7) | (1 << 8)) &&
                 module->surviveMask == ((1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 8)))
            ruleName = "Day & Night";
        else if (module->birthMask == ((1 << 5) | (1 << 6) | (1 << 7) | (1 << 8)) &&
                 module->surviveMask == ((1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8)))
            ruleName = "Majority";
        else if (module->birthMask == ((1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8)) &&
                 module->surviveMask == ((1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8)))
            ruleName = "Voter";
        nvgText(vg, 4, 3, ruleName, nullptr);

        std::snprintf(buf, sizeof(buf), "gen %d  alive %d", module->generation, module->aliveCount);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct LifeWidget : ModuleWidget {
    LifeWidget(Life* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Life.svg")));
        addChild(new ModuleTitle("LIFE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "DENSITY"); labels->k1(1, "SPEED");
        labels->k1(2, "WRAP");    labels->k1(3, "SHUF");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "DENS·CV");
        labels->outSection();
        labels->out(0, "ALIVE"); labels->out(1, "Δ");
        labels->out(2, "OSC");   labels->out(3, "ROW");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new LifeView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Life::DENSITY_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Life::SPEED_PARAM));
        addParam(createParamCentered<CKSS>(
            Vec(195, 258), module, Life::WRAP_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 258), module, Life::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Life::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Life::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Life::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Life::DENSITY_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Life::ALIVE_FRAC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Life::DELTA_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Life::OSC_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(222, 358), module, Life::OSC_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Life::ROW_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Life*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("CA rule preset"));

        struct RuleItem : MenuItem {
            Life* m;
            int b, s;
            void onAction(const event::Action&) override { m->setRule(b, s); }
        };
        auto add = [&](const char* name, int b, int s) {
            auto* it = new RuleItem;
            it->text = name;
            it->m = m;
            it->b = b;
            it->s = s;
            it->rightText = (m->birthMask == b && m->surviveMask == s) ? "✓" : "";
            menu->addChild(it);
        };
        add("Conway Life  B3/S23",      (1<<3),                   (1<<2)|(1<<3));
        add("HighLife  B36/S23",        (1<<3)|(1<<6),            (1<<2)|(1<<3));
        add("Day & Night  B3678/S34678", (1<<3)|(1<<6)|(1<<7)|(1<<8),
                                         (1<<3)|(1<<4)|(1<<6)|(1<<7)|(1<<8));
        add("Majority  B5678/S45678",   (1<<5)|(1<<6)|(1<<7)|(1<<8),
                                         (1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8));
        add("Voter  B45678/S45678",     (1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8),
                                         (1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8));

        appendAboutMenu(menu, "Life",
            {"Conway's Game of Life, plus tunable B/S rule strings.",
             "A 2D cellular automaton; outputs include alive fraction",
             "and aggregated activity per row."},
            "Seed (reproducible starting state), Frame (alive fraction)");
    }
};

Model* modelLife = createModel<Life, LifeWidget>("Life");
