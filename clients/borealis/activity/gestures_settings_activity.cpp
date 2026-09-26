/* Touch-gestures screen - same structure as quality_settings_activity. */

#include "activity/gestures_settings_activity.hpp"
#include "gestures_view.hpp"
#include "../ui/screen_base.hpp"

brls::View *GesturesSettingsActivity::createContentView()
{
    view = new GesturesView();
    return view;
}

void GesturesSettingsActivity::onContentAvailable()
{
    ui::wireNavigation(this, view);
}
