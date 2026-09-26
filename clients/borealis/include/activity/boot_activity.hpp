// BootActivity - the first screen. HTTP init, TINAG, an OAuth refresh. If that
// works it pushes VmListActivity; otherwise it shows the error and stays put.

#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>

class BootView;

class BootActivity : public brls::Activity {
public:
    /* S67 - no more XML: the screen IS the view. See boot_view.hpp. */
    brls::View *createContentView() override;

    BootActivity();
    ~BootActivity() override;

    void onContentAvailable() override;

    /* === UX8 2026-09-10 - THIS SCREEN WAS A DEAD END ===
     *
     * Every failure here — expired authentication above all — printed a
     * sentence and stopped. No action was registered, so the footer stayed
     * empty and B did nothing: the only way out was to kill the application,
     * which the message said in so many words ("restart the application").
     * A screen that can fail must carry the way out of its own failure.
     *
     * Publishes or withdraws the "Retry" hint. `registerAction` replaces the
     * action bound to the same button, so re-registering it hidden removes the
     * hint while keeping the binding — the same trick ConnectingActivity uses. */
    void setRetryAvailable(bool available);

private:
    /* The WHOLE boot sequence, so that it can be run AGAIN. It used to sit
     * inline in `onContentAvailable`, which is what made it a one-shot. */
    void startBootFlow();

    /* No XML bindings left: everything goes through `view`. */
    BootView *view = nullptr;

    // Keeps the activity alive for the async callbacks.
    std::shared_ptr<std::atomic<bool>> alive;

    bool retry_available = false;
};
