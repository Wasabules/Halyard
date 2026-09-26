/* Equalizer screen - cf. gestures_settings_activity, same structure. */

#include "activity/eq_settings_activity.hpp"
#include "eq_view.hpp"
#include "../ui/screen_base.hpp"

brls::View *EqSettingsActivity::createContentView()
{
    view = new EqView();
    return view;
}

void EqSettingsActivity::onContentAvailable()
{
    ui::wireNavigation(this, view);
}
