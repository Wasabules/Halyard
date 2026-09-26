/* Stream quality screen - the activity now does nothing but wire up the
 * buttons; all the display is in QualityView. */

#include "activity/quality_settings_activity.hpp"
#include "quality_view.hpp"
#include "../ui/screen_base.hpp"
#include "../ui/i18n.hpp"

brls::View *QualitySettingsActivity::createContentView()
{
    view = new QualityView();
    return view;
}

void QualitySettingsActivity::onContentAvailable()
{
    QualityView *v = view;

    /* One call replaces the five or six identical registrations every activity
     * used to repeat. They now live in one place for everyone: a screen can no
     * longer behave differently from the others through an oversight. */
    ui::wireNavigation(this, v);
}
