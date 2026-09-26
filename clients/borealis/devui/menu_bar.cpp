/* devui - the menu bar implementation. See menu_bar.hpp. */
#include "menu_bar.hpp"

namespace devui {

namespace {

bool inRect(float px, float py, float x, float y, float w, float h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

float textWidth(NVGcontext *vg, const std::string &s)
{
    if (s.empty()) return 0.0f;
    float bounds[4] = {0, 0, 0, 0};
    nvgTextBounds(vg, 0, 0, s.c_str(), nullptr, bounds);
    return bounds[2] - bounds[0];
}

/* Left margin of the label: leaves room for the checkbox or the dot. */
const float MARK_INDENT = 30.0f;

}  // namespace

/* ---------------------------------------------------------------- entrees */

float Item::measure(const DrawCtx &c) const
{
    return MARK_INDENT + textWidth(c.vg, label_) + c.style.paddingX;
}

void Item::drawRow(const DrawCtx &c, float x, float y, float w, bool hovered,
                   float textIndent) const
{
    NVGcontext *vg = c.vg;
    if (hovered) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, c.style.itemHeight);
        nvgFillColor(vg, c.style.highlight);
        nvgFill(vg);
    }
    nvgFillColor(vg, c.style.text);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + textIndent, y + c.style.itemHeight / 2.0f,
            label_.c_str(), nullptr);
}

void Action::draw(const DrawCtx &c, float x, float y, float w, bool hovered) const
{
    drawRow(c, x, y, w, hovered, MARK_INDENT);
}

bool Action::activate()
{
    if (fn_) fn_();
    return true;   /* an action closes the menu */
}

void Toggle::draw(const DrawCtx &c, float x, float y, float w, bool hovered) const
{
    drawRow(c, x, y, w, hovered, MARK_INDENT);
    NVGcontext *vg = c.vg;
    float bx = x + 10.0f, by = y + (c.style.itemHeight - 11.0f) / 2.0f;
    nvgBeginPath(vg);
    nvgRect(vg, bx, by, 11.0f, 11.0f);
    nvgStrokeColor(vg, c.style.textDim);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    if (get_ && get_()) {
        nvgBeginPath(vg);
        nvgRect(vg, bx + 2.5f, by + 2.5f, 6.0f, 6.0f);
        nvgFillColor(vg, c.style.mark);
        nvgFill(vg);
    }
}

bool Toggle::activate()
{
    if (flip_) flip_();
    return false;   /* we stay open: the box must be seen changing */
}

void Radio::draw(const DrawCtx &c, float x, float y, float w, bool hovered) const
{
    drawRow(c, x, y, w, hovered, MARK_INDENT);
    NVGcontext *vg = c.vg;
    float cx = x + 15.5f, cy = y + c.style.itemHeight / 2.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 5.5f);
    nvgStrokeColor(vg, c.style.textDim);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    if (selected_ && selected_()) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 3.0f);
        nvgFillColor(vg, c.style.mark);
        nvgFill(vg);
    }
}

bool Radio::activate()
{
    if (select_) select_();
    return false;   /* we stay open: handy for sweeping a group */
}

float Info::measure(const DrawCtx &c) const
{
    std::string v = value_ ? value_() : std::string();
    return MARK_INDENT + textWidth(c.vg, label_) + 24.0f
           + textWidth(c.vg, v) + c.style.paddingX;
}

void Info::draw(const DrawCtx &c, float x, float y, float w, bool) const
{
    NVGcontext *vg = c.vg;
    nvgFillColor(vg, c.style.textDim);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + MARK_INDENT, y + c.style.itemHeight / 2.0f,
            label_.c_str(), nullptr);
    if (value_) {
        std::string v = value_();
        nvgFillColor(vg, c.style.text);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + w - c.style.paddingX, y + c.style.itemHeight / 2.0f,
                v.c_str(), nullptr);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
}

void Separator::draw(const DrawCtx &c, float x, float y, float w, bool) const
{
    NVGcontext *vg = c.vg;
    float sy = y + c.style.separatorHeight / 2.0f;
    nvgBeginPath(vg);
    nvgMoveTo(vg, x + 8.0f, sy);
    nvgLineTo(vg, x + w - 8.0f, sy);
    nvgStrokeColor(vg, c.style.menuBorder);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
}

/* ------------------------------------------------------------------ menu */

Menu &Menu::action(std::string label, std::function<void()> fn)
{
    items_.push_back(std::unique_ptr<Item>(new Action(std::move(label), std::move(fn))));
    return *this;
}

Menu &Menu::toggle(std::string label, std::function<bool()> get,
                   std::function<void()> flip)
{
    items_.push_back(std::unique_ptr<Item>(
        new Toggle(std::move(label), std::move(get), std::move(flip))));
    return *this;
}

Menu &Menu::radio(std::string label, std::function<bool()> selected,
                  std::function<void()> select)
{
    items_.push_back(std::unique_ptr<Item>(
        new Radio(std::move(label), std::move(selected), std::move(select))));
    return *this;
}

Menu &Menu::info(std::string label, std::function<std::string()> value)
{
    items_.push_back(std::unique_ptr<Item>(new Info(std::move(label), std::move(value))));
    return *this;
}

Menu &Menu::separator()
{
    items_.push_back(std::unique_ptr<Item>(new Separator()));
    return *this;
}

/* ----------------------------------------------------------------- barre */

Menu &MenuBar::add(std::string title)
{
    menus_.push_back(std::unique_ptr<Menu>(new Menu(std::move(title))));
    return *menus_.back();
}

void MenuBar::clear()
{
    menus_.clear();
    open_ = hoverTitle_ = hoverItem_ = -1;
    laidOut_ = false;
}

void MenuBar::layout(NVGcontext *vg)
{
    DrawCtx c{vg, style_};
    float tx = x_ + style_.paddingX;
    for (auto &m : menus_) {
        m->titleX_ = tx;
        m->titleW_ = textWidth(vg, m->title_) + style_.paddingX * 1.6f;
        tx += m->titleW_;

        float wanted = style_.menuMinWidth;
        for (auto &it : m->items_) {
            float need = it->measure(c);
            if (need > wanted) wanted = need;
        }
        m->width_ = wanted;
    }
    laidOut_ = true;
}

bool MenuBar::dropdown(int idx, float &x, float &y, float &w, float &h) const
{
    if (!laidOut_ || idx < 0 || idx >= (int)menus_.size()) return false;
    const Menu &m = *menus_[idx];
    x = m.titleX_ - style_.paddingX * 0.5f;
    y = y_ + style_.barHeight;
    w = m.width_;
    h = 8.0f;
    for (auto &it : m.items_) h += it->height(style_);
    return true;
}

int MenuBar::itemAt(int idx, float px, float py) const
{
    float mx, my, mw, mh;
    if (!dropdown(idx, mx, my, mw, mh)) return -1;
    if (!inRect(px, py, mx, my, mw, mh)) return -1;
    const Menu &m = *menus_[idx];
    float iy = my + 4.0f;
    for (size_t i = 0; i < m.items_.size(); i++) {
        float ih = m.items_[i]->height(style_);
        if (py >= iy && py < iy + ih)
            return m.items_[i]->selectable() ? (int)i : -1;
        iy += ih;
    }
    return -1;
}

void MenuBar::pointerMoved(float px, float py)
{
    if (!laidOut_) return;
    px /= scale_; py /= scale_;

    hoverTitle_ = -1;
    for (size_t i = 0; i < menus_.size(); i++) {
        if (inRect(px, py, menus_[i]->titleX_ - style_.paddingX * 0.5f, y_,
                   menus_[i]->titleW_, style_.barHeight)) {
            hoverTitle_ = (int)i;
            break;
        }
    }
    /* An open menu follows the hover over the other titles, as everywhere
     * else: once the bar is "engaged", you walk the menus without clicking
     * again. */
    if (open_ >= 0 && hoverTitle_ >= 0 && hoverTitle_ != open_)
        open_ = hoverTitle_;

    hoverItem_ = (open_ >= 0) ? itemAt(open_, px, py) : -1;
}

bool MenuBar::hovers(float px, float py) const
{
    if (menus_.empty() || !laidOut_) return false;
    px /= scale_; py /= scale_;
    if (inRect(px, py, x_, y_, w_, style_.barHeight)) return true;
    float mx, my, mw, mh;
    if (dropdown(open_, mx, my, mw, mh) && inRect(px, py, mx, my, mw, mh))
        return true;
    return false;
}

bool MenuBar::onMouseButton(float px, float py, bool pressed)
{
    if (menus_.empty() || !laidOut_) return false;

    if (!pressed) {
        /* We also swallow the release of the click we consumed, otherwise the
         * VM receives a release with no press and keeps a phantom button. */
        return hovers(px, py);
    }

    px /= scale_; py /= scale_;

    for (size_t i = 0; i < menus_.size(); i++) {
        if (inRect(px, py, menus_[i]->titleX_ - style_.paddingX * 0.5f, y_,
                   menus_[i]->titleW_, style_.barHeight)) {
            open_ = (open_ == (int)i) ? -1 : (int)i;
            hoverItem_ = -1;
            return true;
        }
    }

    if (open_ >= 0) {
        int idx = itemAt(open_, px, py);
        if (idx >= 0) {
            if (menus_[open_]->items_[idx]->activate())
                open_ = -1;
            return true;
        }
        /* Click outside the menu: we close, and we swallow that click - it served
         * to close, not to act on the remote machine. */
        open_ = -1;
        hoverItem_ = -1;
        return true;
    }

    return inRect(px, py, x_, y_, w_, style_.barHeight);
}

void MenuBar::draw(NVGcontext *vg, float x, float y, float width)
{
    if (!vg || menus_.empty()) return;

    const float k = scale_;
    nvgSave(vg);
    nvgScale(vg, k, k);
    x /= k; y /= k; width /= k;

    x_ = x; y_ = y; w_ = width;

    nvgFontFace(vg, "regular");
    nvgFontSize(vg, style_.fontSize);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    layout(vg);

    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, style_.barHeight);
    nvgFillColor(vg, style_.barBg);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x, y + style_.barHeight);
    nvgLineTo(vg, x + width, y + style_.barHeight);
    nvgStrokeColor(vg, style_.barBorder);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    for (size_t i = 0; i < menus_.size(); i++) {
        const Menu &m = *menus_[i];
        bool active = ((int)i == open_);
        bool hover  = ((int)i == hoverTitle_);
        if (active || hover) {
            nvgBeginPath(vg);
            nvgRect(vg, m.titleX_ - style_.paddingX * 0.5f, y,
                    m.titleW_, style_.barHeight);
            nvgFillColor(vg, active ? style_.highlight
                                    : nvgRGBA(60, 60, 68, 255));
            nvgFill(vg);
        }
        nvgFillColor(vg, style_.text);
        nvgText(vg, m.titleX_, y + style_.barHeight / 2.0f,
                m.title_.c_str(), nullptr);
    }

    float mx, my, mw, mh;
    if (!dropdown(open_, mx, my, mw, mh)) { nvgRestore(vg); return; }

    nvgBeginPath(vg);
    nvgRect(vg, mx, my, mw, mh);
    nvgFillColor(vg, style_.menuBg);
    nvgFill(vg);
    nvgStrokeColor(vg, style_.menuBorder);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    DrawCtx c{vg, style_};
    const Menu &m = *menus_[open_];
    float iy = my + 4.0f;
    for (size_t i = 0; i < m.items_.size(); i++) {
        float ih = m.items_[i]->height(style_);
        m.items_[i]->draw(c, mx, iy, mw, (int)i == hoverItem_);
        iy += ih;
    }

    nvgRestore(vg);
}

}  // namespace devui
