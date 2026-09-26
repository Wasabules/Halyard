/* Controller screen - the activity does nothing but wire up the buttons; all
 * the rest is in PadView. */

#include "activity/pad_settings_activity.hpp"
#include "pad_view.hpp"
#include "../ui/screen_base.hpp"
#include "../ui/i18n.hpp"

brls::View *PadSettingsActivity::createContentView()
{
    view = new PadView();
    return view;
}

void PadSettingsActivity::onContentAvailable()
{
    PadView *v = view;

    /* One call replaces the identical registrations every activity used to
     * repeat. This screen CONSUMES B - `PadView::retour()` returns true while
     * the tester is open - so B closes the tester before the activity pops. */
    ui::wireNavigation(this, v);
}
