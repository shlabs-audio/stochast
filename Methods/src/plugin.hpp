#pragma once
#include <rack.hpp>
#include <string>
#include <vector>

using namespace rack;

extern Plugin* pluginInstance;

extern Model* modelStrata;
extern Model* modelCohort;
extern Model* modelFactor;
extern Model* modelSample;
extern Model* modelFrame;
extern Model* modelRegress;
extern Model* modelTest;
extern Model* modelBoot;
extern Model* modelLag;
extern Model* modelCode;
extern Model* modelTab;
extern Model* modelSeed;
extern Model* modelTape;
extern Model* modelGauge;
extern Model* modelQuantity;

// Renders the module title across the header strip via NanoVG. Used in place
// of SVG <text> in the panel, which VCV Rack's NanoSVG parser does not render.
struct ModuleTitle : Widget {
    std::string text;

    ModuleTitle(const std::string& t, float panelW) : text(t) {
        box.pos = math::Vec(0, 0);
        box.size = math::Vec(panelW, 36);
    }

    void draw(const DrawArgs& args) override {
        Widget::draw(args);
        NVGcontext* vg = args.vg;
        if (!APP->window->uiFont) return;
        nvgFontSize(vg, 15.f);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgTextLetterSpacing(vg, 1.f);
        nvgText(vg, box.size.x / 2.f,         18.f, text.c_str(), nullptr);
        nvgText(vg, box.size.x / 2.f + 0.4f,  18.f, text.c_str(), nullptr);

        // Universal SHLabs maker's mark — bottom-centre, identical on every module.
        nvgFontSize(vg, 7.f);
        nvgFillColor(vg, nvgRGB(0x6e, 0x72, 0x7c));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
        nvgTextLetterSpacing(vg, 3.f);
        nvgText(vg, box.size.x / 2.f, 375.f, "SHLABS", nullptr);
    }
};

// Renders the standard 20HP panel labels via NanoVG. NanoSVG silently drops
// <text> in panel SVGs, so labels (knob row labels, IN/OUT strip labels, jack
// column labels) must be drawn here in C++.
//
// Standard 20HP grid (matches docs/design_spec.md):
//   Column x-positions   : 45, 120, 195, 270
//   Knob row 1 label y   : 246
//   Knob row 2 label y   : 282
//   Input  column label y: 313
//   Output column label y: 344
//   IN / OUT strip label : left-aligned at x = 10
struct PanelLabels : Widget {
    struct Item {
        float x;
        float y;
        float fontSize;
        NVGcolor color;
        int align;
        float letterSpacing;
        std::string text;
    };
    std::vector<Item> items;

    static constexpr float kCols[4] = {45.f, 120.f, 195.f, 270.f};

    PanelLabels(float panelW = 300.f) {
        box.pos = math::Vec(0, 0);
        box.size = math::Vec(panelW, 380);
    }

    // Knob-row-1 column label (e.g. "POP", "MODE")
    void k1(int col, const std::string& text) {
        items.push_back({kCols[col], 246.f, 8.f,
                         nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.5f, text});
    }
    // Knob-row-2 column label (e.g. "INFLUENCE", "CLEAR")
    void k2(int col, const std::string& text) {
        items.push_back({kCols[col], 282.f, 7.5f,
                         nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.5f, text});
    }
    // Input column label above the input jack at this column
    void in(int col, const std::string& text) {
        items.push_back({kCols[col], 313.f, 7.5f,
                         nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, text});
    }
    // Output column label above the output jack at this column
    void out(int col, const std::string& text) {
        items.push_back({kCols[col], 344.f, 7.5f,
                         nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, text});
    }
    // Section markers down the left edge
    void inSection() {
        items.push_back({10.f, 313.f, 7.f,
                         nvgRGB(0x6a, 0x70, 0x88),
                         NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 2.f, "IN"});
    }
    void outSection() {
        items.push_back({10.f, 344.f, 7.f,
                         nvgRGB(0x6a, 0x70, 0x88),
                         NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 2.f, "OUT"});
    }
    // Arbitrary label at exact (x, y) — for non-standard placements
    void custom(float x, float y, float fontSize, NVGcolor color, int align,
                float letterSpacing, const std::string& text) {
        items.push_back({x, y, fontSize, color, align, letterSpacing, text});
    }

    void draw(const DrawArgs& args) override {
        Widget::draw(args);
        NVGcontext* vg = args.vg;
        if (!APP->window->uiFont) return;
        // Clip to the panel rectangle so any over-wide custom label (long
        // caption, large letter-spacing, or both) can't leak onto adjacent
        // modules. The label coordinates are panel-local since this widget
        // covers the whole panel.
        nvgSave(vg);
        nvgScissor(vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        for (const auto& it : items) {
            nvgFontSize(vg, it.fontSize);
            nvgFillColor(vg, it.color);
            nvgTextAlign(vg, it.align);
            nvgTextLetterSpacing(vg, it.letterSpacing);
            nvgText(vg, it.x, it.y, it.text.c_str(), nullptr);
        }
        nvgRestore(vg);
    }
};

// Right-click "What does this do?" helper. Appended to every Empiria module so
// students can read a short explanation + typical companion modules without
// leaving the patch.
inline void appendAboutMenu(Menu* menu,
                            const std::string& name,
                            const std::vector<std::string>& headlineLines,
                            const std::string& companions) {
    menu->addChild(new MenuSeparator);
    menu->addChild(createSubmenuItem("What does this do?", "",
        [name, headlineLines, companions](Menu* sub) {
            sub->addChild(createMenuLabel(name));
            sub->addChild(new MenuSeparator);
            for (const auto& line : headlineLines)
                sub->addChild(createMenuLabel(line));
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuLabel("Companions:"));
            sub->addChild(createMenuLabel(companions));
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuLabel("Full reference: docs/modules.md"));
        }));
}
