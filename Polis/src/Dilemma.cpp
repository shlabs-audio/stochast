#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

// ============================================================================
// Dilemma — Iterated Prisoner's Dilemma round-robin tournament.
//
//   N agents (2..16) are each assigned to one of four strategies, drawn at
//   SHUFFLE time with weights biased by MIX:
//       ALL_C  — always cooperate
//       ALL_D  — always defect
//       TFT    — tit-for-tat: cooperate first, then copy opponent's last move
//       GRIM   — grim trigger: cooperate until opponent defects once, then defect forever
//
//   Each tick is one round: every pair (i, j) plays one PD with the payoff
//   matrix below. The pairing's history is remembered separately so each
//   strategy "knows" what each opponent did last (independent of what other
//   opponents did).
//
//   Payoff matrix (classic):
//       both C → R = 3        (Reward for mutual cooperation)
//       both D → P = 1        (Punishment for mutual defection)
//       C vs D → S = 0 / T    (Sucker / Temptation)
//       T = 3 + 2·PAYOFF      → defection gets more tempting as PAYOFF rises
//
//   NOISE flips each agent's chosen action with probability ε before play —
//   this is what kills "nice" cooperation between TFT pairs (one accidental
//   defection cascades).
// ============================================================================

struct Dilemma : Module {
    enum Strategy { STRAT_ALL_C = 0, STRAT_ALL_D, STRAT_TFT, STRAT_GRIM, NUM_STRATS };

    enum ParamId {
        POPULATION_PARAM,
        MIX_PARAM,        // cooperative-bias of strategy distribution
        PAYOFF_PARAM,     // 0..1 → T = 3..5 (R held at 3)
        NOISE_PARAM,      // 0..1 → action error rate 0..0.20
        RATE_PARAM,       // rounds per tick (trimpot)
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        NOISE_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        COOP_GATES_OUTPUT,   // poly: gate per agent if cooperated in majority of pairings this round
        MEAN_COOP_OUTPUT,    // 0..10 V fraction of cooperative actions overall this round
        SCORE_OUTPUT,        // 0..10 V mean score per agent per round (scaled by 5)
        SUCKER_OUTPUT,       // gate pulse whenever a (C,D) sucker outcome occurs
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, SUCKER_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxN     = 16;
    static constexpr int kHistLen  = 256;
    static constexpr float kInternalClockHz = 15.f;
    static constexpr float kSuckerPulseSec = 0.025f;

    int N = 12;
    std::array<int, kMaxN> strategies{};

    // Per-pair history. Symmetric tables: myLast[i][j] = i's last action vs j.
    std::array<std::array<bool, kMaxN>, kMaxN> myLast{};
    std::array<std::array<bool, kMaxN>, kMaxN> oppEverDefected{};
    std::array<std::array<bool, kMaxN>, kMaxN> hasPlayed{};

    std::array<float, kMaxN> totalScore{};
    int roundsPlayed = 0;

    // Per-agent cooperation count this round (denom = N - 1)
    std::array<int, kMaxN> coopCountThisRound{};
    int totalActionsThisRound = 0;
    int totalCoopActionsThisRound = 0;

    // Per-strategy history of average score per agent per round (for viz)
    std::array<std::array<float, kHistLen>, NUM_STRATS> stratHist{};
    int histIdx = 0;

    float suckerPulse = 0.f;
    float internalClockPhase = 0.f;

    std::mt19937 rng{0xFADED01u};

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger shuffleBtn;

    Dilemma() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 2.f, (float)kMaxN, 12.f, "Population");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(MIX_PARAM,    0.f, 1.f, 0.55f, "Cooperative mix (0=all defectors, 1=all cooperators)");
        configParam(PAYOFF_PARAM, 0.f, 1.f, 1.00f, "Temptation T (3..5; default = classic T=5)");
        configParam(NOISE_PARAM,  0.f, 1.f, 0.00f, "Action error rate (0..20%)");
        configParam(RATE_PARAM,   1.f, 32.f, 1.f,  "Rounds per tick");
        paramQuantities[RATE_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Re-draw strategies & reset scores");
        configInput(CLOCK_INPUT, "Clock (free-runs at 15 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (clear scores + history, keep strategies)");
        configInput(NOISE_CV_INPUT, "Noise CV (0..10 V adds to knob)");
        configOutput(COOP_GATES_OUTPUT, "Cooperate gate per agent (polyphonic)");
        configOutput(MEAN_COOP_OUTPUT, "Mean cooperation rate this round (0..10 V)");
        configOutput(SCORE_OUTPUT, "Mean score per agent per round (0..10 V)");
        configOutput(SUCKER_OUTPUT, "Sucker outcome gate (one cooperates, one defects)");
        assignStrategies();
    }

    void assignStrategies() {
        float mix = clamp(params[MIX_PARAM].getValue(), 0.f, 1.f);
        // Weights: ALL_D scales with (1-mix)^2 (so defectors dominate at low mix);
        // TFT scales with mix; ALL_C with mix^2; GRIM with mix*(1-mix) (mid-range).
        float wD    = (1.f - mix) * (1.f - mix);
        float wC    = mix * mix;
        float wTFT  = mix;
        float wGRIM = mix * (1.f - mix) * 2.f;
        float weights[NUM_STRATS] = { wC, wD, wTFT, wGRIM };

        // Avoid degenerate all-zero
        float wsum = weights[0] + weights[1] + weights[2] + weights[3];
        if (wsum < 1e-6f) { weights[STRAT_TFT] = 1.f; wsum = 1.f; }

        // Cumulative
        float cum[NUM_STRATS];
        float c = 0.f;
        for (int s = 0; s < NUM_STRATS; ++s) { c += weights[s] / wsum; cum[s] = c; }

        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int i = 0; i < kMaxN; ++i) {
            float r = ud(rng);
            int s = 0;
            while (s < NUM_STRATS - 1 && r > cum[s]) ++s;
            strategies[i] = s;
        }
        resetGame();
    }

    void resetGame() {
        for (int i = 0; i < kMaxN; ++i) {
            totalScore[i] = 0.f;
            coopCountThisRound[i] = 0;
            for (int j = 0; j < kMaxN; ++j) {
                myLast[i][j] = true;
                oppEverDefected[i][j] = false;
                hasPlayed[i][j] = false;
            }
        }
        roundsPlayed = 0;
        totalActionsThisRound = 0;
        totalCoopActionsThisRound = 0;
        for (int s = 0; s < NUM_STRATS; ++s) stratHist[s].fill(0.f);
        histIdx = 0;
    }

    void onReset() override { resetGame(); }

    bool decide(int agent, int opponent, std::uniform_real_distribution<float>& ud) {
        const int s = strategies[agent];
        const bool first = !hasPlayed[agent][opponent];
        switch (s) {
            case STRAT_ALL_C: return true;
            case STRAT_ALL_D: return false;
            case STRAT_TFT:   return first ? true : myLast[opponent][agent]; // opponent's last action vs me
            case STRAT_GRIM:  return !oppEverDefected[agent][opponent];
        }
        return true;
    }

    float currentNoise() {
        float n = clamp(params[NOISE_PARAM].getValue(), 0.f, 1.f);
        if (inputs[NOISE_CV_INPUT].isConnected()) {
            n += clamp(inputs[NOISE_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        }
        return clamp(n, 0.f, 1.f) * 0.20f;  // max 20% error
    }

    void runOneRound() {
        float payoffKnob = clamp(params[PAYOFF_PARAM].getValue(), 0.f, 1.f);
        float T = 3.f + 2.f * payoffKnob; // 3..5
        const float R = 3.f, P = 1.f, S = 0.f;

        float noise = currentNoise();
        std::uniform_real_distribution<float> ud(0.f, 1.f);

        for (int i = 0; i < N; ++i) coopCountThisRound[i] = 0;
        totalActionsThisRound = 0;
        totalCoopActionsThisRound = 0;

        for (int i = 0; i < N; ++i) {
            for (int j = i + 1; j < N; ++j) {
                bool actI = decide(i, j, ud);
                bool actJ = decide(j, i, ud);
                if (ud(rng) < noise) actI = !actI;
                if (ud(rng) < noise) actJ = !actJ;

                float scoreI, scoreJ;
                if      ( actI &&  actJ) { scoreI = R; scoreJ = R; }
                else if ( actI && !actJ) { scoreI = S; scoreJ = T; suckerPulse = kSuckerPulseSec; }
                else if (!actI &&  actJ) { scoreI = T; scoreJ = S; suckerPulse = kSuckerPulseSec; }
                else                     { scoreI = P; scoreJ = P; }

                totalScore[i] += scoreI;
                totalScore[j] += scoreJ;

                myLast[i][j] = actI;
                myLast[j][i] = actJ;
                if (!actI) oppEverDefected[j][i] = true;
                if (!actJ) oppEverDefected[i][j] = true;
                hasPlayed[i][j] = true;
                hasPlayed[j][i] = true;

                if (actI) ++coopCountThisRound[i];
                if (actJ) ++coopCountThisRound[j];
                totalActionsThisRound += 2;
                totalCoopActionsThisRound += (actI ? 1 : 0) + (actJ ? 1 : 0);
            }
        }
        ++roundsPlayed;
    }

    void tick() {
        int rounds = clamp((int)std::round(params[RATE_PARAM].getValue()), 1, 32);
        for (int r = 0; r < rounds; ++r) runOneRound();

        // Snapshot per-strategy averages for viz
        float perStratScore[NUM_STRATS] = {};
        int   perStratCount[NUM_STRATS] = {};
        for (int i = 0; i < N; ++i) {
            perStratScore[strategies[i]] += totalScore[i];
            ++perStratCount[strategies[i]];
        }
        for (int s = 0; s < NUM_STRATS; ++s) {
            float avg = (perStratCount[s] > 0 && roundsPlayed > 0)
                        ? perStratScore[s] / perStratCount[s] / roundsPlayed
                        : 0.f;
            stratHist[s][histIdx] = clamp(avg, 0.f, 5.f);
        }
        histIdx = (histIdx + 1) % kHistLen;
    }

    void process(const ProcessArgs& args) override {
        int newN = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 2, kMaxN);
        if (newN != N) {
            N = newN;
            assignStrategies();
        }

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) resetGame();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) assignStrategies();
        lights[SHUFFLE_LIGHT].setBrightness(params[SHUFFLE_PARAM].getValue() > 0.5f ? 1.f : 0.f);

        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) tick();
        } else {
            internalClockPhase += args.sampleTime * kInternalClockHz;
            if (internalClockPhase >= 1.f) {
                internalClockPhase -= 1.f;
                tick();
            }
        }

        // Outputs
        outputs[COOP_GATES_OUTPUT].setChannels(N);
        int denom = std::max(1, N - 1);
        for (int i = 0; i < N; ++i) {
            bool gate = (coopCountThisRound[i] * 2 >= denom); // majority
            outputs[COOP_GATES_OUTPUT].setVoltage(gate ? 10.f : 0.f, i);
        }

        float meanCoop = totalActionsThisRound > 0
                         ? (float)totalCoopActionsThisRound / totalActionsThisRound
                         : 0.f;
        outputs[MEAN_COOP_OUTPUT].setVoltage(meanCoop * 10.f);

        float meanScore = 0.f;
        for (int i = 0; i < N; ++i) meanScore += totalScore[i];
        meanScore = (roundsPlayed > 0 && N > 0) ? meanScore / N / roundsPlayed : 0.f;
        // meanScore ∈ [0, 5]; map to 0..10 V
        outputs[SCORE_OUTPUT].setVoltage(clamp(meanScore / 5.f, 0.f, 1.f) * 10.f);

        if (suckerPulse > 0.f) suckerPulse -= args.sampleTime;
        bool s = suckerPulse > 0.f;
        outputs[SUCKER_OUTPUT].setVoltage(s ? 10.f : 0.f);
        lights[SUCKER_LIGHT].setBrightness(s ? 1.f : 0.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "roundsPlayed", json_integer(roundsPlayed));
        json_t* strat = json_array();
        json_t* score = json_array();
        for (int i = 0; i < kMaxN; ++i) {
            json_array_append_new(strat, json_integer(strategies[i]));
            json_array_append_new(score, json_real(totalScore[i]));
        }
        json_object_set_new(root, "strategies", strat);
        json_object_set_new(root, "totalScore", score);
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* j = json_object_get(root, "roundsPlayed"))
            roundsPlayed = (int)json_integer_value(j);
        if (auto* arr = json_object_get(root, "strategies")) {
            if (json_is_array(arr)) {
                size_t n = std::min((size_t)kMaxN, json_array_size(arr));
                for (size_t i = 0; i < n; ++i) {
                    json_t* v = json_array_get(arr, i);
                    if (json_is_integer(v)) strategies[i] = (int)json_integer_value(v);
                }
            }
        }
        if (auto* arr = json_object_get(root, "totalScore")) {
            if (json_is_array(arr)) {
                size_t n = std::min((size_t)kMaxN, json_array_size(arr));
                for (size_t i = 0; i < n; ++i) {
                    json_t* v = json_array_get(arr, i);
                    if (json_is_number(v)) totalScore[i] = (float)json_number_value(v);
                }
            }
        }
    }
};

// ============================================================================
// Visualization — one scrolling time-series line per strategy showing average
// score per agent per round. Strategy legend colors:
//   ALL_C  cyan-green   (cooperative)
//   ALL_D  red          (defector)
//   TFT    amber        (reactive)
//   GRIM   purple       (unforgiving)
// ============================================================================

struct DilemmaView : LightWidget {
    Dilemma* module = nullptr;

    NVGcolor stratColor(int s, int alpha = 230) const {
        switch (s) {
            case Dilemma::STRAT_ALL_C: return nvgRGBA( 80, 200, 140, alpha);
            case Dilemma::STRAT_ALL_D: return nvgRGBA(220,  85,  85, alpha);
            case Dilemma::STRAT_TFT:   return nvgRGBA(230, 175,  60, alpha);
            case Dilemma::STRAT_GRIM:  return nvgRGBA(170, 110, 220, alpha);
        }
        return nvgRGBA(200, 200, 200, alpha);
    }

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
            nvgText(vg, box.size.x / 2, box.size.y / 2, "TOURNAMENT  ·  TIME", nullptr);
            return;
        }

        const int N = module->N;
        const int H = Dilemma::kHistLen;
        const int wIdx = module->histIdx;

        float pad = 6.f;
        float W = box.size.x - 2 * pad - 4.f;
        float Hh = box.size.y - 2 * pad - 14.f; // reserve bottom strip for legend
        float x0 = pad, y0 = pad + 12.f;        // top strip for header

        // Reference lines: at R=3 (mutual coop) and P=1 (mutual defect), scaled by 5
        for (float v : {3.f, 1.f}) {
            nvgBeginPath(vg);
            float y = y0 + Hh * (1.f - v / 5.f);
            nvgMoveTo(vg, x0, y);
            nvgLineTo(vg, x0 + W, y);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, v == 3.f ? 110 : 80));
            nvgStrokeWidth(vg, 0.5f);
            nvgStroke(vg);
        }

        // Count agents per strategy (so we only draw lines for strategies in play)
        int countS[Dilemma::NUM_STRATS] = {};
        for (int i = 0; i < N; ++i) ++countS[module->strategies[i]];

        // One line per strategy with at least one agent
        for (int s = 0; s < Dilemma::NUM_STRATS; ++s) {
            if (countS[s] == 0) continue;
            NVGcolor c = stratColor(s, 220);
            nvgBeginPath(vg);
            for (int t = 0; t < H; ++t) {
                int idx = (wIdx + t) % H;
                float v = module->stratHist[s][idx];
                float x = x0 + (float)t / (H - 1) * W;
                float y = y0 + Hh * (1.f - clamp(v / 5.f, 0.f, 1.f));
                if (t == 0) nvgMoveTo(vg, x, y);
                else nvgLineTo(vg, x, y);
            }
            nvgStrokeColor(vg, c);
            nvgStrokeWidth(vg, 1.2f);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);
            nvgStroke(vg);
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Header label
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, "SCORE / ROUND  ·  TIME →", nullptr);

        // Round counter (top right)
        char buf[24];
        std::snprintf(buf, sizeof(buf), "r=%d", module->roundsPlayed);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);

        // Bottom legend with counts per strategy
        const char* labels[Dilemma::NUM_STRATS] = {"C", "D", "TFT", "GRIM"};
        float legY = box.size.y - 4.f;
        float lx = 4.f;
        nvgFontSize(vg, 7.5f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        for (int s = 0; s < Dilemma::NUM_STRATS; ++s) {
            char tag[24];
            std::snprintf(tag, sizeof(tag), "%s:%d", labels[s], countS[s]);
            // colored swatch
            nvgBeginPath(vg);
            nvgCircle(vg, lx + 3, legY - 3, 2.4f);
            nvgFillColor(vg, stratColor(s, 255));
            nvgFill(vg);
            nvgFillColor(vg, nvgRGBA(160, 170, 190, 200));
            nvgText(vg, lx + 9, legY, tag, nullptr);
            lx += 50.f;
        }
    }
};

// ============================================================================
// Widget
// ============================================================================

struct DilemmaWidget : ModuleWidget {
    DilemmaWidget(Dilemma* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Dilemma.svg")));
        addChild(new ModuleTitle("DILEMMA", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP");    labels->k1(1, "MIX");
        labels->k1(2, "PAYOFF"); labels->k1(3, "NOISE");
        labels->k2(1, "RATE");   labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "N·CV");
        labels->outSection();
        labels->out(0, "COOP");  labels->out(1, "MEAN");
        labels->out(2, "SCORE"); labels->out(3, "SUCKER");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new DilemmaView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Dilemma::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Dilemma::MIX_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Dilemma::PAYOFF_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Dilemma::NOISE_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(120, 294), module, Dilemma::RATE_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Dilemma::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Dilemma::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Dilemma::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Dilemma::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Dilemma::NOISE_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Dilemma::COOP_GATES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Dilemma::MEAN_COOP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Dilemma::SCORE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Dilemma::SUCKER_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(243, 358), module, Dilemma::SUCKER_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Dilemma",
            {"Iterated 2x2 game (Prisoner's Dilemma, Stag Hunt, ...).",
             "Strategies (TFT, ALLD, GRIM, ...) play repeatedly and",
             "the payoff stream becomes a CV."},
            "Tape (record payoff stream), Frame (mean payoff)");
    }
};

Model* modelDilemma = createModel<Dilemma, DilemmaWidget>("Dilemma");
