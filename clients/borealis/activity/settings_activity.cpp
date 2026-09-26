/* Settings screen - now drawn by the in-house framework.
 *
 * The activity does only three things: create the view, route its actions to
 * the other screens, and wire up the buttons. All the display and all the
 * state are in SettingsView - see its header for the four defects that
 * motivated the port.
 */
#include "activity/boot_activity.hpp"
#include "activity/settings_activity.hpp"
#include "settings_view.hpp"
#include "activity/quality_settings_activity.hpp"
#include "activity/pad_settings_activity.hpp"
#include "activity/gestures_settings_activity.hpp"
#include "activity/eq_settings_activity.hpp"
#include "activity/journal_activity.hpp"
#include "activity/shadow_app.hpp"
#include "settings.hpp"
#include "../ui/screen_base.hpp"
#include "../ui/i18n.hpp"

brls::View *SettingsActivity::createContentView()
{
    view = new SettingsView();
    return view;
}

void SettingsActivity::onContentAvailable()
{
    SettingsView *v = view;

    v->setOnAction([](int id) {
        switch (id) {
            case SET_VIDEO:
                brls::Application::pushActivity(new QualitySettingsActivity());
                break;
            case SET_GAMEPAD:
                brls::Application::pushActivity(new PadSettingsActivity());
                break;
            case SET_GESTURES:
                brls::Application::pushActivity(new GesturesSettingsActivity());
                break;
            case SET_EQ:
                brls::Application::pushActivity(new EqSettingsActivity());
                break;
            /* S86 - reading the log is a PAGE, not a dialog: you scroll in it,
             * you come back from it with B, and it has its own buttons.
             * Clearing, on the other hand, stays with the view
             * (`askClearLogs`), because it does not leave this screen and has
             * to rebuild it afterwards. */
            case SET_LOG_VIEW:
                brls::Application::pushActivity(new JournalActivity());
                break;
            case SET_LOGOUT: {
                brls::Dialog *dlg = new brls::Dialog(ui::tr("settings/logout_question"));
                dlg->setCancelable(true);
                /* S66 - no `close()`: `Dialog::buttonClick` already removes the
                 * dialog, and a second pop would take this screen with it. */
                dlg->addButton(ui::tr("action/cancel"), []() { });
                dlg->addButton(ui::tr("action/logout"), []() {
                    ShadowApp::instance().logout();
                });
                dlg->open();
                break;
            }
            /* S110 - no confirmation dialog: these are demo gestures, repeated
             * often, and asking every time would make them a chore. They
             * destroy nothing unrecoverable - the worst case is redoing the
             * pairing, which is precisely what we want to film. */
            case SET_REPLAY:
            case SET_REPLAY_AUTH:
                ShadowApp::instance().prepareRestart(id == SET_REPLAY_AUTH);
                brls::Application::pushActivity(new BootActivity());
                break;
            default: break;
        }
    });

    /* One call replaces the identical registrations every activity used to
     * repeat. They now live in one place for everybody: a screen can no longer
     * end up reacting differently from the others by omission. The hint labels
     * stay with the screen, which alone knows what A means where it is. */
    ui::wireNavigation(this, v);
}

void SettingsActivity::onResume()
{
    /* We are coming back from a sub-page that may have changed settings: the
     * list is rebuilt, otherwise it would show the state from before. */
    if (view) view->rebuild();
    if (view) brls::Application::giveFocus(view);
}
