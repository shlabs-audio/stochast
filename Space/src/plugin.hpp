#pragma once
#include <rack.hpp>
#include <string>
#include <vector>

using namespace rack;

extern Plugin* pluginInstance;

extern Model* modelLife;
extern Model* modelSchelling;
extern Model* modelTuring;

// Renders the module title across the header strip via NanoVG.
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
    }
};

// 20HP panel labels via NanoVG. See Methods/plugin.hpp for the full spec.
struct PanelLabels : Widget {
    struct Item {
        float x, y, fontSize;
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
    void k1(int col, const std::string& t) {
        items.push_back({kCols[col], 246.f, 8.f, nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.5f, t});
    }
    void k2(int col, const std::string& t) {
        items.push_back({kCols[col], 282.f, 7.5f, nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.5f, t});
    }
    void in(int col, const std::string& t) {
        items.push_back({kCols[col], 313.f, 7.5f, nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, t});
    }
    void out(int col, const std::string& t) {
        items.push_back({kCols[col], 344.f, 7.5f, nvgRGB(0x9a, 0xa0, 0xb4),
                         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 1.2f, t});
    }
    void inSection() {
        items.push_back({10.f, 313.f, 7.f, nvgRGB(0x6a, 0x70, 0x88),
                         NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 2.f, "IN"});
    }
    void outSection() {
        items.push_back({10.f, 344.f, 7.f, nvgRGB(0x6a, 0x70, 0x88),
                         NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 2.f, "OUT"});
    }
    void custom(float x, float y, float fs, NVGcolor c, int al, float ls,
                const std::string& t) {
        items.push_back({x, y, fs, c, al, ls, t});
    }
    void draw(const DrawArgs& args) override {
        Widget::draw(args);
        NVGcontext* vg = args.vg;
        if (!APP->window->uiFont) return;
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

// Right-click "What does this do?" helper. See Methods/plugin.hpp.
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
