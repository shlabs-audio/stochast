#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

// ============================================================================
// Discourse — Deffuant-Weisbuch bounded-confidence convergence model.
//
//   N agents (2..16) hold continuous values v_i ∈ [0, 1].
//   Each tick, RATE random pairs (i, j) are drawn. If |v_i - v_j| < TALK
//   (confidence threshold ε), they pull toward each other by μ = PULL:
//     v_i ← v_i + μ (v_j - v_i)
//     v_j ← v_j + μ (v_i - v_j)
//   Otherwise no interaction — agents too far apart simply don't engage.
//
//   Over time the population fragments into a small number of value clusters
//   whose count is governed by ε:
//     ε → 1   : single consensus
//     ε ≈ 0.3 : 2–3 clusters
//     ε → 0   : every agent its own cluster (fragmentation)
// ============================================================================

struct Discourse : Module {
    enum ParamId {
        POPULATION_PARAM,
        TALK_PARAM,        // ε  confidence threshold
        PULL_PARAM,        // μ  convergence rate
        RATE_PARAM,        // pairs per tick
        SHUFFLE_PARAM,
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        TALK_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        VALUES_OUTPUT,     // polyphonic per-agent value
        MEAN_OUTPUT,
        VAR_OUTPUT,
        CLUSTERS_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { SHUFFLE_LIGHT, NUM_LIGHTS };

    static constexpr int kMaxN = 16;
    static constexpr int kHistLen = 256;
    static constexpr float kInternalClockHz = 30.f;

    int N = 16;
    std::array<float, kMaxN> values{};

    // History buffer for the trajectory view (ring of N-vectors)
    std::array<std::array<float, kMaxN>, kHistLen> history{};
    int histWriteIdx = 0;
    int histFrameCounter = 0;

    float internalClockPhase = 0.f;

    dsp::SchmittTrigger clockTrig;
    dsp::SchmittTrigger resetTrig;
    dsp::SchmittTrigger shuffleBtn;

    std::mt19937 rng{0xD15C0u};

    Discourse() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(POPULATION_PARAM, 2.f, (float)kMaxN, 16.f, "Population");
        paramQuantities[POPULATION_PARAM]->snapEnabled = true;
        configParam(TALK_PARAM, 0.01f, 0.50f, 0.22f, "Confidence threshold ε (interact-if-closer-than)");
        configParam(PULL_PARAM, 0.f,   0.50f, 0.30f, "Convergence rate μ (pull strength)");
        configParam(RATE_PARAM, 1.f,   32.f,  6.f,   "Interactions per tick");
        paramQuantities[RATE_PARAM]->snapEnabled = true;
        configButton(SHUFFLE_PARAM, "Re-randomize values");
        configInput(CLOCK_INPUT, "Clock (free-runs at 30 Hz if unpatched)");
        configInput(RESET_INPUT, "Reset");
        configInput(TALK_CV_INPUT, "Confidence CV (±5 V adds to knob)");
        configOutput(VALUES_OUTPUT, "Values (polyphonic, 0..10 V)");
        configOutput(MEAN_OUTPUT, "Mean value (0..10 V)");
        configOutput(VAR_OUTPUT, "Variance (0..10 V, scaled)");
        configOutput(CLUSTERS_OUTPUT, "Cluster count (0..10 V)");
        randomizeValues();
    }

    void randomizeValues() {
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        for (int i = 0; i < kMaxN; ++i) values[i] = ud(rng);
        for (int h = 0; h < kHistLen; ++h) history[h] = values;
        histWriteIdx = 0;
    }

    void onReset() override { randomizeValues(); }

    float currentTalk() {
        float eps = clamp(params[TALK_PARAM].getValue(), 0.01f, 0.5f);
        if (inputs[TALK_CV_INPUT].isConnected()) {
            eps += clamp(inputs[TALK_CV_INPUT].getVoltage() / 10.f, -0.5f, 0.5f);
        }
        return clamp(eps, 0.001f, 1.f);
    }

    void tick() {
        int pairs = clamp((int)std::round(params[RATE_PARAM].getValue()), 1, 32);
        float eps = currentTalk();
        float mu  = clamp(params[PULL_PARAM].getValue(), 0.f, 0.5f);
        std::uniform_int_distribution<int> uid(0, N - 1);
        for (int p = 0; p < pairs; ++p) {
            int i = uid(rng);
            int j = uid(rng);
            if (i == j) continue;
            float d = values[j] - values[i];
            if (std::abs(d) < eps) {
                values[i] += mu * d;
                values[j] -= mu * d;
            }
        }
    }

    float meanValue() const {
        float m = 0.f;
        for (int i = 0; i < N; ++i) m += values[i];
        return m / std::max(1, N);
    }

    float variance() const {
        float m = meanValue();
        float v = 0.f;
        for (int i = 0; i < N; ++i) {
            float d = values[i] - m;
            v += d * d;
        }
        return v / std::max(1, N);
    }

    // Count contiguous clusters: sort values and split on gaps > ε.
    int countClusters(float eps) const {
        if (N <= 0) return 0;
        std::array<float, kMaxN> s;
        for (int i = 0; i < N; ++i) s[i] = values[i];
        std::sort(s.begin(), s.begin() + N);
        int clusters = 1;
        for (int i = 1; i < N; ++i) {
            if (s[i] - s[i - 1] > eps) ++clusters;
        }
        return clusters;
    }

    void process(const ProcessArgs& args) override {
        int newN = clamp((int)std::round(params[POPULATION_PARAM].getValue()), 2, kMaxN);
        if (newN != N) N = newN;

        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            randomizeValues();
        }
        if (shuffleBtn.process(params[SHUFFLE_PARAM].getValue())) {
            randomizeValues();
        }
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

        // Sample history at ~60 Hz for the trajectory view
        int stride = std::max(1, (int)(args.sampleRate / 60.f));
        if (++histFrameCounter >= stride) {
            histFrameCounter = 0;
            history[histWriteIdx] = values;
            histWriteIdx = (histWriteIdx + 1) % kHistLen;
        }

        // Outputs
        outputs[VALUES_OUTPUT].setChannels(N);
        for (int i = 0; i < N; ++i) {
            outputs[VALUES_OUTPUT].setVoltage(clamp(values[i], 0.f, 1.f) * 10.f, i);
        }
        outputs[MEAN_OUTPUT].setVoltage(clamp(meanValue(), 0.f, 1.f) * 10.f);
        // variance maxes around 0.083 (uniform on [0,1]); scale to ≈10 V at full spread
        outputs[VAR_OUTPUT].setVoltage(clamp(variance() * 120.f, 0.f, 10.f));
        float clusterFrac = (float)countClusters(currentTalk()) / std::max(1, N);
        outputs[CLUSTERS_OUTPUT].setVoltage(clusterFrac * 10.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_t* arr = json_array();
        for (int i = 0; i < kMaxN; ++i)
            json_array_append_new(arr, json_real(values[i]));
        json_object_set_new(root, "values", arr);
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (auto* arr = json_object_get(root, "values")) {
            if (json_is_array(arr)) {
                size_t n = std::min((size_t)kMaxN, json_array_size(arr));
                for (size_t i = 0; i < n; ++i) {
                    json_t* v = json_array_get(arr, i);
                    if (json_is_number(v)) values[i] = (float)json_number_value(v);
                }
            }
        }
    }
};

// ============================================================================
// Visualization — trajectory plot: time on X, value on Y, one colored line
// per agent. As clusters form, lines bundle. A dot at the right edge shows
// each agent's current value.
// ============================================================================

static NVGcolor agentHue(int i, int N, float sat = 0.65f, float light = 0.55f, int alpha = 255) {
    // HSL → RGB; hue evenly spaced around the wheel by agent index
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

struct DiscourseView : LightWidget {
    Discourse* module = nullptr;

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
                    "VALUES  ·  TIME", nullptr);
            return;
        }

        const int N = module->N;
        const int H = Discourse::kHistLen;
        const int wIdx = module->histWriteIdx;

        float pad = 6.f;
        float W = box.size.x - 2 * pad - 14.f; // reserve right strip for current dots
        float Hh = box.size.y - 2 * pad;
        float x0 = pad, y0 = pad;
        float dotX = x0 + W + 4.f;

        // Horizontal reference lines at value = 0, 0.5, 1
        nvgStrokeWidth(vg, 0.5f);
        for (float v : {0.f, 0.5f, 1.f}) {
            nvgBeginPath(vg);
            float y = y0 + Hh * (1.f - v);
            nvgMoveTo(vg, x0, y);
            nvgLineTo(vg, x0 + W, y);
            nvgStrokeColor(vg, nvgRGBA(50, 56, 78, v == 0.5f ? 90 : 60));
            nvgStroke(vg);
        }

        // Trajectory lines, one per agent
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        for (int agent = 0; agent < N; ++agent) {
            NVGcolor c = agentHue(agent, N, 0.7f, 0.58f, 200);
            nvgBeginPath(vg);
            for (int t = 0; t < H; ++t) {
                int idx = (wIdx + t) % H;
                float v = clamp(module->history[idx][agent], 0.f, 1.f);
                float x = x0 + (float)t / (H - 1) * W;
                float y = y0 + Hh * (1.f - v);
                if (t == 0) nvgMoveTo(vg, x, y);
                else nvgLineTo(vg, x, y);
            }
            nvgStrokeColor(vg, c);
            nvgStrokeWidth(vg, 0.9f);
            nvgStroke(vg);
        }

        // Current value dots in the right strip
        for (int agent = 0; agent < N; ++agent) {
            float v = clamp(module->values[agent], 0.f, 1.f);
            float y = y0 + Hh * (1.f - v);
            nvgBeginPath(vg);
            nvgCircle(vg, dotX, y, 2.4f);
            nvgFillColor(vg, agentHue(agent, N, 0.85f, 0.62f, 255));
            nvgFill(vg);
        }

        // Frame + header label
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);

        nvgFontSize(vg, 7.5f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 180));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, 4, 3, "VALUES  ·  TIME →", nullptr);

        // Cluster readout
        char buf[32];
        int k = module->countClusters(module->currentTalk());
        std::snprintf(buf, sizeof(buf), "k=%d", k);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(vg, box.size.x - 4, 3, buf, nullptr);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct DiscourseWidget : ModuleWidget {
    DiscourseWidget(Discourse* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Discourse.svg")));
        addChild(new ModuleTitle("DISCOURSE", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "POP");  labels->k1(1, "TALK");
        labels->k1(2, "PULL"); labels->k1(3, "RATE");
        labels->k2(3, "SHUFFLE");
        labels->inSection();
        labels->in(0, "CLOCK"); labels->in(1, "RESET");
        labels->in(2, "T·CV");
        labels->outSection();
        labels->out(0, "VAL");  labels->out(1, "MEAN");
        labels->out(2, "VAR");  labels->out(3, "CLUST");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new DiscourseView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Discourse::POPULATION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Discourse::TALK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Discourse::PULL_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Discourse::RATE_PARAM));

        addParam(createParamCentered<VCVButton>(
            Vec(270, 294), module, Discourse::SHUFFLE_PARAM));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            Vec(270, 280), module, Discourse::SHUFFLE_LIGHT));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Discourse::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Discourse::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(195, 327), module, Discourse::TALK_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Discourse::VALUES_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Discourse::MEAN_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Discourse::VAR_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Discourse::CLUSTERS_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Discourse",
            {"Deffuant bounded-confidence opinion dynamics.",
             "N agents on a 1D opinion axis; pairs interact when",
             "within tolerance ε. Watch consensus or polarisation emerge."},
            "Seed (replicate runs), Frame (mean opinion variance)");
    }
};

Model* modelDiscourse = createModel<Discourse, DiscourseWidget>("Discourse");
