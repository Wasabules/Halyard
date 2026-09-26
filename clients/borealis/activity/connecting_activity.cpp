#include "activity/connecting_activity.hpp"
#include "connecting_view.hpp"
#include "../ui/i18n.hpp"
#include "../ui/sfx.hpp"
#include "../ui/nav_guard.hpp"
#include "../ui/shutdown.hpp"   /* UX10 - announce the work a close must wait for */

extern "C" {
#include "core/services/errors.h"
}
#include "activity/shadow_app.hpp"
#include "../device_mode.hpp"
#include "../autotest.hpp"
#include "../demo.hpp"                /* DEMO-1 */

extern "C" {
#include "../../../core/common/log.h"
/* S81 — this module's log category. See shadow/journal.h: it is declared
 * here, never inferred from the text of the messages. */
#define calog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define cadbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)

}

#include <chrono>
#include <thread>
#include "activity/stream_activity.hpp"
#include "clients/borealis/activity/stream_view.hpp"
#include "settings.hpp"   /* B1 2026-05-18: quality params Q1 */

extern "C" {
#include "core/services/launcher.h"
#include "core/services/proximus.h"
#include "core/protocol/smoke_test.h"
#include "core/protocol/shadowusb.h"
#include "core/protocol/ctrl_session_glue.h"  /* the native Shadow protocol */
#include "core/protocol/ctrl_tcp.h"      /* S60: g_channels_down */
#include "core/protocol/ctrl_rest.h"          /* clean_my_zombies before native bootstrap */
}

#include <cstdlib>

#include "switch_compat.h"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <atomic>
#include <memory>

// === Step row builder ===
ConnectingActivity::ConnectingActivity() {
    calog("[NAV] connecting screen BUILT");
    alive = std::make_shared<std::atomic<bool>>(true);
    already_popped = std::make_shared<std::atomic<bool>>(false);
    steps = std::make_shared<std::vector<app::Step>>();
}

ConnectingActivity::~ConnectingActivity() {
    calog("[NAV] connecting screen DESTROYED");
    if (alive) alive->store(false);
    // Forces the session threads to break out of their loops.
    ShadowApp::instance().signalAbort();
}

/* Static helper: touches the model only, and only from the main thread. The
 * view re-reads it every frame, so there is nothing left to synchronize on the
 * display side. */
static void setStepUI(std::shared_ptr<std::atomic<bool>> alive_flag,
                       app::StepsPtr steps,
                       int idx, int state_i, std::string detail) {
    if (!alive_flag || !alive_flag->load()) return;
    brls::Threading::sync([alive_flag, steps, idx, state_i, detail]() {
        if (!alive_flag || !alive_flag->load()) return;
        if (!steps || idx < 0 || idx >= (int)steps->size()) return;
        (*steps)[idx].state  = (app::StepState)state_i;
        (*steps)[idx].detail = detail;
    });
}

/* S88 — the failure sound is played HERE, at the SINGLE point every error on
 * this screen passes through. Putting it on each call site would miss one —
 * there are nine — and a screen that chimes eight times out of nine is worse
 * than a silent one: you stop trusting the sound. */
static void showErrorUI(std::shared_ptr<std::atomic<bool>> alive_flag,
                         ConnectingView *view, std::string msg,
                         ConnectingActivity *self) {
    if (!alive_flag || !alive_flag->load()) return;
    brls::Threading::sync([alive_flag, view, msg, self]() {
        if (!alive_flag || !alive_flag->load()) return;
        if (view) view->setMessage(msg, true);
        ui::sfx::play(ui::sfx::Sound::Failed);
        /* "Retry" only makes sense once the connection has failed: the hint used
         * to show as soon as the screen opened, which suggested there was
         * already something to resume. */
        if (self) self->setRetryAvailable(true);
    });
}

void ConnectingActivity::buildSteps() {
    /* The labels used to describe the internal calls ("Credentials Proximus",
     * "Handshake WebRTC"): jargon for whoever is looking at the screen, and the
     * last one had been lying ever since WebRTC was abandoned. We describe what
     * is happening instead. */
    static const char *keys[] = {
        "connect/step1", "connect/step2", "connect/step3", "connect/step4",
        "connect/step5", "connect/step6", "connect/step7",
    };
    const size_t n = sizeof(keys) / sizeof(keys[0]);

    steps->clear();
    steps->reserve(n);
    for (size_t i = 0; i < n; i++) {
        app::Step st;
        st.title = ui::tr(keys[i]);
        steps->push_back(std::move(st));
    }

    /* S67 — nothing left to populate: `ConnectingView` READS this vector every
     * frame. The model and the display can therefore no longer diverge, and no
     * view is created or destroyed by a step changing state. */
}

/* === S82 2026-08-29 - FROM A CODE TO A SENTENCE ===
 *
 * The screen said "The connection failed. See the log for details." and, in
 * small type, `HTTP 409`. Together they tell someone who just wants to play
 * nothing at all: "see the log" demands a computer and knowing where to look,
 * and `409` has a precise cause and a precise answer that nothing was showing.
 *
 * `shadow/errors.c` does the translation - a pure function, verified offline.
 * Here we only call it and join the two sentences: what happened, then what can
 * be done about it. The action is optional, and its absence is a result:
 * offering a useless action spends the trust we will need next time.
 *
 * The CODE stays on screen next to the step, in small type. It is what you ask
 * someone to read out to you when helping them remotely. */
static std::string explain(error_step_t step, long http,
                           error_net_t reseau = ERR_NET_NONE)
{
    const error_explanation_t x = error_explain(step, http, reseau);
    std::string phrase = ui::tr(x.key);
    if (x.action_key) phrase += "  " + ui::tr(x.action_key);
    return phrase;
}

/* The technical detail of the failing step. Short, bounded, and it carries the
 * number: that is the only part that helps a remote diagnosis. */
static std::string detail(long http, error_net_t reseau = ERR_NET_NONE)
{
    char buf[48];
    return error_detail(buf, sizeof buf, http, reseau);
}

void ConnectingActivity::showError(const std::string &msg) {
    showErrorUI(alive, view, msg, this);
}

/* Publishes or withdraws the "Retry" hint. registerAction replaces the existing
 * action bound to the same button: re-registering it hidden is enough to remove
 * the hint while keeping the gesture live. */
void ConnectingActivity::setRetryAvailable(bool available) {
    this->registerAction(
        available ? ui::tr("action/retry") : "",
        brls::ControllerButton::BUTTON_A,
        [this](brls::View *) {
            if (!this->retry_available) return false;
            ShadowApp::instance().resetAbort();
            for (auto &st : *this->steps) {
                st.state  = app::StepState::Pending;
                st.detail.clear();
            }
            if (this->view) this->view->setMessage("", false);
            this->setRetryAvailable(false);
            this->runConnectionFlow();
            return true;
        },
        /*hidden=*/!available);
    this->retry_available = available;
}

// When StreamActivity is popped, ConnectingActivity becomes top again.
// Borealis does not auto-restore focus -> button B is inert -> the user is
// stuck. So we explicitly give focus back to our content view.
void ConnectingActivity::onResume() {
    calog("[NAV] connecting screen visible again");
    brls::View *content = this->getContentView();
    if (content) {
        brls::Application::giveFocus(content);
    }
}

brls::View *ConnectingActivity::createContentView()
{
    /* S67 — the view receives the MODEL, not the activity: it therefore holds
     * no reference to anything that could disappear before it does. */
    view = new ConnectingView(steps);
    return view;
}

void ConnectingActivity::onContentAvailable() {
    ShadowApp &app = ShadowApp::instance();
    if (view) view->setTitle(ui::tr("connect/title"),
                           app.selected_vm_alias.empty() ? "VM"
                                                         : app.selected_vm_alias);

    // B = cancel / back to VmList.
    this->registerAction(
        ui::tr("action/back"),
        brls::ControllerButton::BUTTON_B,
        [popped = already_popped](brls::View *) {
            ShadowApp::instance().signalAbort();
            /* S61 — see the header: the worker thread pops this screen too.
             * Whichever gets there first wins, the second does nothing. */
            if (!popped->exchange(true)) {
                calog("[NAV] popping the connecting screen from button B");
                ui::nav::pop();
            } else {
                calog("[NAV] button B ignored - the connecting screen is already popped");
            }
            return true;
        });

    /* "Retry" stays hidden as long as nothing has failed. */
    setRetryAvailable(false);

    buildSteps();

    runConnectionFlow();
}

/* === S51 2026-08-26 — DELETE OUR CLIENT REGISTRATION AT END OF SESSION ===
 *
 * Capture of the official client, file-transfer scenario, on close:
 *
 *     DELETE /3/clients/378008-...-main HTTP/1.1
 *
 * Campaign K14 (2026-08-21) had already spotted this behaviour, written
 * `proximus_delete_client()` and stated the hypothesis in black and white -
 * "after dozens of sessions the VM can accumulate ghost clients; a newcomer
 * facing already-registered clients would be treated as secondary".
 * It wired that call into the HEADLESS test binary and never into the GUI: so
 * never into the path that runs on the console, nor into the one we measure.
 * The hypothesis had been waiting five days to be tested where it counts.
 *
 * It is the best candidate for the audio channel that starts one session in
 * three: the server GRANTS us the channel then declares it dead 95 ms later,
 * while our bootstrap is byte-for-byte identical to the official one
 * (KB §3.35).
 *
 * A failure here is harmless: we are closing anyway. `SHADOW_DELETE_CLIENT=0`
 * restores the previous behaviour. */
/* === UX10 2026-09-10 - THIS THREAD IS WHAT A CLOSE WAITS FOR ===
 *
 * A guard rather than two calls: this thread has a dozen exits (early returns,
 * aborts, errors), and an `endTask` forgotten on one of them would hold the
 * closing screen until its ceiling - a defect that would only show the day that
 * particular path is taken. The destructor covers them all. */
struct ShutdownTask {
    ShutdownTask()  { ui::shutdown::beginTask(); }
    ~ShutdownTask() { ui::shutdown::endTask(); }
};

static void delete_our_clients(VmConnectionInfo *conn,
                               ProximusCredentials *creds,
                               ProximusMainSession *msess,
                               ProximusLauncherSession *lsess)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = std::getenv("SHADOW_DELETE_CLIENT");
        enabled = e ? std::atoi(e) : 1;
    }
    if (!enabled || !conn || !conn->proximus_url || !creds) return;
    /* UX10 - the longest step of a close (measured: ~2 s of ~3 s in total),
     * and the only one that depends on the network. It is the one worth
     * naming on screen. */
    ui::shutdown::setStep(ui::tr("shutdown/session").c_str());
    long st = 0;
    if (msess && msess->id && creds->main_jwt
        && proximus_delete_client(conn->proximus_url, creds->main_jwt, msess->id, &st))
        calog("[S51] client main supprime (HTTP %ld)", st);
    else if (msess && msess->id)
        calog("[S51] deleting the main client failed (HTTP %ld)", st);
    if (lsess && lsess->id && creds->launcher_jwt
        && proximus_delete_client(conn->proximus_url, creds->launcher_jwt, lsess->id, &st))
        calog("[S51] client launcher supprime (HTTP %ld)", st);
}

void ConnectingActivity::runConnectionFlow() {
    auto alive_flag = alive;
    auto popped_flag = already_popped;   /* S61 — see the header */
    auto steps_local = steps;       // shared_ptr copy — outlives a dead `this`
    ConnectingView *err_view = view;   /* valid for as long as `alive` is */
    /* Same rule as `err_view`: a raw pointer, but we only touch it from the
     * main thread, guarded by `alive_flag`. */
    ConnectingActivity *self = this;
    brls::Threading::async([alive_flag, popped_flag, steps_local, err_view, self]() {
        /* UX10: while this thread lives, a close has something to wait for. */
        ShutdownTask shutdown_task;
        ShadowApp &app = ShadowApp::instance();
        const char *vm_id     = app.selected_vm_id.c_str();
        const char *launcher  = app.launcher_url.c_str();
        const char *token     = app.access_token.c_str();
        auto abort_sp = app.abort_flag;

        /* === UX11 2026-09-10 - ONE ABANDON TEST, READ AT EVERY STEP ===
         *
         * Steps 1 to 6 used to test `alive_flag` only. That flag drops in
         * ~ConnectingActivity, which runs after `quit()` - and since UX10,
         * `quit()` waits for THIS thread (`shutdown_task` above). Closing the
         * window during "Connexion en cours" therefore went round in a circle.
         * Measured 2026-09-10: the thread ignored the close for 6.3 s, finished
         * the REST bootstrap, opened both SSE, and left WITHOUT deleting its two
         * clients on the server (no [S51] line). The close raises `abort_sp`;
         * reading it here is what breaks the circle.
         *
         * Safe to read this early: `onVmClicked` resets the flag before every
         * connection, and it is the only place that builds this activity - the
         * auto-connect goes through it too. A fresh connection cannot abandon on
         * a stale value. */
        auto abandon = [alive_flag, abort_sp]() {
            return !alive_flag->load() || (abort_sp && *abort_sp);
        };

        // Local lambdas: everything goes through an alive_flag check plus a
        // main-thread sync. No access to `this` from this thread, so no UAF is
        // possible if the activity is popped.
        auto setStep = [alive_flag, steps_local](int idx, app::StepState st,
                                                   const std::string &sub = "") {
            setStepUI(alive_flag, steps_local, idx, (int)st, sub);
        };
        auto showError = [alive_flag, err_view, self](const std::string &msg) {
            showErrorUI(alive_flag, err_view, msg, self);
        };

        /* === DEMO-1 - the seven steps, timed like a real connection, then the
         * demo stream. No request leaves; the addresses are documentation ones
         * (RFC 5737). The exits mirror the real path below: popping the stream
         * view, then the connecting screen on a deliberate exit (S40, S61). */
        if (demo::enabled()) {
            const char *details[7] = {"", "203.0.113.24:10011", "", "", "", "", ""};
            const int   wait_ms[7] = {500, 900, 400, 600, 700, 500, 0};
            for (int i = 0; i < 7 && !abandon(); i++) {
                setStep(i, app::StepState::Running);
                for (int t = 0; t < wait_ms[i] / 100 && !abandon(); t++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (i < 5) setStep(i, app::StepState::Done, details[i]);
                if (i == 5) setStep(5, app::StepState::Done, ui::tr("connect/st_streamer_up"));
            }
            if (abandon()) return;
            setStep(6, app::StepState::Running, ui::tr("connect/st_running"));
            /* SHADOW_DEMO_HOLD_CONNECTING=1 stops here, for a capture of the steps. */
            const char *hold = getenv("SHADOW_DEMO_HOLD_CONNECTING");
            if (hold && atoi(hold) != 0) {
                while (!abandon()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return;
            }
            brls::Threading::sync([]() { ui::nav::push(new StreamActivity()); });
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            demo::runStream(abandon);
            brls::Threading::sync([]() { ui::nav::pop(); });
            if (!alive_flag->load()) return;
            brls::Threading::sync([popped = popped_flag]() {
                if (!popped->exchange(true)) ui::nav::pop();
            });
            return;
        }

        // Pre-step: GET capabilities + GET turn-servers (non-fatal). The official
        // clients call these before /vm/start - possibly a "modern client"
        // signal for the server-side QoS scoring - so the requests stay, even
        // though nothing here uses a TURN relay: removing them would change the
        // sequence the server sees on a path that works.
        {
            VmCapabilities caps = {};
            long stcaps = 0;
            if (launcher_get_capabilities(launcher, token, vm_id, &caps, &stcaps)) {
                brls::Logger::info("capabilities: video={} fps_max={} {}x{} codecs={} | audio={} codecs={}",
                    caps.video_allowed ? "yes" : "no",
                    caps.max_frame_rate, caps.max_width, caps.max_height,
                    caps.video_codecs ? caps.video_codecs : "?",
                    caps.audio_allowed ? "yes" : "no",
                    caps.audio_codecs ? caps.audio_codecs : "?");
            } else {
                brls::Logger::info("capabilities: HTTP {} (non-fatal, continuing)", stcaps);
            }
            vmcaps_free(&caps);

            TurnServers ts = {};
            long stts = 0;
            if (launcher_get_turn_servers(launcher, token, vm_id, &ts, &stts)) {
                brls::Logger::info("turn-servers: {} relay(s) advertised", ts.count);
                for (size_t i = 0; i < ts.count; i++) {
                    brls::Logger::info("  [{}] {}://{}:{} (transport={})",
                        i,
                        ts.items[i].url ? ts.items[i].url : "?",
                        ts.items[i].host ? ts.items[i].host : "?",
                        ts.items[i].port,
                        ts.items[i].transport ? ts.items[i].transport : "udp");
                }
            } else {
                brls::Logger::info("turn-servers: HTTP {} (non-fatal, continuing)", stts);
            }
            turn_servers_free(&ts);
        }

        // Step 1: start VM
        /* UX11 - a close requested during the capability probe must not go on
         * to START the VM: that request is the one with a lasting effect. */
        if (abandon()) return;
        setStep(0, app::StepState::Running);
        long http = 0;
        bool ok = launcher_start_vm(launcher, token, vm_id, &http);
        if (abandon()) return;
        if (!ok) {
            setStep(0, app::StepState::Error, detail(http));
            showError(explain(ERR_STEP_START, http));
            return;
        }
        setStep(0, app::StepState::Done);

        // Step 2: poll vm/ip
        setStep(1, app::StepState::Running);
        VmConnectionInfo conn = {0};
        bool got_conn = false;
        long last_http = 0;
        for (int attempt = 0; attempt < 30 && !abandon(); attempt++) {
            long stip = 0;
            if (launcher_get_vm_ip(launcher, token, vm_id, &conn, &stip)
                && conn.ip && conn.port) {
                got_conn = true;
                break;
            }
            last_http = stip;
            const std::string hint = (stip == 470) ? ui::tr("connect/vm_starting") : "";
            setStep(1, app::StepState::Running,
                    std::to_string(attempt + 1) + "/30  " + hint);
            vmconn_free(&conn);
            for (int s2 = 0; s2 < 20 && !abandon(); s2++) {
                svcSleepThread(100000000ULL);  // 100 ms grain
            }
        }
        if (abandon()) { vmconn_free(&conn); return; }
        if (!got_conn) {
            setStep(1, app::StepState::Error, detail(last_http));
            showError(explain(ERR_STEP_ADDRESS,
                                last_http ? last_http : 470));
            vmconn_free(&conn);
            return;
        }
        setStep(1, app::StepState::Done, std::string(conn.ip) + ":" + conn.port);

        // Step 3: auth_login
        setStep(2, app::StepState::Running);
        LauncherSessionToken tok = {0};
        long stlogin = 0;
        if (!launcher_auth_login(launcher, token, vm_id, &tok, &stlogin)) {
            setStep(2, app::StepState::Error, detail(stlogin));
            showError(explain(ERR_STEP_ACCOUNT, stlogin));
            launcher_token_free(&tok);
            vmconn_free(&conn);
            return;
        }
        setStep(2, app::StepState::Done);
        if (abandon()) {
            launcher_token_free(&tok); vmconn_free(&conn); return;
        }

        // Step 4: proximus credentials
        setStep(3, app::StepState::Running);
        ProximusCredentials creds = {0};
        long stcred = 0;
        if (!launcher_proximus_credentials(launcher, token, vm_id, &creds, &stcred)) {
            setStep(3, app::StepState::Error, detail(stcred));
            showError(explain(ERR_STEP_PERMISSIONS, stcred));
            launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }
        if (!conn.proximus_url) {
            setStep(3, app::StepState::Error, ui::tr("connect/st_url_missing"));
            showError(ui::tr("connect/err_incomplete"));
            proximus_credentials_free(&creds);
            launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }

        /* Override the `/N/` segment of the URLs with the main_jwt's
         * JWT.instance. RE 2026-05-09 (LD_PRELOAD on the official desktop app):
         * the server returns `proximus_url=".../<slot>"` (sometimes a stale
         * sticky value), BUT the desktop app always uses `/<jwt.instance>/`. A
         * mismatch makes :13011 time out. JWT.instance lives in creds.main_jwt,
         * not in the global OAuth token from launcher_get_vm_ip. */
        if (creds.main_jwt) {
            int inst = launcher_jwt_instance(creds.main_jwt);
            fprintf(stderr, "[connecting] JWT.instance=%d (server slot=%d) "
                             "proximus_url BEFORE = %s\n", inst, conn.slot_number,
                    conn.proximus_url ? conn.proximus_url : "(null)");
            if (inst > 0) {
                launcher_rewrite_url_instance(&conn.proximus_url, inst);
                launcher_rewrite_url_instance(&conn.messaging_url, inst);
                fprintf(stderr, "[connecting] rewrote URLs to /%d/ → "
                                 "proximus_url AFTER = %s\n", inst,
                        conn.proximus_url ? conn.proximus_url : "(null)");
            }
        }

        setStep(3, app::StepState::Done);

        /* Clean up zombies from previous sessions BEFORE creating
         * our new clients. Cleaning up AFTER create deletes the clients we have
         * just created -> JWTs invalidated -> SSE answers 401 (= the bug
         * observed 2026-05-09 ~01h30). */
        {
            if (creds.main_jwt && conn.ip) {
                int inst = launcher_jwt_instance(creds.main_jwt);
                const char *force_clear = std::getenv("SHADOW_NATIVE_FORCE_CLEAR");
                bool do_force = force_clear && force_clear[0] == '1';
                if (inst > 0) {
                    int n_del = -1;
                    if (do_force) {
                        n_del = ctrl_rest_clean_all_clients(conn.ip, creds.main_jwt,
                                                              inst);
                        fprintf(stderr, "[connecting] PRE-create FORCE_CLEAR: "
                                         "%d clients deleted\n", n_del);
                    } else {
                        const char *du = shadow_device_uuid_public();
                        if (du) {
                            n_del = ctrl_rest_clean_my_zombies(conn.ip,
                                                                  creds.main_jwt,
                                                                  inst, du);
                            fprintf(stderr, "[connecting] PRE-create cleanup: "
                                             "%d zombies deleted (device-id=%s)\n",
                                    n_del, du);
                        }
                    }
                }
            }
        }

        /* UX11 - the last exit BEFORE anything exists on the server. Past this
         * line an abandon has two clients to delete. */
        if (abandon()) {
            proximus_credentials_free(&creds); launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }

        // Step 5: open launcher + main client sessions
        setStep(4, app::StepState::Running);
        ProximusLauncherSession lsess = {0};
        long stl = 0;
        if (!proximus_create_launcher_client(conn.proximus_url, creds.launcher_jwt, &lsess, &stl)) {
            setStep(4, app::StepState::Error, "launcher " + detail(stl));
            showError(explain(ERR_STEP_CLIENT, stl));
            proximus_credentials_free(&creds);
            launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }
        ProximusMainSession msess = {0};
        long stm = 0;
        if (!proximus_create_main_client(conn.proximus_url, creds.main_jwt, &msess, &stm)) {
            setStep(4, app::StepState::Error, "main " + detail(stm));
            showError(explain(ERR_STEP_CLIENT, stm));
            proximus_launcher_session_free(&lsess); proximus_credentials_free(&creds);
            launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }
        setStep(4, app::StepState::Done);

        /* UX11 - from here our two clients exist on the server, so every
         * abandon deletes them. The step-6 exit below used to free the memory
         * and return without the DELETE - a leak older than UX10, reached by B
         * pressed during step 6 as much as by a close. The server swept the
         * ghosts at the NEXT connection, which is why it never showed. */
        auto abandonWithClients = [&]() {
            delete_our_clients(&conn, &creds, &msess, &lsess);
            proximus_main_session_free(&msess); proximus_launcher_session_free(&lsess);
            proximus_credentials_free(&creds); launcher_token_free(&tok); vmconn_free(&conn);
        };

        // M32 binary protobuf disabled: 10 paths probed, all 404/405/400.
        // The binary CtrlChanV2 does NOT exist on this VM (dead end).
        // streaming_smoke_test_m32(conn.ip, msess.streaming_token, msess.id, creds.main_jwt);

        // === SHADOWUSB FLOW: create usb client -> register device ===
        // 1. POST /clients type=usb -> registers client.id
        // 2. POST /<instance>/devices -> returns {id, port} for the usbredir
        //    tunnel
        if (creds.usb_jwt) {
            ProximusUsbSession usb_sess = {0};
            long st_usb = 0;
            if (proximus_create_usb_client(conn.proximus_url, creds.usb_jwt,
                                             &usb_sess, &st_usb)) {
                brls::Logger::info("shadowusb: USB client created id={}", usb_sess.id);
                int inst = jwt_instance(creds.main_jwt);
                if (inst < 1) inst = 3;
                /* Full probe: 3 client-types x 2 paths, to identify the
                 * (id, jwt, path) combination that unlocks spice_url. */
                shadowusb_probe_remote_consoles(conn.ip, inst,
                    lsess.id, creds.launcher_jwt,
                    msess.id, creds.main_jwt,
                    usb_sess.id, creds.usb_jwt);
                proximus_usb_session_free(&usb_sess);
            } else {
                brls::Logger::error("shadowusb: create USB client FAIL HTTP {}", st_usb);
            }
        } else {
            brls::Logger::info("shadowusb: usb_jwt non disponible (compte legacy ?), skip");
        }
        // =========================================================

        // Step 6: status streamer
        setStep(5, app::StepState::Running);
        ProximusStatus pst = {0};
        long sts = 0;
        bool stat_ok = proximus_get_status(conn.proximus_url, creds.launcher_jwt, &pst, &sts);
        if (!abandon()) {
            if (!stat_ok) {
                setStep(5, app::StepState::Error, "HTTP " + std::to_string(sts));
                // Non-fatal: carry on anyway
            } else if (!pst.streamer_up) {
                setStep(5, app::StepState::Done, ui::tr("connect/st_streamer_not"));
            } else {
                setStep(5, app::StepState::Done, ui::tr("connect/st_streamer_up"));
            }
        }
        proximus_status_free(&pst);

        if (abandon()) { abandonWithClients(); return; }

        // Step 7: the session (long-running - StreamActivity is pushed in
        // parallel so that stream_view_set_active is in place when it starts).
        setStep(6, app::StepState::Running, ui::tr("connect/st_running"));

        /* === S88 — THE REWARD, THEN SILENCE ===
         *
         * The session-established sound plays BEFORE moving to the stream
         * screen: the console has a single audio output, and it is the stream
         * that takes it. `release()` right after lets the output close as soon
         * as the sound has ended, instead of two seconds later.
         *
         * AUDC-1 / OUT-2 2026-09-11 - this comment used to promise that the
         * 500 ms wait below "leaves it fifty blocks". It never did: `release()`
         * CUT every voice, microseconds after this `play()` - Threading::sync
         * only queues the push in between - so 0 of the chime's 34 464 frames
         * ever reached the output, on Switch and on Linux, since S88 itself.
         * `release()` no longer cuts: the whole chime plays (718 ms - the
         * shipped file, not the 420 ms docs/UI_SOUNDS.md specified), and
         * the handoff to the stream is the StreamClaim around
         * ctrl_session_glue_run below, which waits for the UI-sound thread to
         * have closed the output; a second passes in between. Bench: 34 080 /
         * 34 464 frames, bit-exact, under both models of HOS. Moving release()
         * after the 500 ms sleep instead (OUT-2's first proposal) played ~49 of
         * 72 blocks: the file is longer than the wait. */
        /* UX11 - no stream screen, no "connected" chime and no SSE for a
         * session nobody wants any more. During a close the push would be
         * ignored anyway (UX10, nav_guard), and the whole native bootstrap would
         * have run for nothing - which is exactly what the 2026-09-10 trace
         * showed: `[nav] empilement ignore` three seconds after the close. */
        if (abandon()) { abandonWithClients(); return; }
        ui::sfx::play(ui::sfx::Sound::Connected);

        // Move on to StreamActivity: this installs the active StreamView.
        brls::Threading::sync([]() {
            ui::nav::push(new StreamActivity());
        });
        ui::sfx::release();
        svcSleepThread(500000000ULL);  // 500 ms — lets Borealis create the view

        // === The native Shadow protocol - the only path since 2026-09-26 ===
        // (It used to be chosen against a libdatachannel/WebRTC path through
        // SHADOW_NATIVE; that path and the choice are gone.)
        {
            setStep(6, app::StepState::Running, ui::tr("connect/st_running"));

            /* Step 1: (the zombie cleanup already happened BEFORE
             * proximus_create_*, so as not to invalidate the JWTs of our new
             * session). */

            /* Step 2: compute port_base from vm.port (= the /vm/ip response).
             * Formula observed 2026-05-09: port_base = vm.port + 7000 (validated
             * on 2 VMs: revere-cool 3000 -> 10000, self-belt 2000 -> 9000). */
            int port_base = 0;
            if (conn.port) {
                int vm_port = atoi(conn.port);
                if (vm_port > 0) port_base = vm_port + 7000;
            }
            fprintf(stderr, "[connecting] port_base computed = %d (from vm.port=%s + 7000)\n",
                    port_base, conn.port ? conn.port : "(null)");

            /* Step 3: DUAL SSE keepalive — critical for the server-side bind.
             * The server waits for both SSE to be open AND stable BEFORE it
             * binds :port_base+11. 3 s leaves time for the zombie DELETE to
             * propagate server-side and for the binding to open. */
            proximus_sse_keepalive *nat_sse_l = nullptr;
            proximus_sse_keepalive *nat_sse_m = nullptr;
            /* D10 2026-08-21 — CORRECTION. This comment used to claim the
             * desktop client opens /N/status with main_jwt, "confirmed via an
             * LD_PRELOAD hook 2026-05-09". That is false: `/status` is a **JSON**
             * endpoint (cf. proximus.h:44, `{meta, data:{vm_status, reachable,
             * streamer_up}}`), not an SSE stream — `/stream` is the SSE one
             * (proximus.h:70). Holding it open like a stream makes no sense: it
             * answers 200 and ends immediately, the keepalive thread exited, and
             * the server closed the session ~118 s later (KB §3.17).
             * main_test.c was already opening /stream for both JWTs — which is
             * why the headless binary survived 5/5 and masked the bug.
             * The default is aligned on /stream; SHADOW_SSE_MAIN_SUFFIX=status
             * reproduces the old behaviour. */
            if (conn.proximus_url && creds.launcher_jwt)
                nat_sse_l = proximus_sse_start_ex(conn.proximus_url,
                                                    creds.launcher_jwt, "stream");
            /* D8 2026-08-21 — bisect of the GUI-only freeze (KB §3.17). The
             * only protocol divergence found between the GUI (dies at 118 s,
             * 4/4) and the headless binary (survives, 5/5): the 2nd SSE. The GUI
             * opens /status with main_jwt (matching the desktop client, hook
             * 2026-05-09) where main_test.c opens /stream for both. If /status
             * ends on its own and the server concludes the client is gone, that
             * would explain the teardown. SHADOW_SSE_MAIN_SUFFIX=stream aligns
             * the GUI on the headless binary to settle it. */
            const char *sse_main_suffix = getenv("SHADOW_SSE_MAIN_SUFFIX");
            if (!sse_main_suffix || !*sse_main_suffix) sse_main_suffix = "stream";
            if (conn.proximus_url && creds.main_jwt)
                nat_sse_m = proximus_sse_start_ex(conn.proximus_url,
                                                    creds.main_jwt, sse_main_suffix);
            svcSleepThread(500000000ULL);  /* 500 ms — the minimum for the SSE to be open */

            /* B1 2026-05-18: pass the quality params from Settings (= the UI). */
            auto& cfg = Settings::instance();
            ctrl_session_glue_params np = {};
            np.vm_host         = conn.ip;
            np.streaming_token = msess.streaming_token;
            np.client_id       = msess.id;
            np.bearer_jwt      = creds.main_jwt;
            np.display_width   = (int)(cfg.display_width  > 0 ? cfg.display_width  : 1920);
            np.display_height  = (int)(cfg.display_height > 0 ? cfg.display_height : 1080);
            np.port_base       = port_base;
            /* B1: resolved below by `effectiveBitrateMbps()`, which knows the
             * per-link rule. Left here so the field is never read uninitialised
             * if that block ever moves. */
            np.max_bitrate_mbps = cfg.max_bitrate_mbps;  /* 0 = desktop default */

            /* Gamepad trial: register the input channel as well. Pushed here
             * because registration happens when the sockets are opened. */
            ctrl_session_glue_set_udp_register_input(cfg.udp_register_input ? 1 : 0);

            /* === Resolution follows the console mode ===
             * The built-in screen is 1280x720. Asking for 1080p in handheld mode
             * encodes, transmits and decodes twice as many pixels as anyone will
             * ever see — and on the Switch each of those three stages is a
             * scarce resource. Docked, we render the chosen resolution. */
            if (cfg.auto_resolution) {
                const bool docked = device::isDocked();
                if (!docked && np.display_height > 720) {
                    np.display_width  = 1280;
                    np.display_height = 720;
                }
                fprintf(stderr, "[mode] %s -> %dx%d\n",
                        docked ? "dock" : "portable",
                        np.display_width, np.display_height);
            }

            /* === B1 - THE BITRATE IN FORCE, RESOLVED IN ONE PLACE ===
             * The rule (global vs per-link, and what an unanswered network
             * service means) lives in `streaming/bitrate.h` and is applied by
             * `Settings::effectiveBitrateMbps`. This block used to hold its own
             * copy of it, and the pause menu a third: the three could disagree,
             * and did.
             * `Unknown` still means "the service did not answer" and still
             * falls back on the global value - never on a per-link one, which
             * would throttle a wired link on the strength of a missing
             * answer. */
            np.max_bitrate_mbps = cfg.effectiveBitrateMbps();
            fprintf(stderr, "[lien] %s -> %u Mbps%s\n",
                    device::linkLabel(), np.max_bitrate_mbps,
                    np.max_bitrate_mbps ? "" : " (defaut client)");
            np.target_fps       = (float)cfg.target_fps; /* 0 = desktop default */
            /* B4 - video transport (UDP by default, TCP = the official
             * `reliability` profile). `SHADOW_VIDEO_NET_TCP` still wins:
             * ctrl_session resolves the two, once, for the whole session. */
            np.video_tcp        = cfg.video_tcp ? 1 : 0;
            np.abort_flag      = abort_sp.get();
            ctrl_session_glue_stats nstats = {};
            bool nat_ok = false;

            /* === AUTOMATED TEST LOOP ===
             * With no `autotest.txt` on the SD card, what follows runs exactly
             * one pass: that is the normal behaviour.
             *
             * With one, it chains N sessions. An INTERMITTENT defect cannot be
             * characterised in a single session - the cursor/audio channel
             * starts six times out of seven on desktop and never on console -
             * and until now every measurement cost a manual round trip. One
             * launch now yields N samples, logged as they happen to the
             * development machine. */
            const autotest::Plan &at = autotest::plan();
            for (int attempt = 1; attempt <= at.runs; attempt++) {
                if (!alive_flag->load() || (abort_sp && *abort_sp)) break;

                /* A flag OWNED by the session: the duration limiter must not
                 * write into the application's abort flag, otherwise a run that
                 * simply finishes would look like the user quitting and the loop
                 * would stop there. */
                /* === S40 2026-08-25 — DIVERT THE FLAG ONLY FOR A SERIES ===
                 *
                 * This substitution used to be UNCONDITIONAL. With no
                 * `autotest.txt`, `runs` is 1 and `duration` is 0: the session
                 * therefore watched `run_over`, which nothing ever raised - the
                 * timer that could raise it is only created when
                 * `duration > 0`. "Quit the stream" was therefore
                 * STRUCTURALLY unable to stop the session: the UI left the view,
                 * the stream kept running underneath, and you had to open
                 * another application to force it closed. A regression caused by
                 * the test loop itself.
                 *
                 * So in normal use we hand back the application's flag. In a
                 * series we keep `run_over` - so that a run reaching its
                 * duration does not look like a "quit" - but a watchdog thread
                 * COPIES the application flag into it, so that a real "quit"
                 * stops the series too. */
                volatile int run_over = 0;
                np.abort_flag = at.active ? &run_over : abort_sp.get();

                std::thread timer;
                if (at.active) {
                    timer = std::thread([&run_over, &at, alive_flag, abort_sp]() {
                        /* duration == 0 in a series = no time limit, but we
                         * still watch for the user's abort. */
                        const int ticks = (at.duration > 0) ? at.duration * 10 : -1;
                        for (int t = 0; ticks < 0 || t < ticks; t++) {
                            /* 100 ms granularity: KB rule §7.3 - every
                             * long-lived thread must see an abort at that rate,
                             * otherwise HOS leaks its handles and the console
                             * has to be rebooted. */
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            if (run_over) return;
                            if (!alive_flag->load() || (abort_sp && *abort_sp)) break;
                        }
                        run_over = 1;
                    });
                }

                /* === AUTOMATIC RECONNECTION (2026-08-27) ===
                 * The native path - the one the console runs - had NONE: only
                 * the WebRTC branch, abandoned and Linux-only, ever had one. Yet
                 * the video UDP sometimes dies mid-session with the server still
                 * alive (KB §3.34): the user was then left facing a dead screen
                 * with no explanation.
                 *
                 * We only resume on an ENDURED ending. A deliberate exit
                 * (`exit_reason == 3`) or a requested abort must obviously not
                 * restart - otherwise quitting the stream would reopen it. */
                {
                    const auto &reg = Settings::instance();
                    const int max_attempts = reg.auto_reconnect
                                         ? (int)reg.reconnect_max + 1 : 1;
                    for (int attempt = 1; attempt <= max_attempts; attempt++) {
                        /* UX11 (audit finding 11) - the pause below does stop
                         * on an abort, but the loop head then called
                         * `ctrl_session_glue_run` again with the flag already
                         * up: one more full bootstrap, up to 8 s against a
                         * server that accepts TLS and goes quiet. */
                        if (attempt > 1 && abandon()) break;
                        nstats = ctrl_session_glue_stats{};
                        {
                            /* === AUDC-1 / OUT-2 2026-09-11 - THE STREAM CLAIMS THE OUTPUT ===
                             * libnx has ONE IAudioOut per process, and the UI
                             * sounds used it with no handoff. Every retry
                             * below plays Failed (showError -> showErrorUI);
                             * the UI-sound thread then kept audout started
                             * until 2 s after the sound, while this loop waits
                             * 1 s and the glue opens the stream's output first
                             * thing. The retried session was silent: HOS
                             * refuses the second Start (no decoder for the
                             * whole session) or accepts it, and the UI
                             * thread's idle close stops the shared output a
                             * second in. Default settings, every automatic
                             * reconnection - a first attempt that fails at
                             * bootstrap included.
                             * The claim makes the UI-sound thread close its
                             * output and acknowledge (<= 100 ms, 10 ms slices,
                             * median 10 ms) BEFORE the session opens audout,
                             * keeps the UI sounds silent for the whole session
                             * - the wake-up lock over a live stream included
                             * (AF2) - and gives the output back when it goes
                             * out of scope: before showError() below, and on
                             * every path out. Bench (the real sfx.cpp and
                             * audio.c built for __SWITCH__ against a mock of
                             * the one IAudioOut): reconnected session 0.0 %
                             * heard (Start refused) or 3-33 % (accepted) at
                             * X <= 0.9 s before, 100 % bit-exact at every X
                             * after, six materials including FLAC.
                             * SHADOW_SFX_HANDOFF=0 makes it a no-op. */
                            ui::sfx::StreamClaim claim;
                            nat_ok = ctrl_session_glue_run(&np, &nstats);
                        }

                        const bool wanted  = (nstats.exit_reason == 3);
                        const bool asked   = (abort_sp && *abort_sp) || !alive_flag->load();
                        if (nat_ok || wanted || asked) break;
                        if (attempt >= max_attempts) break;

                        calog("[reco] stream lost (exit_reason=%d) - attempt %d/%d",
                                   nstats.exit_reason, attempt, max_attempts - 1);
                        showError(ui::tr("connect/reconnecting", attempt,
                                         max_attempts - 1));
                        /* One second between attempts: enough for the server to
                         * finish closing its side, not enough to make the
                         * application look frozen. */
                        for (int k = 0; k < 10 && alive_flag->load()
                                        && !(abort_sp && *abort_sp); k++)
                            svcSleepThread(100000000ULL);
                    }
                }
                run_over = 1;
                if (timer.joinable()) timer.join();

                if (at.active) {
                    /* One line per run, machine-readable: this is the line we
                     * will count to characterise an intermittent defect. */
                    /* `journal_uncategorised` and NOT `brls::Logger`: only the former goes
                     * through the network mirror. Found on the first real
                     * series - six chained sessions, zero summaries received,
                     * because they went into Borealis's own log. */
                    /* S60: the mask of the channels the SERVER declared dead.
                     * That is the oracle: it is known ~1 s into the session,
                     * where it used to take 15 s of silence to guess. Without it
                     * on this line, nobody read it. */
                    char dead[80] = "-";
                    {
                        static const char *N[8] = {"VIDEO","AUDIO","INPUT","CURSOR",
                                                   "MICRO","CONTROLLER","CLIPBOARD","FILETRANSFER"};
                        int o = 0; dead[0] = 0;
                        for (int b = 0; b < 8; b++)
                            if (g_channels_down & (1u << b))
                                o += snprintf(dead + o, sizeof(dead) - o,
                                              "%s%s", o ? "," : "", N[b]);
                        if (!o) snprintf(dead, sizeof(dead), "-");
                    }
                    calog("[AUTOTEST] run=%d/%d ok=%d sec=%d frames=%u "
                               "cursor=%u audio=%llu exit=%d dead=%s dropped=%u",
                               attempt, at.runs, nat_ok ? 1 : 0,
                               nstats.session_seconds, nstats.frames_displayed,
                               nstats.udp_cursor_pkts,
                               (unsigned long long)nstats.udp_audio_bytes,
                               nstats.exit_reason, dead, nstats.dec_dropped);
                }
                if (attempt < at.runs && at.pause > 0) {
                    for (int t = 0; t < at.pause * 10; t++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        if (!alive_flag->load() || (abort_sp && *abort_sp)) break;
                    }
                }
            }
            np.abort_flag = abort_sp.get();   /* hand back the original flag */
            /* AF12 2026-09-10 - consumed only when the series went to the END.
             * It was erased after ANY exit of the loop, aborts included: the
             * next launch then ran one plain session, and the remote `quit` of
             * the S45 cycle erased the plan on every iteration. */
            if (at.active && !abandon()) autotest::consumePlan();

            if (nat_sse_m) proximus_sse_stop(nat_sse_m);
            if (nat_sse_l) proximus_sse_stop(nat_sse_l);
            brls::Threading::sync([]() {
                calog("[NAV] popping the stream view from the worker thread");
                ui::nav::pop();
            });
            if (!alive_flag->load()) {
                delete_our_clients(&conn, &creds, &msess, &lsess);
                proximus_main_session_free(&msess); proximus_launcher_session_free(&lsess);
                proximus_credentials_free(&creds); launcher_token_free(&tok); vmconn_free(&conn);
                return;
            }
            /* === S40 2026-08-25 - A DELIBERATE EXIT GOES BACK TO THE VM LIST ===
             *
             * When the user quit the stream, we removed the stream view and
             * left them on the connection screen, its seven steps still shown
             * and a "done" message: a dead end, with no way back. Reported
             * symptom: "stuck on Connecting, I have to force-quit". A DELIBERATE exit (`exit_reason == 3`) is not a result
             * worth showing - we hand control back to the VM list. The endings
             * that were SUFFERED (a server cut, a bootstrap failure) stay on
             * screen: there, the user needs to know what happened. */
            /* UX11 - an abort requested while this screen is still ALIVE is a
             * deliberate exit, whatever the session had time to report. Before,
             * a close during the connection broke the loop with `nstats` still
             * empty (exit_reason 0): neither branch below ran, so the two
             * clients stayed on the server. Folding the abort into the FIRST
             * branch instead would have been wrong: that branch does not pop the
             * connecting screen, and a plain "quit the stream" would have left
             * the user stuck on it - the S40 dead end again. */
            if (abort_sp && *abort_sp) nstats.exit_reason = 3;
            calog("[NAV] native session ended, exit_reason=%d", nstats.exit_reason);
            if (nstats.exit_reason == 3) {
                delete_our_clients(&conn, &creds, &msess, &lsess);
                proximus_main_session_free(&msess); proximus_launcher_session_free(&lsess);
                proximus_credentials_free(&creds); launcher_token_free(&tok); vmconn_free(&conn);
                calog("[NAV] deliberate exit (exit_reason=3) - back to the list");
                brls::Threading::sync([popped = popped_flag]() {
                    /* S61 - same thing: if B already went through during the
                     * cleanup, this screen is no longer on the stack and popping
                     * would take the VM list with it. */
                    if (!popped->exchange(true)) {
                        calog("[NAV] popping the connecting screen from the worker thread");
                        ui::nav::pop();
                    } else {
                        calog("[NAV] worker thread: the connecting screen was already popped by B");
                    }
                });
                return;
            }
            if (nat_ok) {
                setStep(6, app::StepState::Done,
                        "native " + std::to_string(nstats.frames_displayed) + " frames");
                showError(ui::tr("connect/ended"));
            } else if (nstats.exit_reason == 2) {
                /* Review 2026-08-21: tell a server-side cut from a bootstrap
                 * failure - both used to show the same message. */
                setStep(6, app::StepState::Error, ui::tr("connect/st_closed"));
                showError(ui::tr("connect/closed"));
            } else {
                setStep(6, app::StepState::Error, ui::tr("connect/st_boot_failed"));
                showError(ui::tr("connect/failed"));
            }
            proximus_main_session_free(&msess); proximus_launcher_session_free(&lsess);
            proximus_credentials_free(&creds); launcher_token_free(&tok); vmconn_free(&conn);
            return;
        }
    });
}
