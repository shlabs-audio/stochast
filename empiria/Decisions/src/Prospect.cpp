#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

// ============================================================================
// Prospect — prospect-theory transformation (Kahneman & Tversky 1992).
//
//   Takes an objective outcome x (±10 V relative to a reference point of 0 V)
//   and an objective probability p (0..10 V representing 0..1), and outputs
//   the SUBJECTIVE value Û(x) · ŵ(p).
//
//     Value function     Û(x) = { x^α          if x ≥ 0
//                               { −λ (−x)^β    if x < 0
//
//     Weighting function ŵ(p) = p^γ / (p^γ + (1 − p)^γ)^(1/γ)
//                               (Tversky-Kahneman 1992 form)
//
//   Standard estimates: α = β ≈ 0.88 (concave for gains, convex for losses),
//   λ ≈ 2.25 (loss aversion), γ ≈ 0.61 for gains.
//
//   Concretely the module visualizes both Û and ŵ on a single panel with the
//   current (x, p) operating point highlighted, and outputs:
//     U      Û(x) — subjective value alone
//     W      ŵ(p) — subjective decision weight
//     SU     Û(x) · ŵ(p) — subjective expected value
//     EV     x · p — objective expected value (for comparison)
// ============================================================================

struct Prospect : Module {
    enum ParamId {
        ALPHA_PARAM,    // α (gain curvature)
        LAMBDA_PARAM,   // λ (loss aversion)
        GAMMA_PARAM,    // γ (probability weighting)
        BETA_PARAM,     // β (loss curvature — defaults equal to α)
        NUM_PARAMS
    };
    enum InputId {
        X_INPUT,
        P_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        U_OUTPUT,
        W_OUTPUT,
        SU_OUTPUT,
        EV_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId { NUM_LIGHTS };

    Prospect() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ALPHA_PARAM, 0.1f, 1.5f, 0.88f,
                    "α — gain curvature (1 = linear, <1 = concave/diminishing)");
        configParam(LAMBDA_PARAM, 1.0f, 4.0f, 2.25f,
                    "λ — loss aversion (kink at zero)");
        configParam(GAMMA_PARAM, 0.3f, 1.5f, 0.61f,
                    "γ — probability weighting (1 = linear; <1 = overweight rare events)");
        configParam(BETA_PARAM, 0.1f, 1.5f, 0.88f,
                    "β — loss curvature (often set equal to α)");
        configInput(X_INPUT, "Objective outcome x (±10 V = ±10 units)");
        configInput(P_INPUT, "Objective probability p (0..10 V = 0..1)");
        configOutput(U_OUTPUT, "Subjective value Û(x), V");
        configOutput(W_OUTPUT, "Subjective weight ŵ(p), V");
        configOutput(SU_OUTPUT, "Subjective expected value Û(x)·ŵ(p), V");
        configOutput(EV_OUTPUT, "Objective expected value x·p, V (comparison)");
    }

    float utility(float x) {
        float alpha = clamp(params[ALPHA_PARAM].getValue(), 0.05f, 2.f);
        float beta  = clamp(params[BETA_PARAM].getValue(),  0.05f, 2.f);
        float lam   = clamp(params[LAMBDA_PARAM].getValue(), 1.f, 5.f);
        if (x >= 0.f) return std::pow(x, alpha);
        return -lam * std::pow(-x, beta);
    }

    float weight(float p) {
        float gamma = clamp(params[GAMMA_PARAM].getValue(), 0.1f, 2.f);
        p = clamp(p, 0.f, 1.f);
        // Tversky-Kahneman 92 single-parameter weighting
        float pg  = std::pow(p, gamma);
        float qg  = std::pow(1.f - p, gamma);
        float denom = std::pow(pg + qg, 1.f / gamma);
        return (denom > 1e-12f) ? pg / denom : p;
    }

    void process(const ProcessArgs&) override {
        float x = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage() : 0.f;
        float p = inputs[P_INPUT].isConnected() ? inputs[P_INPUT].getVoltage() / 10.f : 0.f;
        p = clamp(p, 0.f, 1.f);

        float u  = utility(x);
        float w  = weight(p);
        float su = u * w;
        float ev = x * p;

        outputs[U_OUTPUT].setVoltage(clamp(u, -12.f, 12.f));
        outputs[W_OUTPUT].setVoltage(clamp(w * 10.f, 0.f, 12.f));
        outputs[SU_OUTPUT].setVoltage(clamp(su, -12.f, 12.f));
        outputs[EV_OUTPUT].setVoltage(clamp(ev, -12.f, 12.f));
    }
};

// ============================================================================
// Visualization — value function on top, weighting function on bottom.
// ============================================================================

struct ProspectView : LightWidget {
    Prospect* module = nullptr;

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
                    "Û(x)   AND   ŵ(p)", nullptr);
            return;
        }

        float pad = 6.f, gap = 6.f;
        float W = box.size.x - 2 * pad;
        float halfH = (box.size.y - 2 * pad - gap) / 2.f;

        // --- Value function on top half ---
        float x0 = pad, y0 = pad;
        // axes (x: -5..5; y: -10..10 for value units)
        auto mapX = [&](float xv) { return x0 + W * (xv + 5.f) / 10.f; };
        auto mapY = [&](float v)  { return y0 + halfH * (1.f - (v + 10.f) / 20.f); };

        // Reference axes
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 140));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapX(0.f), y0); nvgLineTo(vg, mapX(0.f), y0 + halfH);
        nvgMoveTo(vg, x0, mapY(0.f)); nvgLineTo(vg, x0 + W, mapY(0.f));
        nvgStroke(vg);

        // 45° linear reference
        nvgStrokeColor(vg, nvgRGBA(120, 130, 150, 100));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapX(-5.f), mapY(-5.f));
        nvgLineTo(vg, mapX( 5.f), mapY( 5.f));
        nvgStroke(vg);

        // Curve
        nvgBeginPath(vg);
        bool first = true;
        const int N = 100;
        for (int i = 0; i <= N; ++i) {
            float xv = -5.f + 10.f * i / N;
            float v  = module->utility(xv);
            float px = mapX(xv), py = mapY(clamp(v, -10.f, 10.f));
            if (first) { nvgMoveTo(vg, px, py); first = false; }
            else        nvgLineTo(vg, px, py);
        }
        nvgStrokeColor(vg, nvgRGB(110, 200, 220));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        // Operating point
        float xObs = clamp(module->inputs[Prospect::X_INPUT].getVoltage(), -5.f, 5.f);
        float uObs = module->utility(xObs);
        nvgBeginPath(vg);
        nvgCircle(vg, mapX(xObs), mapY(clamp(uObs, -10.f, 10.f)), 2.4f);
        nvgFillColor(vg, nvgRGB(245, 200, 90));
        nvgFill(vg);

        // Label
        nvgFontSize(vg, 7.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, x0 + 2, y0 + 2, "Û(x)", nullptr);

        // --- Weighting function on bottom half ---
        float y1 = pad + halfH + gap;
        auto mapXp = [&](float pv) { return x0 + W * pv; };
        auto mapYw = [&](float wv) { return y1 + halfH * (1.f - wv); };

        // Reference axes
        nvgStrokeColor(vg, nvgRGBA(60, 70, 90, 140));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapXp(0.f), y1); nvgLineTo(vg, mapXp(0.f), y1 + halfH);
        nvgMoveTo(vg, x0, mapYw(0.f)); nvgLineTo(vg, x0 + W, mapYw(0.f));
        nvgStroke(vg);

        // 45° linear reference w(p) = p
        nvgStrokeColor(vg, nvgRGBA(120, 130, 150, 100));
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, mapXp(0.f), mapYw(0.f));
        nvgLineTo(vg, mapXp(1.f), mapYw(1.f));
        nvgStroke(vg);

        // Curve
        nvgBeginPath(vg);
        first = true;
        for (int i = 0; i <= N; ++i) {
            float pv = (float)i / N;
            float w  = module->weight(pv);
            float px = mapXp(pv), py = mapYw(clamp(w, 0.f, 1.f));
            if (first) { nvgMoveTo(vg, px, py); first = false; }
            else        nvgLineTo(vg, px, py);
        }
        nvgStrokeColor(vg, nvgRGB(245, 130, 130));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        // Operating point on weighting
        float pObs = clamp(module->inputs[Prospect::P_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        float wObs = module->weight(pObs);
        nvgBeginPath(vg);
        nvgCircle(vg, mapXp(pObs), mapYw(clamp(wObs, 0.f, 1.f)), 2.4f);
        nvgFillColor(vg, nvgRGB(245, 200, 90));
        nvgFill(vg);

        nvgFillColor(vg, nvgRGBA(150, 160, 180, 200));
        nvgText(vg, x0 + 2, y1 + 2, "ŵ(p)", nullptr);

        // Frame
        nvgBeginPath(vg);
        nvgRect(vg, 0.5f, 0.5f, box.size.x - 1, box.size.y - 1);
        nvgStrokeColor(vg, nvgRGB(43, 47, 66));
        nvgStrokeWidth(vg, 0.5f);
        nvgStroke(vg);
    }
};

struct ProspectWidget : ModuleWidget {
    ProspectWidget(Prospect* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Prospect.svg")));
        addChild(new ModuleTitle("PROSPECT", 300.f));

        auto* labels = new PanelLabels(300.f);
        labels->k1(0, "α"); labels->k1(1, "λ");
        labels->k1(2, "γ"); labels->k1(3, "β");
        labels->inSection();
        labels->in(0, "X"); labels->in(1, "P");
        labels->outSection();
        labels->out(0, "U");  labels->out(1, "W");
        labels->out(2, "SU"); labels->out(3, "EV");
        addChild(labels);

        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(270, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(270, 365)));

        auto* view = new ProspectView;
        view->module = module;
        view->box.pos  = Vec(10, 44);
        view->box.size = Vec(280, 190);
        addChild(view);

        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(45,  258), module, Prospect::ALPHA_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(120, 258), module, Prospect::LAMBDA_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(195, 258), module, Prospect::GAMMA_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(
            Vec(270, 258), module, Prospect::BETA_PARAM));

        addInput(createInputCentered<PJ301MPort>(
            Vec(45,  327), module, Prospect::X_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            Vec(120, 327), module, Prospect::P_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(
            Vec(45,  358), module, Prospect::U_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(120, 358), module, Prospect::W_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(195, 358), module, Prospect::SU_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            Vec(270, 358), module, Prospect::EV_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        appendAboutMenu(menu, "Prospect",
            {"Prospect-theory subjective value function with reference",
             "dependence, loss aversion, and probability weighting.",
             "Outputs subjective value v(x) and decision weight π(p)."},
            "Gauge (preset 'Probability' or 'Percent'), Frame");
    }
};

Model* modelProspect = createModel<Prospect, ProspectWidget>("Prospect");
