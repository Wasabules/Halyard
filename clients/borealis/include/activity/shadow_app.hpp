// The ShadowApp singleton - carries the state shared between activities.
//
// Successive activities:
//   BootActivity        : TINAG + OAuth refresh, writes launcher_url + access_token
//   VmListActivity      : shows the VM list (cards). The user picks -> ConnectingActivity
//   ConnectingActivity  : start VM -> poll IP -> auth_login -> proximus -> WebRTC
//   StreamActivity      : full-screen H.264 video + audio
//
// We avoid any heavy C++ <-> Borealis bridge: just std::string fields plus a
// simplified VmInfo for the list. The activities read and write this
// singleton.

#pragma once

#include <string>

extern "C" {
#include "../../../../core/services/config.h"
}
#include <cstdio>
#include <vector>
#include <memory>
#include <atomic>

extern "C" {
#include "core/services/launcher.h"   // VmInfo (POD struct)
}

struct ShadowVm {
    std::string id;
    std::string name;
    std::string alias;
    std::string state;       // "DESIRED_AVAILABLE", "ACTIVE", etc.
    std::string image_url;   // future: the VM's cover image
};

class ShadowApp {
public:
    // Auth + datacenter (rempli par BootActivity)
    std::string launcher_url;
    std::string dc_name;
    std::string access_token;
    std::string refresh_token;

    // The VM list (filled by VmListActivity on refresh)
    std::vector<ShadowVm> vms;

    // The VM being connected to (set by VmListActivity, read by ConnectingActivity)
    std::string selected_vm_id;
    std::string selected_vm_alias;

    // The shared abort_flag: when the app closes or when the user leaves the stream,
    // the running threads watch this flag and break out.
    std::shared_ptr<volatile int> abort_flag;

    static ShadowApp& instance() {
        static ShadowApp inst;
        return inst;
    }

    // Reset l'auth (sur logout / token expired)
    void clearAuth() {
        access_token.clear();
        refresh_token.clear();
        launcher_url.clear();
        dc_name.clear();
    }

    /* Sign out: erases the persisted token and the in-memory state, then quits.
     * On the next launch, startup goes through authentication again.
     *
     * This body used to be written in the machine list, with the token's path
     * hardcoded for each platform. It lives here since the gesture is offered in
     * two places: duplicating it would have guaranteed that one of the two
     * forgets to erase the file. */
    void logout() {
        std::string token = std::string(SHADOW_DATA_DIR) + "refresh_token";
        std::remove(token.c_str());
        clearAuth();
        vms.clear();
        brls::Application::quit();
    }

    /* === S110 2026-08-29 - REPLAY FROM THE START, WITHOUT CLOSING ===
     *
     * `logout()` QUITS. That is the right gesture for signing out, but it makes
     * it impossible to record the complete journey - the pairing code included -
     * since the capture stops with the window. Redoing the pairing required
     * relaunching the application, hence cutting the recording in two and
     * splicing it back together.
     *
     * So we go back to the boot screen IN PLACE. `forget_token` decides whether
     * we replay the authentication (the code to scan) or only the loading of the
     * machines.
     *
     * The running threads are warned BEFORE we pop: an activity destroyed while
     * a network thread is answering it is the family of crashes `alive_flag`
     * exists to prevent, and popping without signalling would go around it. */
    /* Returns the application to its first-launch state and EMPTIES the
     * activity stack. The caller then pushes the boot screen - this header
     * cannot know it without a circular dependency, and making it know it for a
     * single line would be paying dearly for a shortcut. */
    void prepareRestart(bool forget_token) {
        signalAbort();
        if (forget_token) {
            std::string token = std::string(SHADOW_DATA_DIR) + "refresh_token";
            std::remove(token.c_str());
            clearAuth();
        }
        vms.clear();

        /* We pop EVERYTHING, the root included: the boot screen rests on a fresh
         * state, and keeping one underneath would bring back a stale machine
         * list on the first B. */
        /* === AF6 2026-09-10 - POP WITHOUT THE FADE ===
         *
         * With the default FADE, `popActivity` takes an input-block token and
         * removes the activity only at the END of the transition. The next
         * call of this loop therefore finds the SAME activity, already hidden:
         * it destroys it mid-fade and hands back one token, while the first
         * call's callback - the one that would have returned the other - dies
         * with the view. Net +2 -1 per activity, so at least two tokens stayed
         * taken after "Rejouer", and Borealis processes input only at zero:
         * keyboard, mouse and gamepad dead on desktop, every button dead on
         * Switch (touch survived, being read straight in `draw()`).
         * Reconstructed from Borealis' code by the 2026-09-10 shutdown audit
         * (application.cpp popActivity, view.cpp hide callback), not yet seen
         * on screen. With NONE the callback runs at once: one call, one
         * activity gone, its token returned. */
        while (brls::Application::popActivity(brls::TransitionAnimation::NONE)) { }
        resetAbort();
    }

    // Resets the abort_flag (to be called before every connection / refresh / etc.)
    void resetAbort() {
        abort_flag = std::make_shared<volatile int>(0);
    }

    // Tells the threads to stop.
    void signalAbort() {
        if (abort_flag) *abort_flag = 1;
    }

private:
    ShadowApp() { resetAbort(); }
};
