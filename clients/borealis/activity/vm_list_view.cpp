/* VmListView - see the header for why the architecture is what it is. */

#include "vm_list_view.hpp"

#include "../ui/paint.hpp"

#include <chrono>

VmListView::VmListView()
{
    /* This view is the screen's ONLY focusable one: that is what guarantees
     * `Application::currentFocus` cannot point at something a reload of the
     * list destroys. Its lifetime is the activity's. */
}

void VmListView::setVms(const std::vector<VmRow> &vms)
{
    ids_.clear();
    ids_.reserve(vms.size());

    std::vector<ui::Item> items;
    items.reserve(vms.size());

    for (size_t i = 0; i < vms.size(); i++) {
        const VmRow &v = vms[i];
        ui::Item it;
        it.title       = v.name;
        it.subtitle  = v.subtitle;
        it.badge       = v.badge;
        it.badge_tint = v.lit ? ui::paint::ledActive : ui::paint::ledOff;
        it.active      = v.lit;
        it.actionable = true;
        /* The identifier CARRIED by the item is the index into `ids_`: stable
         * for the lifetime of one list, and `ui::ListScreen` uses it to find
         * the focus again after a reload. The string itself stays here. */
        it.id          = (int)i;
        items.push_back(std::move(it));
        ids_.push_back(v.id);
    }
    screen_.setItems(std::move(items));
}

void VmListView::setHeader(std::string title, std::string subtitle, std::string info)
{
    screen_.setTitle(std::move(title));
    screen_.setSubtitle(std::move(subtitle));
    screen_.setInfo(std::move(info));
}

void VmListView::setStatus(const std::string &s, bool busy)
{
    screen_.setStatus(s, busy);
}

bool VmListView::activate()
{
    /* The dialog comes FIRST: it has the hand as long as it is open. */
    if (modal_.activate()) return true;

    const ui::Item *it = screen_.focusedItem();
    if (!it) return false;                       /* nothing focused: we do not guess */
    if (it->id < 0 || it->id >= (int)ids_.size()) return false;
    if (on_activate_) on_activate_(ids_[(size_t)it->id]);
    return true;
}

bool VmListView::buttonX()
{
    if (!on_refresh_) return false;
    on_refresh_();
    return true;
}

bool VmListView::touch(float x, float y)
{
    const int id = screen_.touchAt(x, y);
    if (id < 0 || id >= (int)ids_.size()) return false;
    if (on_activate_) on_activate_(ids_[(size_t)id]);
    return true;
}

void VmListView::paint(NVGcontext *vg, float x, float y, float w, float h,
                    double t)
{

    /* ONE clock read per frame, handed to everything that animates: the
     * scrolling of long text and the busy indicator must advance together, or
     * the page looks out of sync with itself. */

    screen_.draw(vg, x, y, w, h, t);
    modal_.draw(vg, x, y, w, h, t);

    /* Did a finger activate a card? We ask AFTER drawing: acting while the
     * screen is being drawn would be a reentrancy, and that is what we avoid
     * everywhere else. */
    const int id = screen_.touchRelease();
    if (id >= 0 && id < (int)ids_.size() && on_activate_) on_activate_(ids_[(size_t)id]);
}
