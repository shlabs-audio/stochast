#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

// ============================================================================
// Bandit — multi-armed bandit with three classical action-selection policies.
//
//   K arms (k = 2..8). Each arm i has a true reward mean μ_i drawn from
//   Normal(0, σ_μ) at reset, plus per-pull noise σ_r. Each tick the agent
//   selects ONE arm according to the active policy, observes a reward sample,
//   updates its running mean estimate, and reports cumulative regret.
//
//   Policies (right-click):
//     - ε-greedy:    with prob 1−ε pull arg-max estimate; with prob ε explore
//                    uniformly. Simplest policy, illustrates the basic
//                    exploration–exploitation tension.
//     - UCB1:        pull arg-max( Q̂_i + c · √(ln t / N_i) ). The bonus term
//                    decays with pulls — over-pulled arms lose their bonus,
//                    under-pulled arms gain one. No randomness.
//     - Thompson:    Bayesian. Treat μ_i ~ Normal(Q̂_i, 1/√N_i); sample one
//                    value from each posterior, pull the arm with highest
//                    sample. Naturally balances exploration via uncertainty.
//
//   Outputs:
//     REWARD      reward of the most recent pull (V)
//     REGRET      cumulative regret so far (V)
//     ARM_GATE    polyphonic gate per arm: high for ~30 ms when that arm was
//                 last pulled (so the gates can sequence a drum machine)
//     BEST        running fraction of pulls of the optimal arm (0..10 V)
// ============================================================================

struct Bandit : Module {
    enum Policy { POLICY_EPS = 0, POLICY_UCB, POLICY_THOMPSON, NUM_POLICIES };

    enum ParamId {
        K_PARAM,
        EPSILON_PARAM,    // ε for ε-greedy / c for UCB; semantically reused
        SIGMA_MU_PARAM,   // SD of true arm means at randomize
        SIGMA_R_PARAM,    // per-pull noise SD
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        REWARD_OUTPUT,
        REGRET_OUTPUT,
        ARM_GATE_OUTPUT,
        BEST_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxK = 8;
    static constexpr float kInternalHz = 4.f;
    static constexpr float kGatePulseSec = 0.030f;

    int K = 4;
    int policy = POLICY_EPS;

    std::array<float, kMaxK> mu{};       // true means
    std::array<double, kMaxK> sumR{};    // sum of observed rewards
    std::array<int, kMaxK> count{};      // pulls per arm
    int totalPulls = 0;
    int bestArm = 0;
    int bestPulls = 0;
    double cumRegret = 0.0;
    int lastArm = -1;
    float lastReward = 0.f;
    float gatePulse = 0.f;

    float internalPhase = 0.f;
    dsp::SchmittTrigger clockTrig, resetTrig, shuffleBtn;
    std::mt19937 rng;
    uint32_t seedVal = 0x4ABCDEF1u;

    Bandit() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(K_PARAM, 2.f, (float)kMaxK, 4.f, "K — number of arms");
        paramQuantities[K_PARAM]->snapEnabled = true;
        configParam(EPSILON_PARAM, 0.f, 1.f, 0.10f,
                    "ε (greedy) / c (UCB) / temperature");
        configParam(SIGMA_MU_PARAM, 0.f, 5.f, 1.5f,
                    "σ_μ — SD of true arm means at randomize");
        configParam(SIGMA_R_PARAM, 0.f, 3.f, 0.8f,
                    "σ_r — per-pull noise SD");
        configButton(SHUFFLE_PARAM, "Re-draw arm means");
        configInput(CLOCK_INPUT, "Clock — pulls one arm per tick (internal 4 Hz)");
        configInput(RESET_INPUT, "Reset learning + re-draw arm means");
        configOutput(REWARD_OUTPUT, "Reward of most recent pull");
        configOutput(REGRET_OUTPUT, "Cumulative regret");
        configOutput(ARM_GATE_OUTPUT, "Per-arm pull gate (polyphonic)");
        configOutput(BEST_OUTPUT, "Fraction of pulls on optimal arm (0..10 V)");
        redrawArms();
    }

    void onReset() override { seedVal = 0x4ABCDEF1u; redrawArms(); }

    int currentK() { return clamp((int)std::round(params[K_PARAM].getValue()), 2, kMaxK); }

    void redrawArms() {
        rng.seed(seedVal);
        K = currentK();
        std::normal_distribution<float> nd(0.f, params[SIGMA_MU_PARAM].getValue());
        for (int i = 0; i < kMaxK; ++i) {
            mu[i] = (i < K) ? nd(rng) : 0.f;
            sumR[i] = 0.0;
            count[i] = 0;
        }
        totalPulls = 0;
        cumRegret = 0.0;
        bestPulls = 0;
        lastArm = -1;
        lastReward = 0.f;
        gatePulse = 0.f;
        // Find best arm
        bestArm = 0;
        for (int i = 1; i < K; ++i) if (mu[i] > mu[bestArm]) bestArm = i;
    }

    int selectEpsGreedy() {
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        float eps = clamp(params[EPSILON_PARAM].getValue(), 0.f, 1.f);
        if (ud(rng) < eps) {
            std::uniform_int_distribution<int> ui(0, K - 1);
            return ui(rng);
        }
        // Exploit: arg-max estimate. Tie-break: random.
        float bestQ = -1e18f;
        int best = 0;
        for (int i = 0; i < K; ++i) {
            float q = (count[i] > 0) ? (float)(sumR[i] / count[i]) : 0.f;
            if (q > bestQ) { bestQ = q; best = i; }
        }
        return best;
    }

    int selectUCB() {
        // Classical UCB1 (Auer, Cesa-Bianchi & Fischer 2002):
        //   pick argmax_i  q_i + c · sqrt( 2 · ln t / n_i )
        // The factor of 2 is part of the published bound. The EPSILON_PARAM
        // knob acts as an additional explore multiplier `c`; default c = 1.f
        // recovers the textbook bonus, c < 1 explores less, c > 1 more.
        for (int i = 0; i < K; ++i) if (count[i] == 0) return i;
        float c = clamp(params[EPSILON_PARAM].getValue(), 0.f, 5.f);
        float lnT = std::log((float)totalPulls + 1.f);
        float bestU = -1e18f;
        int best = 0;
        for (int i = 0; i < K; ++i) {
            float q = (float)(sumR[i] / count[i]);
            float bonus = c * std::sqrt(2.f * lnT / count[i]);
            float u = q + bonus;
            if (u > bestU) { bestU = u; best = i; }
        }
        return best;
    }

    int selectThompson() {
        // Thompson sampling under a Normal–Normal model: each arm's true
        // mean μ_i is drawn from a Normal posterior with mean = sample mean
        // q_i and standard deviation that shrinks as 1/sqrt(n_i). Because the
        // reward signal is real-valued (not Bernoulli), a Beta posterior is
        // not the right conjugate; the Gaussian heuristic implemented here
        // is the pragmatic choice for the Gaussian-reward regime that
        // Empiria uses. Cold-start prior strength is kInitialSD.
        constexpr float kInitialSD = 5.f;
        float bestS = -1e18f;
        int best = 0;
        for (int i = 0; i < K; ++i) {
            float q  = (count[i] > 0) ? (float)(sumR[i] / count[i]) : 0.f;
            float sd = (count[i] > 0) ? 1.f / std::sqrt((float)count[i]) : kInitialSD;
            std::normal_distribution<float> nd(q, sd);
            float s = nd(rng);
            if (s > bestS) { bestS = s; best = i; }
        }
        return best;
    }

    void pullOne() {
        if (K != currentK()) redrawArms();
        int arm = 0;
        switch (policy) {
            case POLICY_UCB:      arm = selectUCB();      break;
            case POLICY_THOMPSON: arm = selectThompson(); break;
            default:              arm = selectEpsGreedy();break;
        }
        std::normal_distribution<float> nd(mu[arm], clamp(params[SIGMA_R_PARAM].getValue(), 0.f, 3.f));
        float r = nd(rng);
        sumR[arm] += r;
        ++count[arm];
        ++totalPulls;
        if (arm == bestArm) ++bestPulls;
        cumRegret += (double)mu[bestArm] - r;
        lastArm = arm;
        lastReward = r;
        gatePulse = kGatePulseSec;
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            seedVal = seedVal * 1664525u + 1013904223u;
            redrawArms();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            seedVal = seedVal * 1664525u + 1013904223u;
            redrawArms();
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
        if (tick) pullOne();

        if (gatePulse > 0.f) gatePulse -= args.sampleTime;
        bool gateActive = gatePulse > 0.f;

        outputs[REWARD_OUTPUT].setVoltage(clamp(lastReward, -12.f, 12.f));
        outputs[REGRET_OUTPUT].setVoltage(clamp((float)(cumRegret * 0.05), -12.f, 12.f));
        outputs[ARM_GATE_OUTPUT].setChannels(K);
        for (int i = 0; i < K; ++i)
            outputs[ARM_GATE_OUTPUT].setVoltage(
                (i == lastArm && gateActive) ? 10.f : 0.f, i);
        float fBest = (totalPulls > 0) ? (float)bestPulls / totalPulls : 0.f;
        outputs[BEST_OUTPUT].setVoltage(clamp(fBest, 0.f, 1.f) * 10.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seedVal", json_integer((json_int_t)seedVal));
        json_object_set_new(root, "policy",  json_integer(policy));
        json_object_set_new(root, "totalPulls", json_integer(totalPulls));
        json_object_set_new(root, "bestArm",    json_integer(bestArm));
        json_object_set_new(root, "bestPulls",  json_integer(bestPulls));
        json_object_set_new(root, "cumRegret",  json_real(cumRegret));
        json_t* muArr  = json_array();
        json_t* sumArr = json_array();
        json_t* cntArr = json_array();
        for (int i = 0; i < kMaxK; ++i) {
            json_array_append_new(muArr,  json_real(mu[i]));
            json_array_append_new(sumArr, json_real(sumR[i]));
            json_array_append_new(cntArr, json_integer(count[i]));
        }
        json_object_set_new(root, "mu",    muArr);
        json_object_set_new(root, "sumR",  sumArr);
        json_object_set_new(root, "count", cntArr);
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "seedVal"))
            seedVal = (uint32_t)json_integer_value(j);
        if (auto* j = json_object_get(root, "policy"))
            policy = clamp((int)json_integer_value(j), 0, NUM_POLICIES - 1);
        if (auto* j = json_object_get(root, "totalPulls")) totalPulls = (int)json_integer_value(j);
        if (auto* j = json_object_get(root, "bestArm"))    bestArm    = (int)json_integer_value(j);
        if (auto* j = json_object_get(root, "bestPulls"))  bestPulls  = (int)json_integer_value(j);
        if (auto* j = json_object_get(root, "cumRegret"))  cumRegret  = json_number_value(j);
        auto loadArr = [](json_t* arr, auto& dst, int maxN) {
            if (!arr || !json_is_array(arr)) return;
            size_t n = std::min((size_t)maxN, json_array_size(arr));
            for (size_t i = 0; i < n; ++i) {
                json_t* v = json_array_get(arr, i);
                if (json_is_number(v)) dst[i] =
                    static_cast<typename std::remove_reference<decltype(dst[i])>::type>(json_number_value(v));
            }
        };
        loadArr(json_object_get(root, "mu"),    mu,    kMaxK);
        loadArr(json_object_get(root, "sumR"),  sumR,  kMaxK);
        loadArr(json_object_get(root, "count"), count, kMaxK);
    }
};

// ============================================================================
// Visualization — per-arm bars (estimate ± confidence) plus regret strip.
// ============================================================================

struct BanditView : LightWidget {
    Bandit* module = nullptr;

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
                    "MULTI-ARMED  BANDIT", nullptr);
            return;
        }

        int K = module->K;
        float pad = 6.f, topStripH = 14.f, botStripH = 16.f;
        float W = box.size.x - 2 * pad;
        float H = box.size.y - 2 * pad - topStripH - botStripH;
        float x0 = pad, y0 = pad + topStripH;

        // Clip everything in the plotting area to (x0, y0, W, H) so nothing
        // (confidence bands, big initial-uncertainty rectangles, etc.) bleeds
        // beyond the visualization box.
        nvgSave(vg);
        nvgScissor(vg, x0, y0, W, H);

        // Map y to ±5 V (or whatever spans the arms). Pad symmetrically so the
        // panel feels stable even when arm means scatter.
        float yMin = -4.f, yMax = 4.f;
        for (int i = 0; i < K; ++i) {
            yMin = std::min(yMin, module->mu[i] - 2.f);
            yMax = std::max(yMax, module->mu[i] + 2.f);
        }
        auto mapY = [&](float v) {
            return y0 + H * (1.f - (v - yMin) / std::max(0.1f, yMax - yMin));
        };
        auto mapYclamped = [&](float v) {
            float y = mapY(v);
            return clamp(y, y0, y0 + H);
        };

        // Zero line
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 130));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, mapY(0.f)); nvgLineTo(vg, x0 + W, mapY(0.f));
        nvgStroke(vg);

        float armW = W / K;
        for (int i = 0; i < K; ++i) {
            float cx = x0 + armW * (i + 0.5f);
            float q  = (module->count[i] > 0) ?
                       (float)(module->sumR[i] / module->count[i]) : 0.f;
            float cnt = (float)module->count[i];
            float sd = (cnt > 0.f) ? 1.f / std::sqrt(cnt) : 4.f;

            // Confidence band ±2σ, clamped so unconverged arms don't fill the
            // entire panel with a giant rectangle.
            float yHi = mapYclamped(q + 2.f * sd);
            float yLo = mapYclamped(q - 2.f * sd);
            nvgBeginPath(vg);
            nvgRect(vg, cx - armW * 0.32f, yHi, armW * 0.64f, std::max(1.f, yLo - yHi));
            nvgFillColor(vg, i == module->bestArm
                              ? nvgRGBA(120, 200, 140, 60)
                              : nvgRGBA(140, 160, 200, 50));
            nvgFill(vg);

            // True mean tick
            nvgStrokeColor(vg, i == module->bestArm
                                ? nvgRGB(120, 220, 140) : nvgRGB(180, 190, 210));
            nvgStrokeWidth(vg, 1.4f);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - armW * 0.40f, mapY(module->mu[i]));
            nvgLineTo(vg, cx + armW * 0.40f, mapY(module->mu[i]));
            nvgStroke(vg);

            // Estimate point
            nvgBeginPath(vg);
            nvgCircle(vg, cx, mapYclamped(q), 2.6f);
            nvgFillColor(vg, i == module->lastArm
                              ? nvgRGB(245, 200, 90)
                              : nvgRGB(110, 200, 220));
            nvgFill(vg);
        }

        // End plotting clip; draw labels OUTSIDE the clip in the bottom strip.
        nvgRestore(vg);

        // Pull counts below the plot
        for (int i = 0; i < K; ++i) {
            float cx = x0 + armW * (i + 0.5f);
            char cbuf[16];
            std::snprintf(cbuf, sizeof(cbuf), "%d", module->count[i]);
            nvgFontSize(vg, 7.5f);
            nvgFontFaceId(vg, APP->window->uiFont->handle);
            nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
            nvgText(vg, cx, box.size.y - 3, cbuf, nullptr);
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Top strip — policy + total pulls
        char buf[80];
        const char* polNames[Bandit::NUM_POLICIES] = {"ε-greedy", "UCB1", "Thompson"};
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "%s  K=%d  t=%d",
                      polNames[module->policy], K, module->totalPulls);
        nvgText(vg, 4, 3, buf, nullptr);

        float fBest = (module->totalPulls > 0)
                      ? (float)module->bestPulls / module->totalPulls : 0.f;
        std::snprintf(buf, sizeof(buf), "regret %.1f  best %.0f%%",
                      module->cumRegret, fBest * 100.f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

struct BanditWidget : ModuleWidget {
    BanditWidget(Bandit* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Bandit.svg")));
        addChild(new ModuleTitle("BANDIT", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "K");  labels->k1(1, "ε/c");
        labels->k1(2, "σμ"); labels->k1(3, "σr");
        labels->k2(0, "SHUF");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->outSection();
        labels->out(0, "R");    labels->out(1, "REGR");
        labels->out(2, "ARMS"); labels->out(3, "BEST");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new BanditView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Bandit::K_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Bandit::EPSILON_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Bandit::SIGMA_MU_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Bandit::SIGMA_R_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(45, 294), module, Bandit::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(45, 280), module, Bandit::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Bandit::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Bandit::RESET_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Bandit::REWARD_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Bandit::REGRET_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Bandit::ARM_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Bandit::BEST_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<Bandit*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Action-selection policy"));

        struct PolItem : MenuItem {
            Bandit* m;
            int p;
            void onAction(const event::Action&) override { m->policy = p; }
        };
        auto add = [&](const char* name, int p) {
            auto* it = new PolItem;
            it->text = name;
            it->m = m;
            it->p = p;
            it->rightText = (m->policy == p) ? "✓" : "";
            menu->addChild(it);
        };
        add("ε-greedy",       Bandit::POLICY_EPS);
        add("UCB1",           Bandit::POLICY_UCB);
        add("Thompson sampling", Bandit::POLICY_THOMPSON);

        appendAboutMenu(menu, "Bandit",
            {"K-armed bandit with UCB1, ε-greedy, and Thompson sampling",
             "policies. Outputs arm pulls, cumulative regret, and the",
             "current action selection."},
            "Tape (record reward trajectory), Frame (mean reward)");
    }
};

Model* modelBandit = createModel<Bandit, BanditWidget>("Bandit");
