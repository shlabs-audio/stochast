#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <queue>
#include <random>

// ============================================================================
// Outbreak — SIR epidemic on a network.
//
//   Instead of mass-action mixing (β·I/N applied uniformly across all
//   susceptibles), infection spreads along graph edges: each S agent has
//   probability 1 − (1 − β)^k of
//   becoming infected this tick, where k is the number of its infected
//   neighbours. Recovery is the same as SIR: P(I → R) = γ.
//
//   The graph is one of three canonical types (Watts-Strogatz,
//   Erdős–Rényi, Barabási–Albert), generated identically to the Network
//   module. Network topology dramatically changes outbreak dynamics:
//   small-world graphs spread fast via shortcuts; preferential-attachment
//   hubs become superspreaders.
//
//   Visualization: the network laid out on a circle, with nodes coloured
//   by their epidemic state (S = blue, I = red, R = green), plus a small
//   S/I/R trajectory at the bottom.
// ============================================================================

struct Outbreak : Module {
    enum State   { S = 0, I, R, NUM_STATES };
    enum NetType { TYPE_RING = 0, TYPE_ERDOS, TYPE_BARA, NUM_TYPES };

    enum ParamId {
        POPULATION_PARAM,
        BETA_PARAM,
        GAMMA_PARAM,
        TYPE_PARAM,
        K_PARAM,
        BETA_NET_PARAM,
        SEED_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        BETA_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        STATES_OUTPUT,
        INFECTED_OUTPUT,
        RECOVERED_OUTPUT,
        PEAK_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SEED_LIGHT, PEAK_LIGHT, NUM_LIGHTS };

    static constexpr int   kMaxN              = 16;
    static constexpr int   kHistLen           = 256;
    static constexpr float kInternalClockHz   = 8.f;
    static constexpr float kPeakPulseSec      = 0.05f;

    int N = 12;
    std::array<std::array<bool, kMaxN>, kMaxN> adj{};
    std::array<int, kMaxN> state{};
    int countS = 0, countI = 0, countR = 0;

    uint32_t graphSeed = 0x4ABCDEF1u;
    int prevPop = -1, prevK = -1, prevType = -1;
    float prevBetaNet = -1.f;
    uint32_t prevGraphSeed = 0;

    std::array<float, kHistLen> histS{}, histI{}, histR{};
    int histIdx = 0;
    int histFrameCounter = 0;

    float maxIfraction = 0.f;
    int falloffCount = 0;
    bool peakAlreadyFired = false;
    float peakPulse = 0.f;
    float internalClockPhase = 0.f;
    std::mt19937 rng{0xB12A75Eu};

    dsp::SchmittTrigger clockTrig, resetTrig, seedBtn;

    Outbreak() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 2.f, (float)kMaxN, 12.f, "Population N");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(BETA_PARAM,  0.f, 1.f, 0.25f, "Per-contact infection rate β");
        configParam(GAMMA_PARAM, 0.f, 1.f, 0.10f, "Recovery rate γ");
        configSwitch(TYPE_PARAM, 0.f, (float)(NUM_TYPES - 1), 0.f, "Network type",
                     {"Ring (Watts-Strogatz)", "Erdős–Rényi", "Barabási–Albert"});
        configParam(K_PARAM, 1.f, 7.f, 3.f, "K (RING: neighbours per side; BARA: seed connections)");
        paramQuantities[K_PARAM]->snapEnabled = true;
        configParam(BETA_NET_PARAM, 0.f, 1.f, 0.10f, "Network β (rewire prob / edge prob)");
        configButton(SEED_PARAM, "Seed one infection + regenerate graph");
        configInput(CLOCK_INPUT, "Clock (free-runs at 8 Hz)");
        configInput(RESET_INPUT, "Reset epidemic to S");
        configInput(BETA_CV_INPUT, "β CV (±10 V → ±1 added to knob)");
        configOutput(STATES_OUTPUT, "Per-agent state (poly: S=0, I=5, R=10 V)");
        configOutput(INFECTED_OUTPUT, "Fraction infected I/N (0..10 V)");
        configOutput(RECOVERED_OUTPUT, "Fraction recovered R/N (0..10 V)");
        configOutput(PEAK_OUTPUT, "Peak-of-epidemic gate");
        rng.seed(graphSeed);
        regenerateGraph();
        resetEpidemic();
    }

    // ---- graph generation (replicated from Polis Network module) ----

    void clearGraph() {
        for (auto& row : adj) row.fill(false);
    }
    void addEdge(int i, int j) {
        if (i == j) return;
        adj[i][j] = adj[j][i] = true;
    }
    bool hasEdge(int i, int j) const { return adj[i][j]; }
    void removeEdge(int i, int j) { adj[i][j] = adj[j][i] = false; }

    void buildRing(int K) {
        clearGraph();
        if (N < 2) return;
        int kEff = std::min(K, N / 2);
        for (int i = 0; i < N; ++i)
            for (int k = 1; k <= kEff; ++k) addEdge(i, (i + k) % N);
    }
    void buildWS(int K, float beta) {
        buildRing(K);
        if (N < 3) return;
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        std::uniform_int_distribution<int> uid(0, N - 1);
        int kEff = std::min(K, N / 2);
        for (int i = 0; i < N; ++i)
            for (int k = 1; k <= kEff; ++k) {
                if (ud(rng) < beta) {
                    int j = (i + k) % N;
                    removeEdge(i, j);
                    int newJ = uid(rng), safety = N * 2;
                    while (safety-- > 0 && (newJ == i || hasEdge(i, newJ))) newJ = uid(rng);
                    if (newJ != i && !hasEdge(i, newJ)) addEdge(i, newJ);
                    else                                addEdge(i, j);
                }
            }
    }
    void buildER(float p) {
        clearGraph();
        if (N < 2) return;
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int i = 0; i < N; ++i)
            for (int j = i + 1; j < N; ++j) if (ud(rng) < p) addEdge(i, j);
    }
    void buildBA(int m) {
        clearGraph();
        if (N < m + 1) {
            for (int i = 0; i < N; ++i)
                for (int j = i + 1; j < N; ++j) addEdge(i, j);
            return;
        }
        for (int i = 0; i <= m; ++i)
            for (int j = i + 1; j <= m; ++j) addEdge(i, j);
        for (int newNode = m + 1; newNode < N; ++newNode) {
            std::array<int, kMaxN> deg{}; int totalDeg = 0;
            for (int i = 0; i < newNode; ++i) {
                int d = 0;
                for (int j = 0; j < newNode; ++j) if (adj[i][j]) ++d;
                deg[i] = d; totalDeg += d;
            }
            std::array<bool, kMaxN> chosen{};
            int needed = m, safety = newNode * 10;
            while (needed > 0 && safety-- > 0 && totalDeg > 0) {
                std::uniform_int_distribution<int> uid(0, totalDeg - 1);
                int r = uid(rng), cum = 0;
                for (int i = 0; i < newNode; ++i) {
                    cum += deg[i];
                    if (r < cum) {
                        if (!chosen[i] && !hasEdge(newNode, i)) {
                            addEdge(newNode, i); chosen[i] = true;
                            totalDeg -= deg[i]; deg[i] = 0; --needed;
                        }
                        break;
                    }
                }
            }
            for (int i = 0; needed > 0 && i < newNode; ++i)
                if (!chosen[i]) { addEdge(newNode, i); chosen[i] = true; --needed; }
        }
    }

    int   currentType() { return clamp((int)std::round(params[TYPE_PARAM].getValue()), 0, NUM_TYPES - 1); }
    int   currentK()    { return clamp((int)std::round(params[K_PARAM].getValue()), 1, kMaxN - 1); }
    float currentBetaNet() { return clamp(params[BETA_NET_PARAM].getValue(), 0.f, 1.f); }

    void regenerateGraph() {
        rng.seed(graphSeed);
        int t = currentType();
        if      (t == TYPE_RING)  buildWS(currentK(), currentBetaNet());
        else if (t == TYPE_ERDOS) buildER(currentBetaNet());
        else                      buildBA(currentK());
    }

    // ---- SIR-on-graph dynamics ----

    void resetEpidemic() {
        for (int i = 0; i < kMaxN; ++i) state[i] = S;
        countS = N; countI = countR = 0;
        maxIfraction = 0.f;
        falloffCount = 0;
        peakAlreadyFired = false;
        peakPulse = 0.f;
        histS.fill(0.f); histI.fill(0.f); histR.fill(0.f);
        histIdx = 0;
    }

    void recountStates() {
        countS = countI = countR = 0;
        for (int i = 0; i < N; ++i) {
            if      (state[i] == S) ++countS;
            else if (state[i] == I) ++countI;
            else                    ++countR;
        }
    }

    void seedOne() {
        if (N == 0) return;
        // Reseed graph for a new realization
        graphSeed = graphSeed * 1664525u + 1013904223u;
        regenerateGraph();
        resetEpidemic();
        // Patient zero
        std::array<int, kMaxN> candidates;
        int nC = 0;
        for (int i = 0; i < N; ++i) if (state[i] == S) candidates[nC++] = i;
        if (nC == 0) return;
        std::uniform_int_distribution<int> uid(0, nC - 1);
        state[candidates[uid(rng)]] = I;
        recountStates();
        maxIfraction = (float)countI / std::max(1, N);
        falloffCount = 0;
        peakAlreadyFired = false;
    }

    void onReset() override {
        graphSeed = 0x4ABCDEF1u;
        regenerateGraph();
        resetEpidemic();
    }

    float currentBeta() {
        float b = clamp(params[BETA_PARAM].getValue(), 0.f, 1.f);
        if (inputs[BETA_CV_INPUT].isConnected()) {
            b += inputs[BETA_CV_INPUT].getVoltage() / 10.f;
        }
        return clamp(b, 0.f, 1.f);
    }

    void tick() {
        float beta  = currentBeta();
        float gamma = clamp(params[GAMMA_PARAM].getValue(), 0.f, 1.f);

        std::uniform_real_distribution<float> ud(0.f, 1.f);
        std::array<int, kMaxN> next;
        for (int i = 0; i < N; ++i) next[i] = state[i];
        for (int i = 0; i < N; ++i) {
            if (state[i] == S) {
                int kI = 0;
                for (int j = 0; j < N; ++j) if (adj[i][j] && state[j] == I) ++kI;
                if (kI > 0) {
                    float pInf = 1.f - std::pow(1.f - beta, (float)kI);
                    if (ud(rng) < pInf) next[i] = I;
                }
            } else if (state[i] == I) {
                if (ud(rng) < gamma) next[i] = R;
            }
        }
        for (int i = 0; i < N; ++i) state[i] = next[i];
        recountStates();

        float fInfNow = (N > 0) ? (float)countI / N : 0.f;
        if (fInfNow > maxIfraction) {
            maxIfraction = fInfNow;
            falloffCount = 0;
        } else if (maxIfraction > 0.02f && !peakAlreadyFired) {
            if (++falloffCount >= 3) {
                peakPulse = kPeakPulseSec;
                peakAlreadyFired = true;
            }
        }
    }

    void process(const ProcessArgs& args) override {
        int n = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 2, kMaxN);
        if (n != N) { N = n; resetEpidemic(); }  // regen handled by the prev* block below

        // Graph re-gen on any parameter change (including a new N)
        int k = currentK(), tp = currentType();
        float bN = currentBetaNet();
        if (N != prevPop || k != prevK || tp != prevType ||
            std::fabs(bN - prevBetaNet) > 1e-4f || graphSeed != prevGraphSeed) {
            regenerateGraph();
            prevPop = N; prevK = k; prevType = tp; prevBetaNet = bN; prevGraphSeed = graphSeed;
        }

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) resetEpidemic();
        if (seedBtn.process(params[SEED_PARAM].getValue())) seedOne();
        lights[SEED_LIGHT].setBrightness(params[SEED_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick();
        } else {
            internalClockPhase += args.sampleTime * kInternalClockHz;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick();
            }
        }

        int stride = std::max(1, (int)(args.sampleRate / 60.f));
        if (++histFrameCounter >= stride) {
            histFrameCounter = 0;
            float nn = std::max(1, N);
            histS[histIdx] = countS / nn;
            histI[histIdx] = countI / nn;
            histR[histIdx] = countR / nn;
            histIdx = (histIdx + 1) % kHistLen;
        }

        outputs[STATES_OUTPUT].setChannels(N);
        for (int i = 0; i < N; ++i) {
            float v = (state[i] == S) ? 0.f : (state[i] == I) ? 5.f : 10.f;
            outputs[STATES_OUTPUT].setVoltage(v, i);
        }
        outputs[INFECTED_OUTPUT].setVoltage((float)countI / std::max(1, N) * 10.f);
        outputs[RECOVERED_OUTPUT].setVoltage((float)countR / std::max(1, N) * 10.f);

        if (peakPulse > 0.f) peakPulse -= args.sampleTime;
        bool gate = peakPulse > 0.f;
        outputs[PEAK_OUTPUT].setVoltage(gate ? 10.f : 0.f);
        lights[PEAK_LIGHT].setBrightness(gate ? 1.f : 0.f);
    }
};

// ============================================================================
// Visualization — network laid out on a circle, nodes coloured by SIR state,
// plus a small S/I/R trajectory along the bottom of the viz area.
// ============================================================================

struct OutbreakView : LightWidget {
    Outbreak* module = nullptr;

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
            nvgText(vg, box.size.x / 2, box.size.y / 2, "SIR ON A NETWORK", nullptr);
            return;
        }

        const int N = module->N;
        const int H = Outbreak::kHistLen;
        const int wIdx = module->histIdx;

        // Fixed bands: top strip, trajectory, bottom strip; network fills the rest.
        float pad = 6.f, topStripH = 12.f, trajH = 36.f, botStripH = 8.f;
        float netH = box.size.y - 2 * pad - topStripH - trajH - botStripH;
        float cx = box.size.x / 2.f;
        float cy = pad + topStripH + netH / 2.f;
        float radius = std::min(box.size.x - 2 * pad, netH) * 0.42f;

        // Compute node positions
        std::array<float, Outbreak::kMaxN> nx, ny;
        for (int i = 0; i < N; ++i) {
            float ang = (float)i / N * 2.f * (float)M_PI - (float)M_PI / 2.f;
            nx[i] = cx + radius * std::cos(ang);
            ny[i] = cy + radius * std::sin(ang);
        }

        // Edges
        nvgLineCap(vg, NVG_ROUND);
        for (int i = 0; i < N; ++i)
            for (int j = i + 1; j < N; ++j) {
                if (!module->adj[i][j]) continue;
                bool flow = (module->state[i] == Outbreak::I && module->state[j] == Outbreak::S) ||
                            (module->state[j] == Outbreak::I && module->state[i] == Outbreak::S);
                nvgBeginPath(vg);
                nvgMoveTo(vg, nx[i], ny[i]);
                nvgLineTo(vg, nx[j], ny[j]);
                nvgStrokeColor(vg, flow ? nvgRGBA(245, 90, 90, 220)
                                        : nvgRGBA(80, 165, 220, 120));
                nvgStrokeWidth(vg, flow ? 1.4f : 0.7f);
                nvgStroke(vg);
            }

        // Nodes, coloured by state
        for (int i = 0; i < N; ++i) {
            NVGcolor c = (module->state[i] == Outbreak::S) ? nvgRGB( 90, 165, 230)
                        : (module->state[i] == Outbreak::I) ? nvgRGB(245,  90,  90)
                                                            : nvgRGB(120, 200, 140);
            nvgBeginPath(vg);
            nvgCircle(vg, nx[i], ny[i], 3.5f);
            nvgFillColor(vg, c);
            nvgFill(vg);
        }

        // Trajectory strip at the bottom of the viz area
        float trajY = box.size.y - pad - botStripH - trajH;
        float trajX0 = pad, trajW = box.size.x - 2 * pad;
        auto drawLine = [&](const std::array<float, Outbreak::kHistLen>& hist, NVGcolor col) {
            nvgBeginPath(vg);
            for (int t = 0; t < H; ++t) {
                int idx = (wIdx + t) % H;
                float v = clamp(hist[idx], 0.f, 1.f);
                float xp = trajX0 + (float)t / (H - 1) * trajW;
                float yp = trajY + trajH * (1.f - v);
                if (t == 0) nvgMoveTo(vg, xp, yp); else nvgLineTo(vg, xp, yp);
            }
            nvgStrokeColor(vg, col);
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        };
        // Faint separator
        nvgBeginPath(vg);
        nvgMoveTo(vg, trajX0, trajY);
        nvgLineTo(vg, trajX0 + trajW, trajY);
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 130));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);
        drawLine(module->histS, nvgRGB( 90, 165, 230));
        drawLine(module->histR, nvgRGB(120, 200, 140));
        drawLine(module->histI, nvgRGB(245,  90,  90));

        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — type / params / R0
        const char* typeShort[Outbreak::NUM_TYPES] = {"RING", "ERDOS", "BARA"};
        int t = module->currentType();
        float beta  = module->currentBeta();
        float gamma = clamp(module->params[Outbreak::GAMMA_PARAM].getValue(), 0.f, 1.f);
        float R0eff = (gamma > 1e-4f) ? beta / gamma : 0.f;

        char buf[80];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "%s  N=%d  β=%.2f  γ=%.2f",
                      typeShort[t], N, beta, gamma);
        nvgText(vg, 4, 3, buf, nullptr);

        if (R0eff >= 1.f) nvgFillColor(vg, nvgRGB(245, 130, 90));
        else              nvgFillColor(vg, nvgRGB(120, 200, 140));
        std::snprintf(buf, sizeof(buf), "β/γ=%.2f", R0eff);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom strip — counts
        nvgFontSize(vg, 7.5f);
        nvgFillColor(vg, nvgRGB(90, 165, 230));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "S:%d", module->countS);
        nvgText(vg, 4, box.size.y - 2, buf, nullptr);

        nvgFillColor(vg, nvgRGB(245, 90, 90));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "I:%d", module->countI);
        nvgText(vg, box.size.x / 2, box.size.y - 2, buf, nullptr);

        nvgFillColor(vg, nvgRGB(120, 200, 140));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "R:%d", module->countR);
        nvgText(vg, box.size.x - 4, box.size.y - 2, buf, nullptr);
    }
};

struct OutbreakWidget : ModuleWidget {
    OutbreakWidget(Outbreak* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Outbreak.svg")));
        addChild(new ModuleTitle("OUTBREAK", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP"); labels->k1(1, "β");
        labels->k1(2, "γ");   labels->k1(3, "TYPE");
        labels->k2(1, "K");   labels->k2(2, "β·NET");
        labels->k2(3, "SEED");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "β·CV");
        labels->outSection();
        labels->out(0, "STATE"); labels->out(1, "I");
        labels->out(2, "R");     labels->out(3, "PEAK");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new OutbreakView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  255), module, Outbreak::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 255), module, Outbreak::BETA_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 255), module, Outbreak::GAMMA_PARAM));
        addParam(createParamCentered<CKSSThree>(
            Vec(270, 255), module, Outbreak::TYPE_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(120, 285), module, Outbreak::K_PARAM));
        addParam(createParamCentered<Trimpot>(
            Vec(195, 285), module, Outbreak::BETA_NET_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 285), module, Outbreak::SEED_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 271), module, Outbreak::SEED_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  316), module, Outbreak::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 316), module, Outbreak::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 316), module, Outbreak::BETA_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  346), module, Outbreak::STATES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 346), module, Outbreak::INFECTED_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 346), module, Outbreak::RECOVERED_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 346), module, Outbreak::PEAK_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(243, 346), module, Outbreak::PEAK_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Outbreak",
            {"Network SIR epidemic. Infection spreads along the edges of an",
             "internally generated graph (Watts-Strogatz, Erdős–Rényi, or",
             "Barabási–Albert) at rate β per infected neighbour, with recovery",
             "rate γ. SEED regenerates the graph and reseeds patient zero."},
            "Seed, Tape (record the curve)");
    }
};

Model* modelOutbreak = createModel<Outbreak, OutbreakWidget>("Outbreak");
