/* devui - a small mouse-driven menu library.
 *
 * Generic: it knows nothing of Shadow or Borealis, only nanovg. The menu content
 * lives in dev_menu.cpp, the facade in devui.hpp.
 *
 * The whole thing is desktop-only. See devui.hpp for how it disappears from the
 * Switch build.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nanovg.h>

namespace devui {

/* Metrics and colours. Everything is in Borealis LOGICAL pixels. */
struct Style {
    float barHeight       = 26.0f;
    float paddingX        = 14.0f;
    float itemHeight      = 24.0f;
    float separatorHeight = 9.0f;
    float fontSize        = 14.0f;
    float menuMinWidth    = 240.0f;

    NVGcolor barBg      = nvgRGBA( 28,  28,  32, 235);
    NVGcolor barBorder  = nvgRGBA( 70,  70,  78, 255);
    NVGcolor menuBg     = nvgRGBA( 34,  34,  40, 246);
    NVGcolor menuBorder = nvgRGBA( 80,  80,  90, 255);
    NVGcolor highlight  = nvgRGBA( 56,  96, 168, 255);
    NVGcolor text       = nvgRGBA(228, 228, 232, 255);
    NVGcolor textDim    = nvgRGBA(150, 150, 158, 255);
    NVGcolor mark       = nvgRGBA(110, 200, 120, 255);
};

struct DrawCtx {
    NVGcontext  *vg;
    const Style &style;
};

/* ---------------------------------------------------------------- entrees */

class Item {
public:
    explicit Item(std::string label) : label_(std::move(label)) {}
    virtual ~Item() = default;

    const std::string &label() const { return label_; }

    virtual float height(const Style &s) const { return s.itemHeight; }
    /* A non-selectable entry reacts neither to hover nor to click (a
     * separator, an information line). */
    virtual bool  selectable() const { return true; }
    virtual float measure(const DrawCtx &c) const;
    virtual void  draw(const DrawCtx &c, float x, float y, float w,
                       bool hovered) const = 0;
    /* Returns true when the menu must close after the activation. */
    virtual bool  activate() { return true; }

protected:
    /* Hover background + label, common to every clickable entry. */
    void drawRow(const DrawCtx &c, float x, float y, float w, bool hovered,
                 float textIndent) const;

    std::string label_;
};

/* Triggers an action and closes the menu. */
class Action : public Item {
public:
    Action(std::string label, std::function<void()> fn)
        : Item(std::move(label)), fn_(std::move(fn)) {}
    void draw(const DrawCtx &, float, float, float, bool) const override;
    bool activate() override;
private:
    std::function<void()> fn_;
};

/* Checkbox. Stays open after the click: you see the box change. */
class Toggle : public Item {
public:
    Toggle(std::string label, std::function<bool()> get, std::function<void()> flip)
        : Item(std::move(label)), get_(std::move(get)), flip_(std::move(flip)) {}
    void draw(const DrawCtx &, float, float, float, bool) const override;
    bool activate() override;
private:
    std::function<bool()> get_;
    std::function<void()> flip_;
};

/* Exclusive choice within a group: a dot instead of a box. */
class Radio : public Item {
public:
    Radio(std::string label, std::function<bool()> selected, std::function<void()> select)
        : Item(std::move(label)), selected_(std::move(selected)), select_(std::move(select)) {}
    void draw(const DrawCtx &, float, float, float, bool) const override;
    bool activate() override;
private:
    std::function<bool()> selected_;
    std::function<void()> select_;
};

/* Read-only line: label on the left, value re-read on every frame. */
class Info : public Item {
public:
    Info(std::string label, std::function<std::string()> value)
        : Item(std::move(label)), value_(std::move(value)) {}
    bool  selectable() const override { return false; }
    float measure(const DrawCtx &) const override;
    void  draw(const DrawCtx &, float, float, float, bool) const override;
private:
    std::function<std::string()> value_;
};

class Separator : public Item {
public:
    Separator() : Item(std::string()) {}
    float height(const Style &s) const override { return s.separatorHeight; }
    bool  selectable() const override { return false; }
    float measure(const DrawCtx &) const override { return 0.0f; }
    void  draw(const DrawCtx &, float, float, float, bool) const override;
};

/* ------------------------------------------------------------------ menu */

/* Construction fluide : m.toggle(...).action(...).separator() ... */
class Menu {
public:
    explicit Menu(std::string title) : title_(std::move(title)) {}

    Menu &action(std::string label, std::function<void()> fn);
    Menu &toggle(std::string label, std::function<bool()> get,
                 std::function<void()> flip);
    Menu &radio(std::string label, std::function<bool()> selected,
                std::function<void()> select);
    Menu &info(std::string label, std::function<std::string()> value);
    Menu &separator();

    const std::string &title() const { return title_; }

private:
    friend class MenuBar;
    std::string                        title_;
    std::vector<std::unique_ptr<Item>> items_;
    /* Geometry recomputed on every frame by MenuBar::layout(). */
    float titleX_ = 0, titleW_ = 0, width_ = 0;
};

/* -------------------------------------------------------------- barre */

class MenuBar {
public:
    Menu &add(std::string title);
    void  clear();

    Style       &style()       { return style_; }
    const Style &style() const { return style_; }
    /* Height in the caller's units: the bar is drawn to scale, and that is the
     * height the video must reserve at the top. */
    float        height() const { return style_.barHeight * scale_; }
    /* Same role as ui::Hud::setScale: constant apparent size as the window
     * grows. Without it the bar ate more and more height. */
    void         setScale(float s) { scale_ = (s > 0.05f) ? s : 0.05f; }
    bool         isOpen() const { return open_ >= 0; }
    bool         empty() const  { return menus_.empty(); }

    /* Hover: to be called before draw(), in logical coordinates. */
    void pointerMoved(float px, float py);
    /* Returns true when the click is consumed (so must NOT go to the VM). */
    bool onMouseButton(float px, float py, bool pressed);
    /* Returns true when the cursor is over the bar or over an open menu. */
    bool hovers(float px, float py) const;

    void draw(NVGcontext *vg, float x, float y, float width);

private:
    void  layout(NVGcontext *vg);
    bool  dropdown(int menuIdx, float &x, float &y, float &w, float &h) const;
    int   itemAt(int menuIdx, float px, float py) const;

    Style                              style_;
    float                              scale_ = 1.0f;
    std::vector<std::unique_ptr<Menu>> menus_;
    int   open_       = -1;
    int   hoverTitle_ = -1;
    int   hoverItem_  = -1;
    float x_ = 0, y_ = 0, w_ = 0;
    bool  laidOut_ = false;
};

}  // namespace devui
