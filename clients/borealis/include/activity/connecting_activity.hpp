// ConnectingActivity - orchestrates the steps of connecting to a Shadow VM:
//   1. Start the VM (POST /vms/{id}/start)
//   2. Wait for the IP (poll /vms/{id}/ip until 200)
//   3. Auth login session (POST /vms/{id}/auth_login)
//   4. Proximus credentials (GET /vms/{id}/proximus-credentials)
//   5. Open the launcher and main clients (POST /clients)
//   6. Status + signalling (GET /status, GET /stream briefly)
//   7. WebRTC handshake (WSS + SDP + ICE + DTLS + SRTP)
//
// Every step shows its visual state: pending (grey) -> running (spinner) ->
// done (green check) -> error (red cross). When the last one turns to done, we
// push StreamActivity.

#pragma once

#include <borealis.hpp>

#include "../../activity/step_list.hpp"
#include <atomic>
#include <memory>
#include <vector>

class ConnectingView;

class ConnectingActivity : public brls::Activity {
public:
    /* S67 - no more XML: the screen IS the view, like the others. The Borealis
     * frame only served to place a title and a footer, which the framework can
     * do itself. */
    brls::View *createContentView() override;

    ConnectingActivity();
    ~ConnectingActivity() override;

    void onContentAvailable() override;
    void onResume() override;       // restaure focus quand StreamActivity pop

private:
    /* No XML bindings left: everything goes through `view`. */
    ConnectingView *view = nullptr;

    std::shared_ptr<std::atomic<bool>> alive;

    /* === S61 2026-08-27 - THIS SCREEN MUST BE POPPED ONLY ONCE ===
     * Two paths remove it: the B button, and the worker thread when the exit is
     * deliberate. Between removing the stream view and this one, the cleanup
     * makes a network call (the client's `DELETE`): the window lasts long enough
     * for a press on B to slip in. Both pops then fired, and the second took the
     * VM LIST with it - you ended up on the boot screen, frozen on its last
     * state, with no way out but quitting the application.
     * The flag is a `shared_ptr` because it must stay readable by the worker
     * thread after the activity is destroyed. */
    std::shared_ptr<std::atomic<bool>> already_popped;

private:
    /* shared_ptr: if the activity is destroyed before the worker finishes, the
     * worker keeps its own reference and does not read a freed vector. The model
     * now holds only text and a state - no more pointers to Labels, hence no more
     * dangling pointers for the liveness flag to cover. */
    app::StepsPtr steps;

    // True when the worker has finished (success or error). Lets us show the
    // retry/back buttons only when appropriate.
    std::atomic<bool> finished{false};
    std::atomic<bool> succeeded{false};

    void buildSteps();
    void runConnectionFlow();
    void showError(const std::string &msg);
public:
    /* Publishes or removes the "Retry" hint from the footer. */
    void setRetryAvailable(bool available);
private:
    bool retry_available = false;
};
