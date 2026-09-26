/* GesturesView - see the header. */

#include "gestures_view.hpp"

#include "settings.hpp"
#include "../ui/i18n.hpp"

extern "C" {
#include "../ui/gestures.h"
}

#include <string>
#include <vector>

namespace {

/* Display order = the order of `gesture_t`, minus the zero entry. The finger
 * count is a separate heading from the gesture type so there are only six
 * strings to translate instead of nine whole sentences. */
struct Row { gesture_t gesture; int fingers; const char *type_key; };
const Row ROWS[] = {
    { GESTURE_TAP_1,   1, "gestures/tap"   },
    { GESTURE_DOUBLE_1, 1, "gestures/double" },
    { GESTURE_LONG_1,   1, "gestures/long"   },
    { GESTURE_TAP_2,   2, "gestures/tap"   },
    { GESTURE_DOUBLE_2, 2, "gestures/double" },
    { GESTURE_LONG_2,   2, "gestures/long"   },
    { GESTURE_TAP_3,   3, "gestures/tap"   },
    { GESTURE_DOUBLE_3, 3, "gestures/double" },
    { GESTURE_LONG_3,   3, "gestures/long"   },
};

const char *ACTION_KEY[] = {
    "gestures/act_none", "gestures/act_click_l", "gestures/act_click_r",
    "gestures/act_click_m", "gestures/act_double", "gestures/act_drag",
    "gestures/act_keyboard", "gestures/act_menu", "gestures/act_escape",
    "gestures/act_windows", "gestures/act_alttab",
};

bool is_double(gesture_t g)
{
    return g == GESTURE_DOUBLE_1 || g == GESTURE_DOUBLE_2 || g == GESTURE_DOUBLE_3;
}

std::vector<std::string> actionList(bool for_double)
{
    std::vector<std::string> v;
    for (int i = 0; i < (int)GestureAction::Count; i++) {
        /* On a "double" row, entry 0 does not mean "none" ("gestures/act_none")
         * but "repeat the single tap" ("gestures/act_auto") - which is what the
         * remote system does when it is sent two clicks. The right wording is
         * what stops the row from reading as "double-tap is off". */
        if (i == 0 && for_double) v.push_back(ui::tr("gestures/act_auto"));
        else                      v.push_back(ui::tr(ACTION_KEY[i]));
    }
    return v;
}

ui::Item heading(const std::string &t)
{
    ui::Item i; i.kind = ui::Kind::Title; i.title = t; i.actionable = false;
    return i;
}

}  // namespace

GesturesView::GesturesView()
{
    screen_.setTitle(ui::tr("gestures/title"));
    screen_.setSubtitle(ui::tr("gestures/subtitle"));
    screen_.setHints({ { "B", ui::tr("action/back") } });
    rebuild();
}

void GesturesView::rebuild()
{
    Settings &s = Settings::instance();
    std::vector<ui::Item> v;

    int fingers_shown = 0;
    for (const Row &r : ROWS) {
        if (r.fingers != fingers_shown) {
            fingers_shown = r.fingers;
            char t[64];
            std::snprintf(t, sizeof t, ui::tr("gestures/fingers").c_str(), fingers_shown);
            v.push_back(heading(t));
        }

        ui::Item i;
        i.kind = ui::Kind::Choice;
        i.id    = (int)r.gesture;
        i.title = ui::tr(r.type_key);

        /* The latency warning shows up ONLY when it applies: displaying it
         * permanently is what would make it invisible. */
        if (is_double(r.gesture)) {
            i.subtitle = (s.gesture_action[r.gesture] == (uint8_t)GestureAction::None)
                         ? ui::tr("gestures/double_auto_desc")
                         : ui::tr("gestures/double_latency_desc");
        }

        i.choice = actionList(is_double(r.gesture));
        int a = (int)s.gesture_action[r.gesture];
        if (a < 0 || a >= (int)GestureAction::Count) a = 0;
        i.choice_index = a;
        v.push_back(std::move(i));
    }

    screen_.setItems(std::move(v));
}

bool GesturesView::apply(const ui::Item &it)
{
    if (it.id <= 0 || it.id >= SETTINGS_GESTURES_COUNT) return false;
    const int a = it.choice_index;
    if (a < 0 || a >= (int)GestureAction::Count) return false;

    Settings &s = Settings::instance();
    s.gesture_action[it.id] = (uint8_t)a;
    s.save();
    return true;
}

bool GesturesView::left()
{
    if (!screen_.left()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it && apply(*it)) rebuild();   /* the warning may change */
    return true;
}

bool GesturesView::right()
{
    if (!screen_.right()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it && apply(*it)) rebuild();
    return true;
}

void GesturesView::paint(NVGcontext *vg, float x, float y, float w, float h,
                         double t)
{
    screen_.draw(vg, x, y, w, h, t);
    /* A choice is not activated by touch: touching a row SELECTS it, and the
     * value is then changed with left/right. */
    (void)screen_.touchRelease();
}
