#include "../ui/sfx.hpp"
#include <cstdio>
#include "activity/vm_list_activity.hpp"
#include "vm_list_view.hpp"
#include "settings.hpp"
extern "C" {
#include "core/services/errors.h"
}
#include "core/services/log.h"        /* [NAV] traces */
/* S81 - this module's log category. See shadow/journal.h: it is DECLARED here,
 * never inferred from the text of the messages. */
#define vllog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)
#define vldbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_UI, __VA_ARGS__)

#include "../ui/screen_base.hpp"
#include "../ui/i18n.hpp"
#include "activity/shadow_app.hpp"
#include "../demo.hpp"                /* DEMO-1 */
#include "activity/connecting_activity.hpp"
#include "activity/settings_activity.hpp"   /* B1 2026-05-18 */

extern "C" {
#include "core/services/launcher.h"
}

#include <borealis/views/dialog.hpp>
#include "switch_compat.h"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <atomic>
#include <memory>
#include <ctime>

// === VmCard: what the machine list used to be ===
// One styled Box widget per machine - an interactable outer box plus two
// labels (alias + state). Generic Borealis "cells" were deliberately avoided,
// for a look closer to the "cloud gaming" theme: tinted background, large
// title, state shown as a pill.
//
// `make_vm_card` was removed on 2026-08-27: it built one focusable Borealis
// view per machine, and it is that family of views - destroyed on every reload
// - that produced the S63 crash. Cards are now VALUES (`ui::Item`), drawn by
// the framework - see vm_list_view.hpp.

static void ui_run(std::shared_ptr<std::atomic<bool>> alive,
                    std::function<void()> fn) {
    if (!alive || !alive->load()) return;
    brls::Threading::sync([alive, fn]() {
        if (alive->load()) fn();
    });
}

VmListActivity::VmListActivity() {
    alive = std::make_shared<std::atomic<bool>>(true);
    auto_connexion_annulee = std::make_shared<std::atomic<bool>>(false);
}

VmListActivity::~VmListActivity() {
    /* === DECISIVE TRACE (2026-08-27) ===
     * Symptom: "I end up back on the boot screen, frozen on Connexion
     * etablie". That screen is the ROOT of the stack: landing on it means the
     * machine list has been DESTROYED, and the boot screen - which finished its
     * job long ago - reappears exactly as it was.
     * The traces placed in the connecting screen never fired, so the pop comes
     * from somewhere else; this line will say WHEN. */
    vllog("[NAV] VM list DESTROYED - the boot screen will reappear");
    if (alive) alive->store(false);
}

brls::View *VmListActivity::createContentView()
{
    /* S65 - the screen IS the view. See the header: injecting our own view into
     * the Borealis tree left it with no height (a scrolling frame measures its
     * children, `setGrow` cannot help there) and kept all the light-themed
     * chrome around it. The view now takes the whole screen and paints its own
     * background. */
    vmView = new VmListView();
    return vmView;
}

void VmListActivity::onContentAvailable() {
    ShadowApp &app = ShadowApp::instance();
    /* The data centre is shown only when it is KNOWN. Writing "unknown" under
     * the title spends a line to teach nothing, while the plan line right next
     * to it already carries the useful information. */
    dc_    = app.dc_name;
    offer_ = ui::tr("vm/plan_loading");
    drive_.clear();
    updateHeader();

    fetchAccountInfo();

    /* === S64 2026-08-27 - THE SCREEN MOVES TO OUR OWN FRAMEWORK ===
     * We inject ONE view into the existing scrolling container, and that view
     * draws everything. No view is created or destroyed on reload any more:
     * that is what deletes the S63 crash family instead of guarding against
     * it. */
    vmView->setHeader(ui::tr("vm/title"),
                      app.dc_name.empty() ? ui::tr("vm/dc_unknown") : app.dc_name,
                      "");
    vmView->setStatus(ui::tr("vm/loading"), true);
    /* === S99 2026-08-29 - "NO MACHINES" IS NOT TRUE UNTIL YOU HAVE LOOKED ===
     * This message was set at construction time, hence BEFORE the server's
     * first reply: arriving on the screen you read "no machines" while the
     * request was still in flight. It is an assertion, and it was false at the
     * very moment you read it. So we say what we know - that we are looking -
     * and the definitive message only replaces that one once the reply is
     * in. */
    vmView->setEmptyMessage(ui::tr("vm/searching"));
    vmView->enableTiles();

    /* The footer button hints. They used to be drawn by the Borealis frame;
     * taking over the whole screen lost them, and the screen no longer said
     * what it can do. On a console this is the ONLY place you learn it: there
     * is no hover and no context menu.
     * The order follows the convention: the primary action last, hence closest
     * to the right edge. */
    vmView->setHints({
        { "B", ui::tr("action/quit")     },
        { "Y", ui::tr("settings/open")   },
        { "X", ui::tr("action/refresh")  },
        { "A", ui::tr("action/connect")  },
    });
    vmView->setOnActivate([this](const std::string &id) {
        /* The alias is looked up at activation time: keeping a second copy of
         * it in the view would let the two diverge on the first reload. */
        for (const ShadowVm &vm : ShadowApp::instance().vms) {
            if (vm.id == id) {
                this->onVmClicked(id, vm.alias.empty() ? vm.name : vm.alias);
                return;
            }
        }
    });

    /* Navigation: the directions drive the view's internal INDEX. There are no
     * sibling views left for Borealis to look for a neighbour among - hence no
     * focus restoration left to get wrong. */
    /* Shared navigation wiring. X, Y and B keep their own registrations just
     * below: they carry roles specific to this screen (refresh, settings, quit
     * the application) that the shared wiring has no business knowing. */
    ui::wireNavigation(this, vmView);

    /* S102 - X goes through the VIEW: see vm_list_view.hpp for why. Both paths
     * - the physical button and a tap on the footer hint - then call the same
     * function. */
    vmView->setOnRefresh([this] { refreshVms(); });

    /* Y opens Settings. It used to lead straight to the single "Stream
     * quality" page: everything else (language, gamepad) had no entry point at
     * all, and that one page was announced under a name that gave no hint
     * there was nothing else. */
    this->registerAction(
        ui::tr("settings/open"),
        brls::ControllerButton::BUTTON_Y,
        [this](brls::View *) {
            /* D6 - cancels the deferred auto-connect: without this it fired ON
             * TOP of the settings screen we had just opened. */
            if (auto_connexion_annulee) auto_connexion_annulee->store(true);
            brls::Application::pushActivity(new SettingsActivity());
            return true;
        });

    /* Signing out used to live here, on a footer button. It added a fifth hint
     * for a gesture performed once a month, and its natural home is the
     * Settings screen - that is where one goes looking for anything account
     * related. It has been moved there. */
    this->registerAction(
        ui::tr("action/quit"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View *) {
            /* S71 - ask the view FIRST: if a dialog is already open, B must
             * CLOSE it. Without this test we opened a second one on top of the
             * first, and the only way out was to pick a button - that is, to
             * quit the application in order to cancel. */
            if (vmView && vmView->back()) return true;
            vllog("[NAV] B on the VM list - opening the exit dialogue");
            /* === S70 2026-08-27 - THE DIALOG IS NO LONGER AN ACTIVITY ===
             * `brls::Dialog::open()` pushed an activity, and its `buttonClick`
             * popped it before calling the button's code. That mechanism is
             * what produced S66: our `close()` added a SECOND pop, which took
             * the screen underneath with it.
             * Here the question is a STATE of the screen. There is no stack
             * left to pop, hence no extra pop is possible - whatever code
             * someone writes in a button tomorrow. */
            if (vmView)
                vmView->ask(ui::tr("vm/quit_question"), {
                    { ui::tr("action/cancel"), nullptr, false },
                    { ui::tr("action/quit"), []() { brls::Application::quit(); }, true },
                });
            return true;
        });

    refreshVms();
}

/* Recomposes the header from the three texts that used to live in `Label`s.
 * The plan and Drive come from a network call separate from the machines' one,
 * and often later: they therefore land next to the title without making the
 * list wait - that was already the behaviour, and it had to be preserved. */
void VmListActivity::updateHeader()
{
    if (!vmView) return;
    std::string info = offer_;
    if (!drive_.empty()) info += info.empty() ? drive_ : ("   " + drive_);
    vmView->setHeader(ui::tr("vm/title"), dc_, info);
}

void VmListActivity::fetchAccountInfo() {
    auto alive_flag = alive;
    if (demo::enabled()) {   /* DEMO-1 - a fictional plan, no request */
        demo::account(offer_, drive_);
        updateHeader();
        return;
    }
    brls::Threading::async([this, alive_flag]() {
        ShadowApp &app = ShadowApp::instance();
        Subscription sub = {0};
        long st_sub = 0;
        bool ok_sub = launcher_get_subscription_status(app.access_token.c_str(), &sub, &st_sub);

        DriveToken drv = {0};
        long st_drv = 0;
        bool ok_drv = launcher_get_drive_token(app.access_token.c_str(), &drv, &st_drv);

        /* UX4 B12 2026-05-18 - plan info enriched with a formatted started_at
         * plus the payment status */
        std::string plan_text;
        if (ok_sub && sub.plan_short) {
            plan_text = ui::tr("vm/plan", sub.plan_short);
            if (sub.status && std::string(sub.status) != "active")
                plan_text += " (" + std::string(sub.status) + ")";
            if (sub.on_hold) plan_text += " - " + ui::tr("vm/plan_hold");

            if (sub.started_at > 0) {
                time_t t = (time_t)sub.started_at;
                struct tm tm_local;
                #ifdef _WIN32
                localtime_s(&tm_local, &t);
                #else
                localtime_r(&t, &tm_local);
                #endif
                char date_buf[32];
                strftime(date_buf, sizeof(date_buf), "%d/%m/%Y", &tm_local);
                plan_text += " - " + ui::tr("vm/plan_since", date_buf);
            }

            /* The payment status is reported only when it is a problem. Shadow
             * answers `payment_succeed` when all is well: that value was
             * missing from the accepted list, so a subscription in good
             * standing was displayed as an anomaly, next to a symbol the font
             * cannot render. */
            if (sub.last_payment_status) {
                const std::string st = sub.last_payment_status;
                const bool fine = (st == "ok" || st == "succeeded" || st == "succeed"
                                   || st == "payment_succeed" || st == "payment_succeeded"
                                   || st == "success" || st == "paid" || st.empty());
                if (!fine) plan_text += " - " + ui::tr("vm/payment_issue", st);
            }
        } else {
            /* UX2 A4 - diagnostic error message */
            if (st_sub == 0)        plan_text = ui::tr("vm/plan_offline");
            else if (st_sub == 401) plan_text = ui::tr("vm/plan_expired");
            else if (st_sub >= 500) plan_text = ui::tr("vm/plan_server", st_sub);
            else                    plan_text = ui::tr("vm/plan_error", st_sub);
        }

        std::string drive_text;
        if (ok_drv && drv.token)        drive_text = ui::tr("vm/drive_on");
        else if (ok_drv && drv.message) drive_text = std::string("Drive: ") + drv.message;
        else                            drive_text = ui::tr("vm/drive_off");

        /* UX4 B9 2026-05-18 - battery warning when the console is below 15% */
#ifdef __SWITCH__
        /* D14 2026-08-21 - Switch build RESTORED. This block used to redeclare
         * the psm symbols by hand (`extern int psmInitialize(void)`) instead of
         * using <switch.h>, already included at line 13. libnx declares
         * `Result psmInitialize(void)` (Result = u32): ever since a libnx
         * update started exposing <switch/services/psm.h> through that include
         * chain, the two declarations conflict and gcc rejects the file
         * ("ambiguating new declaration of 'int psmInitialize()'"). The Switch
         * target therefore stopped compiling at commit 19abcd2 (UX4 B9,
         * 2026-05-18) - the .nro on the SD card dated from 14 May.
         * We rely on the header and on R_SUCCEEDED rather than on `== 0`. */
        uint32_t batt_pct = 100;
        if (R_SUCCEEDED(psmInitialize())) {
            u32 pct = 100;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) batt_pct = pct;
            psmExit();
        }
        if (batt_pct < 15) {
            drive_text += ui::tr("vm/battery_low", batt_pct);
        }
#endif

        ui_run(alive_flag, [this, plan_text, drive_text]() {
            offer_ = plan_text;
            drive_ = drive_text;
            updateHeader();
        });

        subscription_free(&sub);
        drive_token_free(&drv);
    });
}

void VmListActivity::refreshVms() {
    auto alive_flag = alive;
    /* === S65 2026-08-27 - THERE IS NOTHING LEFT TO CLEAR ===
     * S63 had fixed the ORDER (clear, then give the focus) because
     * `clearViews()` freed the cards under the focus. Now that the screen is a
     * single view there is no card left to free: reloading replaces a vector of
     * values. So we simply announce the wait, and the content is replaced only
     * once valid data has arrived - which also preserves D3: a failed refresh
     * leaves the previous list on screen. */
    VmListView *view = vmView;
    /* === S103 2026-08-29 - A REFRESH THAT CANNOT BE SEEN DID NOT HAPPEN, AS
     *     FAR AS THE USER IS CONCERNED ===
     *
     * Reported: "I press X and absolutely nothing happens". The action did
     * fire, and so did the request - but with two machines and a reply in under
     * a second, the spinner appeared and disappeared between two frames, and
     * the list came back identical. Nothing on screen told "refreshed" apart
     * from "keypress ignored".
     *
     * So we ANNOUNCE it, and the confirmation sound goes with it: that is the
     * only immediate feedback available when the result is identical to what
     * was already there. */
    ui_run(alive_flag, [view]() {
        if (view) view->setStatus(ui::tr("vm/refreshing"), true);
        ui::sfx::play(ui::sfx::Sound::Confirm);
    });

    if (demo::enabled()) {   /* DEMO-1 - fictional machines, no request */
        demo::fillVms();
        ui_run(alive_flag, [this, view]() {
            renderVmCards();
            if (view) view->setStatus(ui::tr("vm/count", ShadowApp::instance().vms.size(),
                                             ShadowApp::instance().vms.size()), false);
        });
        return;
    }

    brls::Threading::async([this, alive_flag, view]() {
        ShadowApp &app = ShadowApp::instance();
        VmPage page = {0};
        long http = 0;
        bool ok = launcher_list_vms(
            app.launcher_url.c_str(),
            app.access_token.c_str(),
            0, 50, &page, &http);

        if (!alive_flag->load()) { vmpage_free(&page); return; }

        if (!ok) {
            /* D3 - the previous list STAYS on screen: losing the network for a
             * second must not clear the screen until the app is restarted. */
            /* === S82 2026-08-29 - THE CAUSE, NOT THE CODE ===
             * This line used to display "HTTP error 401". Accurate, and
             * unusable on a console: the code has a precise cause and a precise
             * answer, and neither of them was on screen.
             *
             * Here we keep only the CAUSE, not the action: this is a one-line
             * status bar, under a list that stays displayed (D3). The full
             * sentence, with what can be done about it, belongs to the
             * connecting screen, where there is room and where the user is
             * waiting. The code stays, in parentheses: it is what you ask
             * someone to read out to you when helping them remotely. */
            ui_run(alive_flag, [view, http]() {
                if (!view) return;
                const error_explanation_t x =
                    error_explain(ERR_STEP_INVENTORY, http, ERR_NET_NONE);
                char code[48];
                error_detail(code, sizeof code, http, ERR_NET_NONE);
                view->setStatus(ui::tr(x.key) + "  (" + code + ")", false);
            });
            return;
        }

        // Convert C VmInfo into C++ ShadowVm inside the singleton
        app.vms.clear();
        for (size_t i = 0; i < page.count; i++) {
            const VmInfo *v = &page.items[i];
            ShadowVm sv;
            sv.id    = v->id    ? v->id    : "";
            sv.name  = v->name  ? v->name  : "";
            sv.alias = v->alias ? v->alias : (v->name ? v->name : "");
            sv.state = v->state ? v->state : "";
            app.vms.push_back(sv);
        }
        size_t total = page.total;
        vmpage_free(&page);

        /* The content is replaced only HERE, once VALID data is in hand - that
         * is what keeps the previous list displayed when the reload fails (D3).
         * `renderVmCards` also writes the count into the status bar, hence the
         * absence of a second call to set it. */
        ui_run(alive_flag, [this, view, total]() {
            renderVmCards();
            /* The reply has arrived: "no machines" is now an assertion we are
             * entitled to make. */
            if (view) view->setEmptyMessage(ui::tr("vm/empty"));
            /* UI11 - published so `renderVmCards` says the same thing. */
            ShadowApp::instance().vms_total = total;
            if (view) view->setStatus(ui::tr("vm/count",
                                             ShadowApp::instance().vms.size(), total),
                                      false);
        });
    });
}

void VmListActivity::renderVmCards() {
    ShadowApp &app = ShadowApp::instance();

    /* === S64 2026-08-27 - NO VIEW IS CREATED OR DESTROYED HERE ANY MORE ===
     *
     * The old version built one focusable view per machine and destroyed them
     * on every reload. That is what produced the S63 crash:
     * `Application::currentFocus` stayed on a freed card and the next virtual
     * call jumped into a dead vtable.
     *
     * We now fill a `std::vector` of values. `VmListView` is the screen's only
     * focusable view and lives as long as the activity: there is nothing left
     * to free under the focus. The internal focus is an index, re-clamped on
     * every frame, and `ui::ListScreen` reattaches it BY IDENTIFIER - so the
     * selection no longer jumps when a machine appears or disappears above it
     * during a reload. */
    if (!vmView) return;

    std::vector<VmRow> rows;
    rows.reserve(app.vms.size());
    for (const ShadowVm &vm : app.vms) {
        VmRow r;
        r.id  = vm.id;
        r.name = vm.alias.empty() ? vm.name : vm.alias;
        if (r.name.empty()) r.name = ui::tr("vm/unnamed");   /* D7: translated, not hardcoded */
        r.subtitle = vm.state;
        rows.push_back(std::move(r));
    }
    vmView->setVms(rows);

    /* UI11 - TWO arguments, because the format has two. Passing one made
     * Borealis refuse the string and leave the status line blank. When no reply
     * has been seen yet the list IS the total, which is true and not a guess. */
    vmView->setStatus(app.vms.empty()
                          ? ui::tr("vm/empty")
                          : ui::tr("vm/count", app.vms.size(),
                                   app.vms_total ? app.vms_total : app.vms.size()),
                      false);

    if (app.vms.empty()) return;

    /* === S62 - AUTO-CONNECT ===
     * It existed only as an environment variable, hence unreachable on a
     * console, and it re-fired when coming back from a stream (`onResume`
     * reloads the list), reopening a session 500 ms later: impossible to reach
     * the settings or pick another machine without being sucked back in. So it
     * now arms only ONCE per lifetime of the screen. The variable keeps
     * priority when it is set, for automated test runs. */
    const char *auto_env = getenv("SHADOW_AUTO_CONNECT");
    const bool auto_connect = auto_env ? (atoi(auto_env) != 0)
                                       : Settings::instance().auto_connect;
    if (auto_connect && !auto_connexion_faite && app.vms.size() == 1) {
        auto_connexion_faite = true;
        std::string id    = app.vms[0].id;
        std::string alias = app.vms[0].alias.empty() ? app.vms[0].name : app.vms[0].alias;
        std::shared_ptr<std::atomic<bool>> alive_local = this->alive;
        /* D6 2026-08-27 - the deferred call must be CANCELLABLE. Without that,
         * pressing Y within the second opens the settings AND THEN the
         * connection fires on top of them. `auto_connexion_annulee` is raised
         * as soon as the screen is left by any other route. */
        auto cancelled = this->auto_connexion_annulee;
        brls::Logger::info("[auto-connect] 1 machine - auto-connecting to {} in 500 ms", id);
        /* S8 - no detached thread for a mere delay: a thread capturing `this`
         * outlives the destruction of the activity. Borealis offers a deferred
         * call run on the main thread, without creating a thread at all. */
        brls::Threading::delay(500, [alive_local, cancelled, id, alias, this]() {
            if (!alive_local || !alive_local->load()) return;
            if (cancelled && cancelled->load()) {
                brls::Logger::info("[auto-connect] cancelled - the user went elsewhere");
                return;
            }
            this->onVmClicked(id, alias);
        });
    }
}

void VmListActivity::onResume() {
    vllog("[NAV] liste-VM redevient visible");
    /* === S50 2026-08-26 - RELOAD THE STATE WHEN COMING BACK FROM A STREAM ===
     *
     * `onResume` only gave the focus back. Ever since leaving the stream
     * returns here (S40), the screen redisplayed the state from BEFORE the
     * connection: greyed-out buttons and "Etat inconnu", with no way out but
     * restarting the application. A VM's state necessarily changes during a
     * session - it has to be re-read.
     *
     * The first appearance is already preceded by a `refreshVms()` during
     * construction: we therefore only reload from the second one on. */
    if (!first_appearance) {
        refreshVms();
    }
    first_appearance = false;

    /* S65 - a single focusable view, and it lives as long as the activity:
     * there is no candidate left to choose and no destroyed view to avoid. */
    if (vmView) brls::Application::giveFocus(vmView);
}

void VmListActivity::onVmClicked(const std::string &vm_id,
                                   const std::string &alias) {
    if (vm_id.empty()) return;

    ShadowApp &app = ShadowApp::instance();
    app.selected_vm_id    = vm_id;
    app.selected_vm_alias = alias;
    app.resetAbort();

    /* S6 2026-08-22 - diagnostic milestones: on Switch the crash happens
     * exactly at this transition (the log stops on the VM list, before even the
     * connecting screen's first message). These three lines say whether it is
     * the construction of the activity, pushing it on the stack, or what
     * follows that crashes. */
    brls::Logger::info("[S6] VM choisie id={} alias={} — construction ConnectingActivity",
                       vm_id, alias);
    auto *act = new ConnectingActivity();
    brls::Logger::info("[S6] ConnectingActivity construite — pushActivity");
    brls::Application::pushActivity(act);
    brls::Logger::info("[S6] pushActivity revenu");
}
