#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

// ============================================================================
// Pareto — stochastic pairwise exchange model (Chakraborti 2002, Boghosian 2014).
//
//   N agents start with equal value v_i = 1.
//   Each tick, RATE random pairs (i, j) exchange. Stake = TRADE · min(v_i, v_j).
//   With probability 0.5 + BIAS·sign(v_i - v_j), the higher-value side wins.
//   With BIAS = 0 (fair coin) value STILL condenses: the smaller stake means
//   the lower side has less to lose in absolute terms, and the random walk
//   carries the loser to zero — a "geometric ratchet" toward a single agent
//   holding all the value (Boghosian's absorbing state).
//
//   POOL implements a flat-rate mean-field redistribution per tick: each
//   agent contributes τ·v_i to a pool that's redistributed equally. Even small
//   τ creates non-trivial steady-state distributions instead of full condensation.
// ============================================================================

struct Pareto : Module {
    enum ParamId {
        POPULATION_PARAM,
        TRADE_PARAM,     // β  fraction of smaller value at stake
        BIAS_PARAM,      // advantage in coin flip
        POOL_PARAM,      // mean-field redistribution per tick (squared scaling)
        RATE_PARAM,      // exchanges per tick
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        POOL_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        VALUE_OUTPUT,           // polyphonic per-agent share
        GINI_OUTPUT,
        TOP_OUTPUT,
        CONCENTRATION_OUTPUT,   // gate when top share > 0.5
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, CONCENTRATION_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxN = 16;
    static constexpr float kInternalClockHz = 30.f;
    static constexpr float kConcThreshold = 0.5f;

    int N = 16;
    std::array<float, kMaxN> value{};
    float internalClockPhase = 0.f;

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger shuffleBtn;

    std::mt19937 rng{0xC0FFEEu};

    Pareto() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 2.f, (float)kMaxN, 16.f, "Population");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(TRADE_PARAM, 0.01f, 0.5f, 0.10f, "Exchange fraction β (stake = β·min(v_i, v_j))");
        configParam(BIAS_PARAM, 0.f, 0.5f, 0.f, "Bias (advantage to the higher side)");
        configParam(POOL_PARAM, 0.f, 1.f, 0.f, "Pool / redistribution τ (squared, 0..2%/tick)");
        configParam(RATE_PARAM, 1.f, 32.f, 6.f, "Exchanges per tick");
        paramQuantities[RATE_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Randomize starting values");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset (equal values)");
        configInput(POOL_CV_INPUT, "Pool CV (0..10 V adds to knob)");
        configOutput(VALUE_OUTPUT, "Value share per agent (polyphonic, 0..10 V)");
        configOutput(GINI_OUTPUT, "Gini coefficient (0..10 V)");
        configOutput(TOP_OUTPUT, "Top share (0..10 V)");
        configOutput(CONCENTRATION_OUTPUT, "Concentration gate (top share > 50%)");
        resetValue();
    }

    void resetValue() {
        for (int i = 0; i < kMaxN; ++i) value[i] = 1.f;
    }

    void randomizeValue() {
        std::uniform_real_distribution<float> ud(0.5f, 1.5f);
        for (int i = 0; i < kMaxN; ++i) value[i] = ud(rng);
    }

    void onReset() override { resetValue(); }

    float currentPool() {
        float t = clamp(params[POOL_PARAM].getValue(), 0.f, 1.f);
        if (inputs[POOL_CV_INPUT].isConnected()) {
            t += clamp(inputs[POOL_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        }
        return clamp(t, 0.f, 1.f);
    }

    void tick() {
        int exchanges = clamp((int)std::round(params[RATE_PARAM].getValue()), 1, 32);
        float beta  = clamp(params[TRADE_PARAM].getValue(), 0.001f, 1.f);
        float bias  = clamp(params[BIAS_PARAM].getValue(), 0.f, 0.5f);
        float poolK = currentPool();
        float poolR = poolK * poolK * 0.02f; // 0..2% per tick, quadratic for fine control near 0

        std::uniform_int_distribution<int>  uid(0, N - 1);
        std::uniform_real_distribution<float> ud(0.f, 1.f);

        for (int t = 0; t < exchanges; ++t) {
            int i = uid(rng);
            int j = uid(rng);
            if (i == j) continue;
            float stake = beta * std::min(value[i], value[j]);
            if (stake <= 0.f) continue;

            float pIwins = 0.5f;
            if (value[i] > value[j]) pIwins += bias;
            else if (value[i] < value[j]) pIwins -= bias;

            if (ud(rng) < pIwins) { value[i] += stake; value[j] -= stake; }
            else                  { value[i] -= stake; value[j] += stake; }
            // Float accumulation can creep slightly below zero under bias;
            // clamp so divisors (topShare, gini) stay well-behaved.
            if (value[i] < 0.f) value[i] = 0.f;
            if (value[j] < 0.f) value[j] = 0.f;
        }

        if (poolR > 0.f) {
            float pool = 0.f;
            for (int i = 0; i < N; ++i) {
                float tk = value[i] * poolR;
                pool += tk;
                value[i] -= tk;
            }
            float perAgent = pool / std::max(1, N);
            for (int i = 0; i < N; ++i) {
                value[i] += perAgent;
                if (value[i] < 0.f) value[i] = 0.f;
            }
        }
    }

    float totalValue() const {
        float s = 0.f;
        for (int i = 0; i < N; ++i) s += value[i];
        return s;
    }

    float topShare() const {
        float total = totalValue(), mx = 0.f;
        for (int i = 0; i < N; ++i) if (value[i] > mx) mx = value[i];
        return total < 1e-9f ? 0.f : mx / total;
    }

    // Gini coefficient via the standard sorted-rank formula
    float gini() const {
        if (N <= 1) return 0.f;
        std::array<float, kMaxN> s;
        for (int i = 0; i < N; ++i) s[i] = std::max(0.f, value[i]);
        std::sort(s.begin(), s.begin() + N);
        float sum = 0.f, weighted = 0.f;
        for (int i = 0; i < N; ++i) {
            sum += s[i];
            weighted += s[i] * (i + 1);
        }
        if (sum < 1e-9f) return 0.f;
        float g = (2.f * weighted) / (N * sum) - (float)(N + 1) / (float)N;
        return clamp(g, 0.f, 1.f);
    }

    void process(const ProcessArgs& args) override {
        int newN = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 2, kMaxN);
        if (newN != N) N = newN;

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) resetValue();
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) randomizeValue();
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
        float total = totalValue();
        if (total < 1e-9f) total = 1.f;
        outputs[VALUE_OUTPUT].setChannels(N);
        for (int i = 0; i < N; ++i) {
            outputs[VALUE_OUTPUT].setVoltage(clamp(value[i] / total, 0.f, 1.f) * 10.f, i);
        }

        float g = gini();
        outputs[GINI_OUTPUT].setVoltage(g * 10.f);

        float top = topShare();
        outputs[TOP_OUTPUT].setVoltage(top * 10.f);

        bool concentrated = top >= kConcThreshold;
        outputs[CONCENTRATION_OUTPUT].setVoltage(concentrated ? 10.f : 0.f);
        lights[CONCENTRATION_LIGHT].setBrightness(concentrated ? 1.f : 0.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_t* arr = json_array();
        for (int i = 0; i < kMaxN; ++i)
            json_array_append_new(arr, json_real(value[i]));
        json_object_set_new(root, "value", arr);
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* arr = json_object_get(root, "value")) {
            if (json_is_array(arr)) {
                size_t n = std::min((size_t)kMaxN, json_array_size(arr));
                for (size_t i = 0; i < n; ++i) {
                    json_t* v = json_array_get(arr, i);
                    if (json_is_number(v)) value[i] = (float)json_number_value(v);
                }
            }
        }
    }
};

// ============================================================================
// Visualization — bars sorted ascending by share; color encodes original agent
// identity (so you can watch a specific agent rise or fall through the ranks
// over time). Reference lines at the equality share (1/N) and the
// concentration threshold (0.5).
// ============================================================================

static NVGcolor agentHueP(int i, int N, float sat = 0.65f, float light = 0.58f, int alpha = 255) {
    float h = (N > 0) ? ((float)i / N) : 0.f;
    auto hue2rgb = [](float p, float q, float t) {
        if (t < 0.f) t += 1.f;
        if (t > 1.f) t -= 1.f;
        if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
        if (t < 1.f / 2.f) return q;
        if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
        return p;
    };
    float r, g, b;
    if (sat == 0.f) { r = g = b = light; }
    else {
        float q = light < 0.5f ? light * (1.f + sat) : light + sat - light * sat;
        float p = 2.f * light - q;
        r = hue2rgb(p, q, h + 1.f / 3.f);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.f / 3.f);
    }
    return nvgRGBA(
        (unsigned char)(clamp(r, 0.f, 1.f) * 255),
        (unsigned char)(clamp(g, 0.f, 1.f) * 255),
        (unsigned char)(clamp(b, 0.f, 1.f) * 255),
        (unsigned char)alpha);
}

struct ParetoView : LightWidget {
    Pareto* module = nullptr;

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
                    "VALUE  ·  SORTED", nullptr);
            return;
        }

        const int N = module->N;
        float total = module->totalValue();
        if (total < 1e-9f) total = 1.f;

        // Build (share, originalIndex) pairs, sort ascending by share
        struct Entry { float share; int idx; };
        std::array<Entry, Pareto::kMaxN> es;
        for (int i = 0; i < N; ++i) es[i] = {module->value[i] / total, i};
        std::sort(es.begin(), es.begin() + N,
                  [](const Entry& a, const Entry& b) { return a.share < b.share; });

        float pad = 6.f;
        float W = box.size.x - 2 * pad;
        float Hh = box.size.y - 2 * pad - 4.f;
        float x0 = pad, y0 = pad;

        // Reference lines: equality (1/N) and concentration threshold (0.5)
        float eqY = y0 + Hh * (1.f - 1.f / std::max(1, N));
        nvgBeginPath(vg);
        float dash = 4.f;
        for (float xx = x0; xx < x0 + W; xx += dash * 2.f) {
            nvgMoveTo(vg, xx, eqY);
            nvgLineTo(vg, std::min(xx + dash, x0 + W), eqY);
        }
        nvgStrokeColor(vg, nvgRGBA(100, 130, 170, 130));
        nvgStrokeWidth(vg, 0.7f);
        nvgStroke(vg);

        float concY = y0 + Hh * 0.5f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, concY);
        nvgLineTo(vg, x0 + W, concY);
        nvgStrokeColor(vg, nvgRGBA(180, 80, 80, 100));
        nvgStrokeWidth(vg, 0.6f);
        nvgStroke(vg);

        // Bars
        if (N > 0) {
            float slot = W / N;
            float barW = std::max(2.f, slot * 0.7f);
            for (int i = 0; i < N; ++i) {
                float cx = x0 + slot * (i + 0.5f);
                float share = clamp(es[i].share, 0.f, 1.f);
                float barH = Hh * share;
                float by = y0 + Hh - barH;

                NVGcolor c = agentHueP(es[i].idx, Pareto::kMaxN, 0.65f, 0.55f, 230);
                nvgBeginPath(vg);
                nvgRect(vg, cx - barW * 0.5f, by, barW, barH);
                nvgFillColor(vg, c);
                nvgFill(vg);

                // Cap line on top
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - barW * 0.5f, by);
                nvgLineTo(vg, cx + barW * 0.5f, by);
                nvgStrokeColor(vg, agentHueP(es[i].idx, Pareto::kMaxN, 0.85f, 0.7f, 255));
                nvgStrokeWidth(vg, 1.1f);
                nvgStroke(vg);
            }
        }

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        // Header + Gini readout
        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, "VALUE  ·  LOW → HIGH", nullptr);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "G=%.2f  top=%.0f%%",
                      module->gini(), module->topShare() * 100.f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct ParetoWidget : ModuleWidget {
    ParetoWidget(Pareto* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Pareto.svg")));
        addChild(new ModuleTitle("PARETO", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP");   labels->k1(1, "TRADE");
        labels->k1(2, "BIAS");  labels->k1(3, "POOL");
        labels->k2(1, "RATE");  labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "P·CV");
        labels->outSection();
        labels->out(0, "VAL"); labels->out(1, "GINI");
        labels->out(2, "TOP"); labels->out(3, "CONC");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new ParetoView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Pareto::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Pareto::TRADE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Pareto::BIAS_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Pareto::POOL_PARAM));

        addParam(createParamCentered<Trimpot>(
            Vec(120, 294), module, Pareto::RATE_PARAM));
        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Pareto::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Pareto::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Pareto::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Pareto::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Pareto::POOL_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Pareto::VALUE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Pareto::GINI_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Pareto::TOP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Pareto::CONCENTRATION_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            Vec(243, 358), module, Pareto::CONCENTRATION_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Pareto",
            {"Boghosian-style affine wealth model. Simulates trading",
             "between agents under a small bias and shows convergence",
             "to a Pareto-tailed wealth distribution."},
            "Frame (track inequality), Tape (record CDF)");
    }
};

Model* modelPareto = createModel<Pareto, ParetoWidget>("Pareto");
