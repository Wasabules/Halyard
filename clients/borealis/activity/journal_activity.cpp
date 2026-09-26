/* Journal screen - cf. gestures_settings_activity, same structure.
 *
 * One difference only: an extra button. `ui::cablerNavigation` wires the four
 * directions, the triggers, A and B - everything a screen has in common with the
 * others. X is NOT in there, and it must not be: it means nothing on five
 * screens out of six. So it is wired here, in the only screen that uses it, and
 * the footer hint tells the user about it (JournalView).
 */
#include "activity/journal_activity.hpp"
#include "journal_view.hpp"
#include "../ui/screen_base.hpp"
#include "../ui/sfx.hpp"

brls::View *JournalActivity::createContentView()
{
    view = new JournalView();
    return view;
}

void JournalActivity::onContentAvailable()
{
    ui::wireNavigation(this, view);

    /* No auto-repeat: re-reading the log flushes the write buffer and reads the
     * SD card. Holding X down would trigger ten reads per second on a card the
     * log itself is writing to at the same time. */
    JournalView *v = view;
    registerAction("", brls::ControllerButton::BUTTON_X,
        [v](brls::View *) {
            v->reload();
            ui::sfx::play(ui::sfx::Sound::Confirm);
            return true;
        });
}
