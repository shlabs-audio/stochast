#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

// ============================================================================
// Schelling — 2-D segregation model on a 24 × 24 grid.
//
//   Two populations (type 0 = "red", type 1 = "blue") plus empty cells.
//   Each tick:
//     1. For every occupied cell, compute the fraction f_same of its 8 Moore
//        neighbours that are the same type (ignoring empty cells in the
//        denominator if there are any).
//     2. An agent is UNHAPPY if f_same < θ (the tolerance threshold).
//     3. Each unhappy agent relocates to a uniformly-random empty cell.
//
//   The famous result (Schelling 1971): even at θ = 0.30 — i.e. agents only
//   require 30% of their neighbours to be the same type — the steady state is
//   *highly* segregated, far more than any individual agent prefers. A
//   visual demonstration that emergent macro patterns can be very different
//   from the micro-preferences that produce them.
//
//   Outputs:
//     SEG    Schelling segregation index = mean f_same across occupied cells
//     UNHAPPY  fraction of agents currently unhappy
//     LOCAL  per-quadrant mean same-type fraction (polyphonic, 16 ch)
//     QUIET  gate: high when no agents moved last tick (equilibrium reached)
// ============================================================================

struct Schelling : Module {
    static constexpr int kGrid     = 24;
    static constexpr int kCells    = kGrid * kGrid;
    static constexpr float kInternalHz = 8.f;

    enum ParamId {
        TOLERANCE_PARAM,    // θ
        OCCUPANCY_PARAM,    // fraction of cells occupied
        BALANCE_PARAM,      // fraction of occupied that are type 0
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        TOLERANCE_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        SEG_OUTPUT,
        UNHAPPY_OUTPUT,
        LOCAL_OUTPUT,
        QUIET_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, QUIET_LIGHT, NUM_LIGHTS };

    // -1 = empty; 0/1 = types
    std::array<std::array<int8_t, kGrid>, kGrid> grid{};
    int generation = 0;
    int movedLastTick = 0;
    float segIndex = 0.f;
    float unhappyFrac = 0.f;

    float internalPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;
    std::mt19937 rng;
    uint32_t seedVal = 0x4ABCDEF1u;

    Schelling() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(TOLERANCE_PARAM, 0.f, 1.f, 0.30f,
                    "Tolerance θ — minimum same-type neighbour fraction agents tolerate");
        configParam(OCCUPANCY_PARAM, 0.1f, 0.95f, 0.85f,
                    "Occupancy — fraction of cells initially occupied");
        configParam(BALANCE_PARAM, 0.f, 1.f, 0.5f,
                    "Balance — fraction of occupied cells that are type 0");
        configButton(SHUFFLE_PARAM, "Re-seed grid");
        configInput(CLOCK_INPUT, "Clock — drives a relocation round (internal 8 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset to fresh random configuration");
        configInput(TOLERANCE_CV_INPUT, "Tolerance CV (0..10 V adds to knob)");
        configOutput(SEG_OUTPUT, "Mean same-type neighbour fraction (0..10 V)");
        configOutput(UNHAPPY_OUTPUT, "Unhappy fraction (0..10 V)");
        configOutput(LOCAL_OUTPUT, "Per-quadrant same-type fraction (polyphonic, 16 ch)");
        configOutput(QUIET_OUTPUT, "Equilibrium gate (no agents moved last tick)");
        randomize();
    }

    void onReset() override { seedVal = 0x4ABCDEF1u; randomize(); }

    float currentTolerance() {
        float t = clamp(params[TOLERANCE_PARAM].getValue(), 0.f, 1.f);
        if (inputs[TOLERANCE_CV_INPUT].isConnected())
            t += inputs[TOLERANCE_CV_INPUT].getVoltage() / 10.f;
        return clamp(t, 0.f, 1.f);
    }

    void randomize() {
        rng.seed(seedVal);
        float occ = clamp(params[OCCUPANCY_PARAM].getValue(), 0.1f, 0.95f);
        float bal = clamp(params[BALANCE_PARAM].getValue(), 0.f, 1.f);
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                if (ud(rng) < occ)
                    grid[y][x] = (ud(rng) < bal) ? 0 : 1;
                else
                    grid[y][x] = -1;
            }
        generation = 0;
        movedLastTick = 0;
        recomputeIndex();
    }

    // (count_same, count_neighbours_total) for cell (x,y)
    std::pair<int,int> sameAndTotal(int x, int y) const {
        int8_t my = grid[y][x];
        int same = 0, total = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int nx = (x + dx + kGrid) % kGrid;
                int ny = (y + dy + kGrid) % kGrid;
                int8_t v = grid[ny][nx];
                if (v < 0) continue;
                ++total;
                if (v == my) ++same;
            }
        return {same, total};
    }

    void recomputeIndex() {
        double accumF = 0.0;
        int    occupied = 0;
        int    unhappy = 0;
        float  theta = currentTolerance();
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                if (grid[y][x] < 0) continue;
                ++occupied;
                auto [same, total] = sameAndTotal(x, y);
                float f = (total > 0) ? (float)same / total : 1.f;
                accumF += f;
                if (f < theta) ++unhappy;
            }
        segIndex = (occupied > 0) ? (float)(accumF / occupied) : 0.f;
        unhappyFrac = (occupied > 0) ? (float)unhappy / occupied : 0.f;
    }

    void stepCA() {
        float theta = currentTolerance();
        // Gather unhappy and empty positions
        std::vector<std::pair<int,int>> unhappy;
        std::vector<std::pair<int,int>> empty;
        unhappy.reserve(64);
        empty.reserve(64);
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                if (grid[y][x] < 0) {
                    empty.push_back({x, y});
                } else {
                    auto [same, total] = sameAndTotal(x, y);
                    float f = (total > 0) ? (float)same / total : 1.f;
                    if (f < theta) unhappy.push_back({x, y});
                }
            }

        std::shuffle(unhappy.begin(), unhappy.end(), rng);
        std::shuffle(empty.begin(), empty.end(), rng);

        int moves = 0;
        for (auto& src : unhappy) {
            if (empty.empty()) break;
            // Pop the next empty cell
            auto dst = empty.back();
            empty.pop_back();
            int8_t typ = grid[src.second][src.first];
            grid[src.second][src.first] = -1;
            grid[dst.second][dst.first] = typ;
            empty.push_back(src);     // src is now empty
            ++moves;
        }
        movedLastTick = moves;
        ++generation;
        recomputeIndex();
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            seedVal = seedVal * 1664525u + 1013904223u;
            randomize();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            seedVal = seedVal * 1664525u + 1013904223u;
            randomize();
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
        if (tick) stepCA();

        outputs[SEG_OUTPUT].setVoltage(clamp(segIndex, 0.f, 1.f) * 10.f);
        outputs[UNHAPPY_OUTPUT].setVoltage(clamp(unhappyFrac, 0.f, 1.f) * 10.f);

        bool quiet = (movedLastTick == 0 && generation > 1);
        outputs[QUIET_OUTPUT].setVoltage(quiet ? 10.f : 0.f);
        lights[QUIET_LIGHT].setBrightness(quiet ? 1.f : 0.f);

        // Per-quadrant local same-type fraction. Subdivide the grid into a 4×4
        // tile pattern, take mean f_same over each tile → 16 voices.
        outputs[LOCAL_OUTPUT].setChannels(16);
        const int tile = kGrid / 4;     // 6
        for (int qy = 0; qy < 4; ++qy)
            for (int qx = 0; qx < 4; ++qx) {
                double accum = 0.0; int n = 0;
                for (int dy = 0; dy < tile; ++dy)
                    for (int dx = 0; dx < tile; ++dx) {
                        int x = qx * tile + dx, y = qy * tile + dy;
                        if (grid[y][x] < 0) continue;
                        auto [s, t] = sameAndTotal(x, y);
                        accum += (t > 0) ? (double)s / t : 1.0;
                        ++n;
                    }
                float v = (n > 0) ? (float)(accum / n) : 0.f;
                outputs[LOCAL_OUTPUT].setVoltage(v * 10.f, qy * 4 + qx);
            }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seedVal", json_integer((json_int_t)seedVal));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "seedVal"))
            seedVal = (uint32_t)json_integer_value(j);
    }
};

// ============================================================================
// Visualization
// ============================================================================

struct SchellingView : LightWidget {
    Schelling* module = nullptr;

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
                    "SEGREGATION  GRID", nullptr);
            return;
        }

        float pad = 6.f, topStripH = 12.f, botStripH = 12.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float side = std::min(W, H);
        float x0 = pad + (W - side) / 2.f;
        float y0 = pad + topStripH + (H - side) / 2.f;
        float cell = side / Schelling::kGrid;

        NVGcolor c0 = nvgRGB(240, 110, 110);
        NVGcolor c1 = nvgRGB(110, 165, 240);
        for (int y = 0; y < Schelling::kGrid; ++y) {
            for (int x = 0; x < Schelling::kGrid; ++x) {
                int8_t v = module->grid[y][x];
                if (v < 0) continue;
                nvgBeginPath(vg);
                nvgRect(vg, x0 + x * cell + 0.5f, y0 + y * cell + 0.5f,
                        cell - 1.f, cell - 1.f);
                nvgFillColor(vg, v == 0 ? c0 : c1);
                nvgFill(vg);
            }
        }

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
        std::snprintf(buf, sizeof(buf), "θ=%.2f", module->currentTolerance());
        nvgText(vg, 4, 3, buf, nullptr);

        std::snprintf(buf, sizeof(buf), "seg=%.2f  unh=%.0f%%  gen %d",
                      module->segIndex, 100.f * module->unhappyFrac, module->generation);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

struct SchellingWidget : ModuleWidget {
    SchellingWidget(Schelling* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Schelling.svg")));
        addChild(new ModuleTitle("SCHELLING", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "θ");   labels->k1(1, "OCC");
        labels->k1(2, "BAL"); labels->k1(3, "SHUF");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "θ·CV");
        labels->outSection();
        labels->out(0, "SEG");   labels->out(1, "UNH");
        labels->out(2, "LOCAL"); labels->out(3, "QUIET");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new SchellingView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Schelling::TOLERANCE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Schelling::OCCUPANCY_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Schelling::BALANCE_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 258), module, Schelling::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Schelling::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Schelling::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Schelling::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Schelling::TOLERANCE_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Schelling::SEG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Schelling::UNHAPPY_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Schelling::LOCAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Schelling::QUIET_OUTPUT));
        // In the gap between the LOCAL and QUIET ports, clear of the 'QUIET' label
        addChild(createLightCentered<SmallLight<GreenLight>>(
            Vec(222, 358), module, Schelling::QUIET_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Schelling",
            {"Schelling segregation model. Two groups on a 2D grid;",
             "agents move when fewer than τ% of neighbours match.",
             "Sharp segregation emerges from mild preferences."},
            "Frame (track segregation index), Seed (reproducible)");
    }
};

Model* modelSchelling = createModel<Schelling, SchellingWidget>("Schelling");
