#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <random>
#include <vector>

// ============================================================================
// Network — relational structure generator. Builds an N-agent undirected
// simple graph in one of three canonical models:
//
//   RING (Watts-Strogatz)
//     Start with a regular ring lattice where each node connects to its K
//     nearest neighbours on each side. Each edge is then rewired with
//     probability β to a random target. β → 0 ≈ regular lattice; β → 1 ≈
//     random graph; β ≈ 0.05–0.2 ≈ small-world.
//
//   ERDOS (Erdős–Rényi G(n, p))
//     Each of the n(n−1)/2 possible edges is present independently with
//     probability β. K is ignored.
//
//   BARA (Barabási–Albert preferential attachment)
//     Start with K+1 nodes fully connected. Add the remaining nodes one
//     at a time; each new node attaches to K existing nodes chosen with
//     probability proportional to current degree. Produces scale-free
//     degree distributions. β is ignored.
//
// Network is regenerated whenever the parameters (POP, K, β, TYPE) or the
// seed change. SHUFFLE bumps the seed so the same parameter set produces
// a different random realization.
// ============================================================================

struct Network : Module {
    enum NetType { TYPE_RING = 0, TYPE_ERDOS, TYPE_BARA, NUM_TYPES };
    enum Scale   { SCALE_OFF = 0, SCALE_MAJOR, SCALE_MINOR, SCALE_PENT, SCALE_CHROM, NUM_SCALES };

    enum ParamId {
        POPULATION_PARAM,
        K_PARAM,
        BETA_PARAM,
        TYPE_PARAM,
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        BETA_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        GATES_OUTPUT,         // polyphonic walker gate per agent
        MEAN_DEGREE_OUTPUT,
        CLUSTERING_OUTPUT,
        JUMP_OUTPUT,          // jump distance of the most recent step (0..10 V)
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxN     = 16;
    static constexpr int kTrailLen = 8;
    static constexpr float kInternalWalkerHz = 4.f;
    static constexpr float kGatePulseSec     = 0.020f;

    int N = 12;
    std::array<std::array<bool, kMaxN>, kMaxN> adj{};
    std::array<int, kMaxN> degree{};
    float meanDegree = 0.f;
    float clusteringCoeff = 0.f;
    bool isConnected = false;

    uint32_t seed = 0x4ABCDEF1u;

    // Param snapshots for dirty-detection
    int prevPop = -1, prevK = -1, prevType = -1;
    float prevBeta = -1.f;
    uint32_t prevSeed = 0;

    // Walker state
    int currentNode = 0;
    int lastJumpDist = 0;
    int scaleMode = SCALE_OFF;
    std::array<int, kTrailLen> trail{};
    int trailHead = 0;
    int trailFill = 0;
    float gatePulse = 0.f;
    float walkerInternalPhase = 0.f;

    std::mt19937 rng;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;

    Network() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 2.f, (float)kMaxN, 12.f, "Population N");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(K_PARAM, 1.f, 7.f, 3.f,
                    "K — nearest neighbours per side (RING) / seed connections (BARA)");
        paramQuantities[K_PARAM]->snapEnabled = true;
        configParam(BETA_PARAM, 0.f, 1.f, 0.10f,
                    "β — rewire probability (RING) / edge probability (ERDOS)");
        configSwitch(TYPE_PARAM, 0.f, (float)(NUM_TYPES - 1), 0.f, "Network type",
                     {"Ring (Watts-Strogatz)", "Erdős–Rényi", "Barabási–Albert"});
        configButton(SHUFFLE_PARAM, "Re-seed RNG (new random realization)");
        configInput(CLOCK_INPUT, "Clock — drives the random-walker (free-runs at 4 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset seed + send walker back to node 0");
        configInput(BETA_CV_INPUT, "β CV (0..10 V adds to knob)");
        configOutput(GATES_OUTPUT, "Walker gate per agent (polyphonic, fires on visit)");
        configOutput(MEAN_DEGREE_OUTPUT, "Mean degree ⟨k⟩ (raw, V = degree)");
        configOutput(CLUSTERING_OUTPUT, "Global clustering coefficient (0..10 V)");
        configOutput(JUMP_OUTPUT, "Jump distance — circular index-step of last hop, scaled 0..10 V");
        regenerate();
    }

    void resetWalker() {
        currentNode = 0;
        lastJumpDist = 0;
        trailHead = 0;
        trailFill = 0;
        trail.fill(0);
        gatePulse = 0.f;
        walkerInternalPhase = 0.f;
    }

    void stepWalker() {
        if (N <= 0) return;
        // Gather neighbours of the current node
        std::array<int, kMaxN> nb;
        int nbCount = 0;
        for (int j = 0; j < N; ++j) {
            if (adj[currentNode][j]) nb[nbCount++] = j;
        }
        int next;
        if (nbCount == 0) {
            // Isolated node — jump anywhere
            std::uniform_int_distribution<int> uid(0, N - 1);
            next = uid(rng);
        } else {
            std::uniform_int_distribution<int> uid(0, nbCount - 1);
            next = nb[uid(rng)];
        }
        // Circular index distance from current to next, mod N
        int diff = std::abs(next - currentNode);
        if (N > 0 && diff > N / 2) diff = N - diff;
        lastJumpDist = diff;

        // Record current in trail, advance
        trail[trailHead] = currentNode;
        trailHead = (trailHead + 1) % kTrailLen;
        if (trailFill < kTrailLen) ++trailFill;
        currentNode = next;
        if (currentNode >= N) currentNode = 0;
        gatePulse = kGatePulseSec;
    }

    void onReset() override {
        seed = 0x4ABCDEF1u;
        regenerate();
        resetWalker();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "scaleMode", json_integer(scaleMode));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* m = json_object_get(root, "scaleMode")) {
            scaleMode = clamp((int)json_integer_value(m), 0, NUM_SCALES - 1);
        }
    }

    // Scale-quantize the most recent jump distance into a V/Oct voltage,
    // or return raw 0..10 V linear if SCALE_OFF.
    float quantizedJump() {
        if (scaleMode == SCALE_OFF) {
            int maxJ = std::max(1, N / 2);
            return clamp((float)lastJumpDist / maxJ * 10.f, 0.f, 10.f);
        }
        static const int sizes[NUM_SCALES] = {1, 7, 7, 5, 12};
        static const int scales[NUM_SCALES][12] = {
            {0,0,0,0,0,0,0,0,0,0,0,0},                       // OFF (unused)
            {0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0, 0},           // Major
            {0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0},           // Natural minor
            {0, 2, 4, 7, 9,  0, 0, 0, 0, 0, 0, 0},           // Major pentatonic
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},          // Chromatic
        };
        if (lastJumpDist <= 0) return 0.f;
        int size = sizes[scaleMode];
        int d = lastJumpDist - 1;
        int degree = d % size;
        int octave = d / size;
        int semitone = scales[scaleMode][degree] + octave * 12;
        return (float)semitone / 12.f;
    }

    int   currentN()    { return clamp((int)std::round(params[POPULATION_PARAM].getValue()), 2, kMaxN); }
    int   currentK()    { return clamp((int)std::round(params[K_PARAM].getValue()), 1, kMaxN - 1); }
    int   currentType() { return clamp((int)std::round(params[TYPE_PARAM].getValue()), 0, NUM_TYPES - 1); }
    float currentBeta() {
        float b = params[BETA_PARAM].getValue();
        if (inputs[BETA_CV_INPUT].isConnected()) {
            b += inputs[BETA_CV_INPUT].getVoltage() / 10.f;
        }
        return clamp(b, 0.f, 1.f);
    }

    void clearGraph() {
        for (int i = 0; i < kMaxN; ++i) {
            adj[i].fill(false);
        }
    }

    void addEdge(int i, int j) {
        if (i == j) return;
        adj[i][j] = adj[j][i] = true;
    }
    bool hasEdge(int i, int j) const { return adj[i][j]; }

    void removeEdge(int i, int j) { adj[i][j] = adj[j][i] = false; }

    void buildRingLattice(int K) {
        clearGraph();
        if (N < 2) return;
        int kEff = std::min(K, N / 2);
        for (int i = 0; i < N; ++i) {
            for (int k = 1; k <= kEff; ++k) {
                int j = (i + k) % N;
                addEdge(i, j);
            }
        }
    }

    void buildWattsStrogatz(int K, float beta) {
        buildRingLattice(K);
        if (N < 3) return;
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        std::uniform_int_distribution<int> uid(0, N - 1);
        int kEff = std::min(K, N / 2);
        for (int i = 0; i < N; ++i) {
            for (int k = 1; k <= kEff; ++k) {
                if (ud(rng) < beta) {
                    int j = (i + k) % N;
                    removeEdge(i, j);
                    int newJ = uid(rng);
                    int safety = N * 2;
                    while (safety-- > 0 && (newJ == i || hasEdge(i, newJ))) {
                        newJ = uid(rng);
                    }
                    if (newJ != i && !hasEdge(i, newJ)) addEdge(i, newJ);
                    else                                addEdge(i, j); // restore on failure
                }
            }
        }
    }

    void buildErdosRenyi(float p) {
        clearGraph();
        if (N < 2) return;
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int i = 0; i < N; ++i) {
            for (int j = i + 1; j < N; ++j) {
                if (ud(rng) < p) addEdge(i, j);
            }
        }
    }

    void buildBarabasiAlbert(int m) {
        clearGraph();
        if (N < m + 1) {
            // Fallback: complete graph
            for (int i = 0; i < N; ++i)
                for (int j = i + 1; j < N; ++j) addEdge(i, j);
            return;
        }
        // Seed: small fully-connected K+1 graph
        for (int i = 0; i <= m; ++i)
            for (int j = i + 1; j <= m; ++j) addEdge(i, j);

        // Add remaining nodes
        for (int newNode = m + 1; newNode < N; ++newNode) {
            // Compute degrees of currently-existing nodes
            std::array<int, kMaxN> deg{};
            int totalDeg = 0;
            for (int i = 0; i < newNode; ++i) {
                int d = 0;
                for (int j = 0; j < newNode; ++j) if (adj[i][j]) ++d;
                deg[i] = d;
                totalDeg += d;
            }
            std::array<bool, kMaxN> chosen{};
            int needed = m;
            int safety = newNode * 10;
            while (needed > 0 && safety-- > 0 && totalDeg > 0) {
                std::uniform_int_distribution<int> uid(0, totalDeg - 1);
                int r = uid(rng);
                int cum = 0;
                for (int i = 0; i < newNode; ++i) {
                    cum += deg[i];
                    if (r < cum) {
                        if (!chosen[i] && !hasEdge(newNode, i)) {
                            addEdge(newNode, i);
                            chosen[i] = true;
                            totalDeg -= deg[i];
                            deg[i] = 0;
                            --needed;
                        } else {
                            // try another
                        }
                        break;
                    }
                }
            }
            // If we still need connections (edge cases), pick any unchosen
            for (int i = 0; needed > 0 && i < newNode; ++i) {
                if (!chosen[i]) {
                    addEdge(newNode, i);
                    chosen[i] = true;
                    --needed;
                }
            }
        }
    }

    void computeMetrics() {
        for (int i = 0; i < N; ++i) {
            int d = 0;
            for (int j = 0; j < N; ++j) if (adj[i][j]) ++d;
            degree[i] = d;
        }
        for (int i = N; i < kMaxN; ++i) degree[i] = 0;

        // Mean degree
        int sum = 0;
        for (int i = 0; i < N; ++i) sum += degree[i];
        meanDegree = (N > 0) ? (float)sum / N : 0.f;

        // Global clustering coefficient (average local clustering)
        float total = 0.f;
        int valid = 0;
        for (int i = 0; i < N; ++i) {
            if (degree[i] < 2) continue;
            int triangles = 0;
            int possible = degree[i] * (degree[i] - 1) / 2;
            int nb[kMaxN], nbCount = 0;
            for (int j = 0; j < N; ++j) if (adj[i][j]) nb[nbCount++] = j;
            for (int a = 0; a < nbCount; ++a) {
                for (int b = a + 1; b < nbCount; ++b) {
                    if (adj[nb[a]][nb[b]]) ++triangles;
                }
            }
            total += (float)triangles / possible;
            ++valid;
        }
        clusteringCoeff = (valid > 0) ? total / valid : 0.f;

        // Connectivity via BFS from node 0
        if (N <= 0) { isConnected = false; return; }
        std::array<bool, kMaxN> visited{};
        visited[0] = true;
        std::queue<int> q;
        q.push(0);
        int count = 1;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v = 0; v < N; ++v) {
                if (adj[u][v] && !visited[v]) {
                    visited[v] = true;
                    ++count;
                    q.push(v);
                }
            }
        }
        isConnected = (count == N);
    }

    void regenerate() {
        rng.seed(seed);
        int t = currentType();
        if (t == TYPE_RING)       buildWattsStrogatz(currentK(), currentBeta());
        else if (t == TYPE_ERDOS) buildErdosRenyi(currentBeta());
        else                      buildBarabasiAlbert(currentK());
        computeMetrics();
    }

    void process(const ProcessArgs& args) override {
        int n = currentN();
        if (n != N) { N = n; if (currentNode >= N) resetWalker(); }

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            seed = 0x4ABCDEF1u;
            regenerate();
            resetWalker();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            seed = seed * 1664525u + 1013904223u;
            regenerate();
        }
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        // Detect parameter changes and re-build the network
        int k = currentK(), tp = currentType();
        float beta = currentBeta();
        if (N != prevPop || k != prevK || tp != prevType ||
            std::fabs(beta - prevBeta) > 1e-4f || seed != prevSeed) {
            regenerate();
            prevPop = N; prevK = k; prevType = tp; prevBeta = beta; prevSeed = seed;
        }

        // Walker — driven by external CLOCK if patched, otherwise internal
        bool walkTick = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) walkTick = true;
        } else {
            walkerInternalPhase += args.sampleTime * kInternalWalkerHz;
            if (walkerInternalPhase >= 1.f) {
                walkerInternalPhase -= 1.f;
                walkTick = true;
            }
        }
        if (walkTick) stepWalker();
        if (gatePulse > 0.f) gatePulse -= args.sampleTime;

        // Outputs
        outputs[GATES_OUTPUT].setChannels(N);
        bool gateActive = gatePulse > 0.f;
        for (int i = 0; i < N; ++i) {
            outputs[GATES_OUTPUT].setVoltage((i == currentNode && gateActive) ? 10.f : 0.f, i);
        }
        outputs[MEAN_DEGREE_OUTPUT].setVoltage(clamp(meanDegree, 0.f, 12.f));
        outputs[CLUSTERING_OUTPUT].setVoltage(clamp(clusteringCoeff, 0.f, 1.f) * 10.f);
        outputs[JUMP_OUTPUT].setVoltage(quantizedJump());

        // Publish adjacency + walker state into the right neighbour's left
        // expander buffer. The consumer (currently Diffusion) is responsible
        // for allocating its own leftExpander buffers — a push-receive
        // convention used by most VCV Rack expanders. We write the message,
        // ask the engine to flip the consumer's buffers, and the consumer
        // reads from its own leftExpander.consumerMessage next tick.
        if (rightExpander.module &&
            rightExpander.module->leftExpander.producerMessage) {
            auto* msg = static_cast<EmpiriaNetworkMessage*>(
                            rightExpander.module->leftExpander.producerMessage);
            msg->magic = EmpiriaNetworkMessage::kMagic;
            msg->N = N;
            for (int i = 0; i < EmpiriaNetworkMessage::kMaxN; ++i) {
                for (int j = 0; j < EmpiriaNetworkMessage::kMaxN; ++j)
                    msg->adj[i][j] = (i < N && j < N) ? adj[i][j] : false;
                msg->nodeState[i] = (i == currentNode) ? 10.f : 0.f;
            }
            rightExpander.module->leftExpander.messageFlipRequested = true;
        }
    }
};

// ============================================================================
// Visualization — circular layout, nodes sized by degree, edges drawn
// translucent between connected pairs.
// ============================================================================

struct NetworkView : LightWidget {
    Network* module = nullptr;

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
                    "NETWORK  ·  NODES  ·  EDGES", nullptr);
            return;
        }

        const int N = module->N;

        float pad = 6.f;
        float topStripH = 12.f;
        float botStripH = 12.f;
        float chartW = box.size.x - 2 * pad;
        float chartH = box.size.y - 2 * pad - topStripH - botStripH;
        float cx = pad + chartW / 2;
        float cy = pad + topStripH + chartH / 2;
        float radius = std::min(chartW, chartH) * 0.40f;

        // Compute node positions on a circle
        std::array<float, Network::kMaxN> nx, ny;
        for (int i = 0; i < N; ++i) {
            float angle = (float)i / N * 2.f * (float)M_PI - (float)M_PI / 2.f;
            nx[i] = cx + radius * std::cos(angle);
            ny[i] = cy + radius * std::sin(angle);
        }

        // Draw edges (behind nodes)
        nvgLineCap(vg, NVG_ROUND);
        for (int i = 0; i < N; ++i) {
            for (int j = i + 1; j < N; ++j) {
                if (module->adj[i][j]) {
                    nvgBeginPath(vg);
                    nvgMoveTo(vg, nx[i], ny[i]);
                    nvgLineTo(vg, nx[j], ny[j]);
                    nvgStrokeColor(vg, nvgRGBA(80, 165, 220, 130));
                    nvgStrokeWidth(vg, 0.8f);
                    nvgStroke(vg);
                }
            }
        }

        // Draw recent trail with fading alpha (oldest first, dimmest)
        for (int i = module->trailFill - 1; i >= 0; --i) {
            int idx = (module->trailHead - 1 - i + Network::kTrailLen) % Network::kTrailLen;
            int node = module->trail[idx];
            if (node < 0 || node >= N) continue;
            float age = (float)i / std::max(1, Network::kTrailLen);
            int alpha = (int)(150 * (1.f - age));
            nvgBeginPath(vg);
            nvgCircle(vg, nx[node], ny[node], 3.0f);
            nvgFillColor(vg, nvgRGBA(255, 210, 110, alpha));
            nvgFill(vg);
        }

        // Draw nodes, sized by degree
        int maxDeg = 0;
        for (int i = 0; i < N; ++i) if (module->degree[i] > maxDeg) maxDeg = module->degree[i];
        if (maxDeg == 0) maxDeg = 1;
        for (int i = 0; i < N; ++i) {
            float t = (float)module->degree[i] / maxDeg;
            float r = 2.2f + t * 3.2f;
            bool current = (i == module->currentNode);
            nvgBeginPath(vg);
            nvgCircle(vg, nx[i], ny[i], current ? r + 2.0f : r);
            nvgFillColor(vg, current ? nvgRGB(255, 220, 110) : nvgRGB(245, 160, 90));
            nvgFill(vg);
            if (current) {
                // Outline halo for the walker's current position
                nvgBeginPath(vg);
                nvgCircle(vg, nx[i], ny[i], r + 4.0f);
                nvgStrokeColor(vg, nvgRGBA(255, 220, 110, 180));
                nvgStrokeWidth(vg, 1.0f);
                nvgStroke(vg);
            }
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — type + parameters
        const char* typeShort[Network::NUM_TYPES] = {"RING", "ERDOS", "BARA"};
        int typeIdx = module->currentType();
        char buf[64];
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        if (typeIdx == Network::TYPE_RING) {
            std::snprintf(buf, sizeof(buf), "%s  N=%d  K=%d  β=%.2f",
                          typeShort[typeIdx], N, module->currentK(), module->currentBeta());
        } else if (typeIdx == Network::TYPE_ERDOS) {
            std::snprintf(buf, sizeof(buf), "%s  N=%d  p=%.2f",
                          typeShort[typeIdx], N, module->currentBeta());
        } else {
            std::snprintf(buf, sizeof(buf), "%s  N=%d  m=%d",
                          typeShort[typeIdx], N, module->currentK());
        }
        nvgText(vg, 4, 3, buf, nullptr);

        // Top-right: connectivity
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        if (module->isConnected) {
            nvgFillColor(vg, nvgRGB(120, 200, 140));
            nvgText(vg, box.size.x - 4, 3, "connected", nullptr);
        } else {
            nvgFillColor(vg, nvgRGBA(200, 130, 80, 200));
            nvgText(vg, box.size.x - 4, 3, "disconnected", nullptr);
        }

        // Bottom strip — stats
        nvgFontSize(vg, 7.5f);
        nvgFillColor(vg, nvgRGBA(180, 190, 210, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        std::snprintf(buf, sizeof(buf), "<k>=%.2f", module->meanDegree);
        nvgText(vg, 4, box.size.y - 3, buf, nullptr);
        std::snprintf(buf, sizeof(buf), "CC=%.2f", module->clusteringCoeff);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        nvgText(vg, box.size.x - 4, box.size.y - 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct NetworkWidget : ModuleWidget {
    NetworkWidget(Network* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Network.svg")));
        addChild(new ModuleTitle("NETWORK", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP"); labels->k1(1, "K");
        labels->k1(2, "β");   labels->k1(3, "TYPE");
        labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "β·CV");
        labels->outSection();
        labels->out(0, "GATES"); labels->out(1, "⟨k⟩");
        labels->out(2, "CC");    labels->out(3, "JUMP");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new NetworkView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Network::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Network::K_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Network::BETA_PARAM));
        addParam(createParamCentered<CKSSThree>(
            Vec(270, 258), module, Network::TYPE_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Network::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Network::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Network::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Network::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Network::BETA_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Network::GATES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Network::MEAN_DEGREE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Network::CLUSTERING_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Network::JUMP_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Network*>(this->module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem(
            "Quantize JUMP to scale",
            {"Off (linear 0..10 V)",
             "Major (V/Oct)",
             "Natural minor (V/Oct)",
             "Major pentatonic (V/Oct)",
             "Chromatic (V/Oct)"},
            &m->scaleMode));

        appendAboutMenu(menu, "Network",
            {"Generates a random graph (Erdős-Rényi, Watts-Strogatz,",
             "Barabási-Albert, regular lattice) and exposes adjacency on",
             "its RIGHT expander port for Diffusion to read."},
            "Diffusion (the consumer), Seed (reproducible graphs)");
    }
};

Model* modelNetwork = createModel<Network, NetworkWidget>("Network");
