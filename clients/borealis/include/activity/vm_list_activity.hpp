// VmListActivity - shows the Shadow account's VM list as clickable cards. The
// user picks -> push ConnectingActivity.

#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>

/* Declared, not included: this header only needs a pointer, and including it
 * would tie clients/borealis/include/ to . The definition is in
 * clients/borealis/activity/vm_list_view.hpp. */
class VmListView;

class VmListActivity : public brls::Activity {
public:
    /* === S65 2026-08-27 - NO MORE XML: THE SCREEN IS THE VIEW ===
     * S64 injected the in-house view INTO the existing Borealis tree, inside the
     * scrolling frame. Two visible consequences:
     *   - Borealis's chrome (header bar, footer, light theme) stayed all around
     *     it, so nothing seemed to have changed;
     *   - the view was ZERO pixels tall. `setGrow` has no effect inside a
     *     scrolling frame, which measures its children instead of giving them its
     *     height: the machines block therefore disappeared.
     * We now follow the model of the stream screen, which never had XML: the
     * activity RETURNS its view, which fills the screen and paints its own
     * background. */
    brls::View *createContentView() override;

    VmListActivity();
    ~VmListActivity() override;

    void onContentAvailable() override;
    void onResume() override;       // restores the focus and refreshes the VM states
    /* S50: the first appearance already follows a `refreshVms()` from the
     * constructor; only the SUBSEQUENT ones (coming back from a stream) must
     * reload. */
    bool first_appearance = true;

    /* S62 - auto-connect must fire only ONCE per screen lifetime. We cannot
     * rely on `first_appearance`: `refreshVms()` is asynchronous and builds its
     * cards AFTER `onResume` has set it back to false - auto-connect would then
     * never be armed. */
    bool auto_connexion_faite = false;

    /* D6 2026-08-27 - the auto-connect delay must be CANCELLABLE. Without that,
     * pressing Y within the second opens the settings AND THEN the connection
     * fires on top. A `shared_ptr` because the deferred call must be able to read
     * it after the activity is destroyed. */
    std::shared_ptr<std::atomic<bool>> auto_connexion_annulee;

    /* S64 - the whole screen is ONE view, the only focusable one. Reloading the
     * list no longer destroys anything: see vm_list_view.hpp. */
    VmListView *vmView = nullptr;

private:
    /* No XML binding left: all the content goes through `vmView`. The texts that
     * used to live in `Label`s (data centre, plan, Drive, status) are now fields
     * of the header or of the status bar. */
    std::string dc_, offer_, drive_;
    void updateHeader();          /* recomposes the header and the info line from the three fields */

    void fetchAccountInfo();   // subscription + drive_token (background)

    std::shared_ptr<std::atomic<bool>> alive;

    void refreshVms();
    void renderVmCards();   // builds the cards from ShadowApp::vms
    void onVmClicked(const std::string &vm_id, const std::string &alias);
};
