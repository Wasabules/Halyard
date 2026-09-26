#include "activity/boot_activity.hpp"
#include "boot_view.hpp"
#include "../ui/i18n.hpp"
#include "activity/shadow_app.hpp"
#include "activity/vm_list_activity.hpp"
#include "../demo.hpp"                /* DEMO-1 - fictional data for screenshots */
#include "core/version.h"           /* S85 - build fingerprint shown on the error screen */
#include "core/common/log.h"        /* [NAV] traces */
/* S81 - this module's log category. See shadow/journal.h: it is DECLARED here,
 * never inferred from the text of the messages. */
#define balog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)
#define badbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_UI, __VA_ARGS__)


extern "C" {
#include "core/services/config.h"
#include "core/services/http.h"
#include "core/services/tinag.h"
#include "core/services/oauth.h"
#include "core/services/telemetry.h"
#include "core/services/qr_helper.h"
}

#include "switch_compat.h"
#ifdef __SWITCH__
#include <switch.h>
#endif   // svcSleepThread
#include <thread>
#include <atomic>
#include <ctime>

// Helper: update a UI label, guarded by the alive flag.
/* Both helpers write into the VIEW, on the main thread. They hold the `alive`
 * flag: the background thread can outlive the view, and writing into a
 * destroyed view is exactly the defect that cost a full day here. */
static void ui_set(BootView *view, std::shared_ptr<std::atomic<bool>> alive,
                   const std::string &s)
{
    if (!alive || !alive->load()) return;
    brls::Threading::sync([view, alive, s]() {
        if (alive->load() && view) view->setStatus(s);
    });
}

/* UX8 2026-09-10 - the SINGLE point every failure of this screen goes through,
 * which is why the way out is published here and not at the six call sites: one
 * forgotten site would be a dead end again, and a screen that offers a way out
 * five times out of six is worse than one that never does — you stop looking
 * for it. Same reasoning as the failure sound in `connecting_activity.cpp`. */
static void ui_error(BootView *view, std::shared_ptr<std::atomic<bool>> alive,
                     const std::string &s, BootActivity *self)
{
    if (!alive || !alive->load()) return;
    brls::Threading::sync([view, alive, s, self]() {
        if (!alive->load()) return;
        if (view) view->setError(s);
        if (self) self->setRetryAvailable(true);
    });
}

BootActivity::BootActivity() {
    alive = std::make_shared<std::atomic<bool>>(true);
}

BootActivity::~BootActivity() {
    if (alive) alive->store(false);
}

brls::View *BootActivity::createContentView()
{
    view = new BootView();
    return view;
}

void BootActivity::setRetryAvailable(bool available) {
    this->registerAction(
        available ? ui::tr("action/retry") : "",
        brls::ControllerButton::BUTTON_A,
        [this](brls::View *) {
            if (!this->retry_available) return false;
            this->setRetryAvailable(false);
            /* `setStatus` clears the error by itself, and the flow calls it
             * before anything else — no need to blank the view here. */
            this->startBootFlow();
            return true;
        },
        /*hidden=*/!available);
    this->retry_available = available;
}

void BootActivity::onContentAvailable() {
    startBootFlow();
}

void BootActivity::startBootFlow() {
    auto alive_flag = alive;
    BootView *v = view;
    /* Captured as a raw pointer, guarded by `alive_flag` like everything else
     * this thread touches — the activity can be destroyed while it runs. */
    BootActivity *self = this;

    brls::Threading::async([alive_flag, v, self]() {
        /* DEMO-1 - the same status lines, no request, no token read or saved. */
        if (demo::enabled()) {
            auto pause_ms = [&alive_flag](int ms) {
                for (int t = 0; t < ms / 100 && alive_flag->load(); t++)
                    svcSleepThread(100000000ULL);
            };
            for (const char *key : {"boot/net_init", "boot/net_check", "boot/datacenter", "boot/auth"}) {
                ui_set(v, alive_flag, ui::tr(key));
                pause_ms(400);
            }
            if (demo::holdPairing()) {
                const std::string qr_path = "/tmp/halyard_demo_qr.bmp";
                const bool qr_ok = qr_generate_bmp(demo::pairingUrl(), qr_path.c_str(), 10);
                brls::Threading::sync([alive_flag, v, qr_ok, qr_path]() {
                    if (!alive_flag->load() || !v) return;
                    v->setPairing(demo::pairingUrl(), demo::pairingCode(),
                                  qr_ok ? qr_path : std::string());
                    v->setTimer(ui::tr("boot/grant_waiting", "09:41"));
                });
                return;   /* stays on the sign-in screen until the app is closed */
            }
            ShadowApp::instance().launcher_url = "demo";
            ShadowApp::instance().dc_name      = demo::datacenter();
            ShadowApp::instance().access_token = "demo";
            ui_set(v, alive_flag, ui::tr("boot/connected_to", ShadowApp::instance().dc_name));
            pause_ms(500);
            if (!alive_flag->load()) return;
            brls::Threading::sync([]() {
                balog("[NAV] boot screen -> pushing the VM list (demo)");
                brls::Application::pushActivity(new VmListActivity());
            });
            return;
        }

        // 1. HTTP/curl init
        ui_set(v, alive_flag, ui::tr("boot/net_init"));
        if (!http_global_init()) {
            ui_error(v, alive_flag, ui::tr("boot/err_http_init"), self);
            return;
        }

        // UX1 A6 2026-05-18 - Connectivity check (HEAD to 1.1.1.1)
        ui_set(v, alive_flag, ui::tr("boot/net_check"));
        if (!http_check_connectivity()) {
            ui_error(v, alive_flag,
                     ui::tr("boot/err_offline"), self);
            return;
        }

        // 2. TINAG (datacenter discovery) - UX1 A3 retry with exponential backoff
        ui_set(v, alive_flag, ui::tr("boot/datacenter"));
        GapInfo gi = {0};
        long http = 0;
        bool tinag_ok = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            if (tinag_get_datacenter("test@example.com", &gi, &http)) {
                tinag_ok = true;
                brls::Logger::info("TINAG OK (attempt {}) http={}", attempt, http);
                break;
            }
            /* S1: trace every failure. `http=0` means the request never got
             * through at all (DNS or TLS); the curl detail is in stderr.log. */
            brls::Logger::error("TINAG FAILED attempt {}/3 http={} (0 = no answer: "
                                "DNS/TLS, see stderr.log)", attempt, http);
            if (attempt < 3) {
                int delay_s = 2 << (attempt - 1);  /* 2, 4, 8 */
                /* UIFIX-1 - it used to read "Retry TINAG (1/3) in 2s...", an
                 * internal service name, in English whatever the language. */
                std::string msg = ui::tr("boot/retry_dc", attempt, delay_s);
                ui_set(v, alive_flag, msg);
                /* AF3 2026-09-10 - sliced: a single sleep of 2, 4 then 8 s never
                 * read `alive_flag`, and the exit joins this thread (rule 1:
                 * a long-lived thread sees its stop flag within 100 ms). */
                for (int t = 0; t < delay_s * 10 && alive_flag->load(); t++) {
                    struct timespec ts = {0, 100000000L};
                    nanosleep(&ts, NULL);
                }
            }
        }
        if (!tinag_ok) {
            // UX1 A4 - error screen diagnostic depending on the HTTP code
            std::string diag;
            if (http == 0) {
                diag = ui::tr("boot/err_dc_unreachable");
            } else if (http >= 500) {
                diag = ui::tr("boot/err_server", http);
            } else if (http == 404 || http == 403) {
                diag = ui::tr("boot/err_region", http);
            } else {
                diag = ui::tr("boot/err_other", http);
            }
            /* S3: the build fingerprint on the error screen - without it there
             * is no way to tell, console in hand, whether the binary under test
             * is the one just pushed (this really happened: a test was run
             * against the previous build). */
            /* S85: `version.h` carries the fingerprint. This file used to rely
             * on a bare `#ifndef SHADOW_BUILD_ID` without ever including
             * `build_id.h`, so this screen has displayed "build inconnu" SINCE
             * FOREVER - precisely the test round trip S3 was meant to remove,
             * on the one screen where it is needed. */
            diag += "\n\nv" SHADOW_VERSION " — build " SHADOW_BUILD_ID;
            ui_error(v, alive_flag, diag, self);
            return;
        }
        ShadowApp::instance().launcher_url = gi.launcher_api_url ? gi.launcher_api_url : "";
        /* An empty string means absent: otherwise it comes back out verbatim in
         * the interface and produces truncated labels. */
        ShadowApp::instance().dc_name      = (gi.name && gi.name[0]) ? gi.name : "";
        gapinfo_free(&gi);

        if (!alive_flag->load()) return;

        // 3. OAuth - try refresh from saved token, else interactive Device Grant
        // UX1 A3 retry on OIDC discover
        ui_set(v, alive_flag, ui::tr("boot/auth"));
        OidcDiscovery disc = {0};
        long s = 0;
        bool disc_ok = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            if (oauth_discover(&disc, &s)) {
                disc_ok = true;
                break;
            }
            if (attempt < 3) {
                int delay_s = 2 << (attempt - 1);
                for (int t = 0; t < delay_s * 10 && alive_flag->load(); t++) {   /* AF3 */
                    struct timespec ts = {0, 100000000L};
                    nanosleep(&ts, NULL);
                }
            }
        }
        if (!disc_ok) {
            std::string diag;
            if (s == 0) {
                diag = ui::tr("boot/err_auth_unreachable");
            } else if (s >= 500) {
                diag = ui::tr("boot/err_auth_server", s);
            } else {
                diag = ui::tr("boot/err_auth_other", s);
            }
            ui_error(v, alive_flag, diag, self);
            return;
        }

        ShadowAuthState auth = {0};
        bool auth_ok = false;
        char *saved = nullptr;

        if (oauth_load_refresh(&saved)) {
            auth.refresh_token = saved;
            if (oauth_refresh(&disc, SHADOW_OAUTH_CLIENT_ID, &auth)) {
                auth_ok = true;
            } else {
                oauth_state_free(&auth);
                memset(&auth, 0, sizeof(auth));
            }
        }

        if (!auth_ok) {
            // Interactive Device Grant : show user_code + verification URL
            DeviceGrantInit grant = {0};
            long http = 0;
            if (!oauth_device_init(&disc, &grant, &http)) {
                ui_error(v, alive_flag,
                         ui::tr("boot/err_device_init", http), self);
                oauth_discovery_free(&disc);
                return;
            }
            std::string code = grant.user_code ? grant.user_code : "";
            std::string url  = grant.verification_uri ? grant.verification_uri : "https://shadow.tech/device";
            std::string url_complete = grant.verification_uri_complete && *grant.verification_uri_complete
                                       ? grant.verification_uri_complete : url;
            int interval = grant.interval > 0 ? grant.interval : 5;
            int expires  = grant.expires_in > 0 ? grant.expires_in : 600;
            char *device_code = grant.device_code ? strdup(grant.device_code) : nullptr;

            // Generate QR code BMP encoding the FULL verification URI (= clickable from phone)
            std::string qr_path = "/tmp/shadow_grant_qr.bmp";
            bool qr_ok = qr_generate_bmp(url_complete.c_str(), qr_path.c_str(), 10);

            /* Switch to pairing mode. The QR path is passed EMPTY when it could
             * not be produced: the view then shows the address and the code
             * alone, which is still usable - a missing QR must not prevent
             * signing in. */
            if (alive_flag->load()) {
                const std::string path = qr_ok ? qr_path : std::string();
                brls::Threading::sync([alive_flag, v, code, url, path]() {
                    if (!alive_flag->load() || !v) return;
                    v->setPairing(url, code, path);
                });
            }

            int waited = 0;
            while (alive_flag->load() && waited < expires && device_code) {
                /* AF3 2026-09-10 - a 100 ms grain instead of 1 s (rule 1), and the
                 * flag read again BEFORE the poll: the loop tested it every second
                 * yet still sent one more POST after an exit had begun. */
                for (int j = 0; j < interval * 10 && alive_flag->load(); j++) {
                    svcSleepThread(100000000ULL);
                }
                if (!alive_flag->load()) break;
                waited += interval;
                OAuthPollResult r = oauth_device_poll(&disc, device_code,
                                                     SHADOW_OAUTH_CLIENT_ID, &auth);
                if (r == OAUTH_POLL_SUCCESS) {
                    auth_ok = true;
                    break;
                } else if (r == OAUTH_POLL_SLOW_DOWN) {
                    interval += 2;
                } else if (r == OAUTH_POLL_DENIED || r == OAUTH_POLL_EXPIRED) {
                    break;
                }
                int remaining = expires - waited;
                int mm = remaining / 60;
                int ss = remaining % 60;
                char buf[80];
                snprintf(buf, sizeof(buf), "%02d:%02d", mm, ss);
                /* The TEXT is translated; only the countdown is formatted here.
                 * A hard-coded sentence made this screen monolingual. */
                std::string msg = ui::tr("boot/grant_waiting", buf);
                if (alive_flag->load()) {
                    brls::Threading::sync([alive_flag, v, msg]() {
                        if (alive_flag->load() && v) v->setTimer(msg);
                    });
                }
            }
            free(device_code);
            oauth_device_init_free(&grant);

            if (!auth_ok) {
                // Restore hero box + show error
                if (alive_flag->load()) {
                    brls::Threading::sync([alive_flag, v]() {
                        if (alive_flag->load() && v) v->exitPairing();
                    });
                }
                ui_error(v, alive_flag, ui::tr("boot/auth_failed"), self);
                oauth_discovery_free(&disc);
                return;
            }

            /* Success: back to the home screen. */
            if (alive_flag->load()) {
                brls::Threading::sync([alive_flag, v]() {
                    if (alive_flag->load() && v) v->exitPairing();
                });
            }
        }
        oauth_save_refresh(&auth);
        oauth_discovery_free(&disc);

        // Store into the singleton - copy the strings before the free()
        ShadowApp::instance().access_token  = auth.access_token  ? auth.access_token  : "";
        ShadowApp::instance().refresh_token = auth.refresh_token ? auth.refresh_token : "";
        oauth_state_free(&auth);

        if (!alive_flag->load()) return;

        // Start the launcher.status telemetry in the background. The browser
        // sends it every ~3s for the whole session; we reproduce that pattern
        // so the server-side QoS scoring matches exactly.
        // Bearer = access_token. Keeps running until the global shutdown.
        telemetry_start(ShadowApp::instance().access_token.c_str());

        // 4. All good -> push VmListActivity
        /* The datacenter name is not always filled in by the TINAG reply: we
         * used to display "Connecte a " followed by nothing, or by "?". Build a
         * sentence around a value only when that value is actually there. */
        const std::string &dc = ShadowApp::instance().dc_name;
        ui_set(v, alive_flag,
               dc.empty() ? ui::tr("boot/connected")
                          : ui::tr("boot/connected_to", dc));

        // Short delay so the user gets to see the success
        for (int i = 0; i < 5 && alive_flag->load(); i++) {
            svcSleepThread(100000000ULL);  // 100ms
        }
        if (!alive_flag->load()) return;

        brls::Threading::sync([]() {
            balog("[NAV] boot screen -> pushing the VM list");
            brls::Application::pushActivity(new VmListActivity());
        });
    });
}
