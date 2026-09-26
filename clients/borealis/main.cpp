// halyard (Borealis-based UI) — entry point.
//
// Activity flow:
//   BootActivity (TINAG + OAuth refresh)
//      ↓ success
//   VmListActivity (interactive cards)
//      ↓ user picks VM
//   ConnectingActivity (steps with icons)
//      ↓ session up
//   StreamActivity (full-screen video)

#include <borealis.hpp>
#include <cstdlib>
#include <typeinfo>
#include <cstring>
#if defined(_WIN32)
#  include <process.h>   // _exit
#else
#  include <unistd.h>    // _exit
#endif
#include "../../core/services/win_compat.h"   /* WIN1 - setenv/unsetenv, absent from the Windows CRT */
#include "activity/boot_activity.hpp"
#include "activity/lock_activity.hpp"
#include "clients/borealis/ui/i18n.hpp"
#include "autotest.hpp"
#include "activity/shadow_app.hpp"   /* UX6 B3 2026-05-18 — signalAbort sleep */
#include "settings.hpp"
#include "device_mode.hpp"   /* AF10 - device::releaseServices */
#include "clients/borealis/ui/sfx.hpp"
#include "core/version.h"           /* S85 - version, date and git hash */
#include "clients/borealis/devlink/devlink.hpp"       /* screenshots and state driven from the dev machine */
#include "clients/borealis/devlink/relaunch.hpp"      /* RELOAD-1: hand over to another .nro on exit */
#include "clients/borealis/devlink/envfile.hpp"       /* DEVL-6: the SHADOW_* toggles, set from here */
#include "clients/borealis/devlink/inject.hpp"        /* INJ-1: synthetic input, driven from the dev machine */
#include "clients/borealis/devlink/authz.hpp"         /* AUTH-1: the owner authorises the machine */
#include "clients/borealis/devlink/devcmd.h"          /* the COMMON parser of the command channel */
#include "clients/borealis/ui/shutdown.hpp"           /* UX10 - the shutdown state */
#include "../../core/protocol/ctrl_session.h"  /* CFG-1 - the live-bitrate diagnostic */
#ifndef __SWITCH__
#include "activity/shutdown_activity.hpp"
#endif
#include "gl_compat.h"
#if SHADOW_HAVE_DESKTOP_GL
#include <GLFW/glfw3.h>
#endif

extern "C" {
#include "../../core/services/http.h"      // http_global_cleanup
#include "../../core/services/telemetry.h" // telemetry_stop
#include "../../core/common/log.h"
/* S81 - this module's category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define mnlog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)
#define mndbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#endif
#if defined(__vita__) || defined(__psp2__)
#  include <psp2/io/stat.h>
#endif
extern "C" {
#include "../../core/services/atomic_file.h"   /* shadow_file_remove: `remove` does not resolve ux0: */
}
#include "../../core/services/config.h"          // SHADOW_DATA_DIR
#include "../../core/services/env_override.h"
#include "../../core/services/crypto_selftest.h"
#include "../../core/services/power_profile.h"
#include "../../core/services/applock_store.h"
#include "../../core/services/sockets_compat.h"

#ifdef __SWITCH__
#include <switch.h>   /* D14: appletHook/appletGetFocusState and their types */
#endif
}

/* === S38 2026-08-25 - LOADING THE TOGGLES FROM A FILE ===
 *
 * The project's whole experiment mechanism rests on ~120 `SHADOW_*` environment
 * variables: every fix ships with its revert toggle so a live A/B needs no
 * rebuild.
 *
 * On console that mechanism was UNUSABLE: the home menu passes no environment,
 * and nothing read one anywhere else. Every trial therefore cost a full rebuild
 * and a 22 MB upload - to the point where we stopped running them.
 *
 * So we read `SHADOW_DATA_DIR "env.txt"` at the very start of `main`, BEFORE any
 * environment read at all (`shadow_sockets_init` already reads
 * SHADOW_UDP_RXBUF). One `KEY=VALUE` per line, `#` for a comment. Only keys
 * prefixed `SHADOW_` are accepted: this file comes from the SD card, it has no
 * business redefining `PATH` or `LD_PRELOAD`.
 *
 * An A/B on console now costs uploading a thirty-byte file. */
static char g_env_summary[512];
static size_t g_env_off = 0;
static bool g_env_file_seen = false;   /* tells "no file" apart from "file with no toggle" */

/* === THE DATA DIRECTORY HAS TO EXIST BEFORE ANYTHING READS IT ==============
 *
 * `journal.c` opens its log with a plain `fopen(..., "w")`, which fails
 * silently when the directory is not there, and `env.txt` is read a few lines
 * below. On a console that directory IS the interface - toggles, log mirror,
 * token - so a fresh install with no directory is an application you cannot
 * observe and cannot configure.
 *
 * It used to be created only by `oauth.c`, at pairing time, which is far too
 * late: everything above wants it at startup. The Switch got away with it
 * because `switch-sync.sh` makes the directory on the SD card; a fresh PS Vita
 * install has nobody to do that.
 *
 * Failure is deliberately silent HERE: if the directory cannot be made, every
 * caller below already handles its own file being absent, and a message at this
 * point would have nowhere to go - the journal is not open yet. */
/* `shadow_ensure_data_dir()` now lives in `core/services/atomic_file.c`: it
 * was here AND in oauth.c, and the two disagreed about the path. */
static void shadow_load_env_file(void)
{
    char path[256];
    std::snprintf(path, sizeof(path), "%senv.txt", SHADOW_DATA_DIR);
    std::FILE *f = std::fopen(path, "r");
    if (!f) return;
    g_env_file_seen = true;
    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        char *eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        /* cut the end of line and the trailing whitespace */
        for (char *q = val; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
        for (char *q = key + std::strlen(key); q > key && (q[-1] == ' ' || q[-1] == '\t'); q--) q[-1] = 0;
        if (std::strncmp(key, "SHADOW_", 7) != 0) continue;   /* refused by default */
        setenv(key, val, 1);
        /* Check that it TOOK. On console nothing guarantees that an environment
         * exists: a `setenv` that fails silently would give an experiment we
         * believe we ran and did not - exactly the kind of phantom measurement
         * this repo has already paid for. */
        const char *readback = getenv(key);
        const bool ok = (readback && std::strcmp(readback, val) == 0);
        applied += ok ? 1 : 0;
        int n = std::snprintf(g_env_summary + g_env_off, sizeof(g_env_summary) - g_env_off,
                              "%s%s=%s%s", g_env_off ? " " : "", key, val,
                              ok ? "" : "(FAILED)");
        if (n > 0 && (size_t)(g_env_off + n) < sizeof(g_env_summary)) g_env_off += n;
        std::fprintf(stderr, "[env] %s=%s%s\n", key, val, ok ? "" : " - setenv FAILED");
    }
    std::fclose(f);
    std::fprintf(stderr, "[env] %d toggle(s) loaded from %s\n", applied, path);
}

/* Replays the summary once the log is available: loading happens BEFORE the log
 * is opened, so without this the only trace goes to stderr, which the network
 * mirror does not carry - we were flying blind. */
/* === S45 2026-08-25 - STOPPING THE APPLICATION FROM THE DEV MACHINE ===
 *
 * The `.nro` file is locked as long as the application runs: every upload
 * therefore required a human manipulation (quit, wait, relaunch), and one
 * iteration cost several round trips. Since the log channel is already
 * bidirectional, we add `quit` to it.
 *
 * The stop goes through the SAME path as the pause menu: raise the session flag,
 * then ask for the close on the main thread. Exiting from the log thread would
 * close Borealis out from under its own rendering.
 *
 * Recognised commands: `shot`, `state`, `quit`, `ping`, `autotest N D P`.
 * Any other line is ignored silently - this channel can receive noise.
 *
 * Order matters: `devlink` first serves what belongs to it and returns true,
 * otherwise we fall back on the common parser, then on the autotest. */
static void shadow_command_handler(const char *line)
{
    if (!line) return;

    /* `shot` and `state`: devlink RECORDS the request and returns at once. The
     * capture itself waits for the render thread - `glReadPixels` only means
     * something where the OpenGL context is current, and calling it here would
     * return not a wrong image but nothing at all, or a crash depending on the
     * driver. Zero cost when the channel does not exist. */
    if (devlink::handleCommand(line)) return;

    /* The rest goes through the channel's COMMON parser (devlink/devcmd.h),
     * which works on an explicit length and accepts only a closed vocabulary.
     * The old `strncmp(line, "quit", 4)` test accepted "quitter" and
     * "quit maintenant": a merely SIMILAR line stopped the running session,
     * which is a steep price for a typo.
     *
     * The stop goes through the SAME path as the pause menu: session flag, then
     * the close requested on the main thread. Exiting from the log thread would
     * close Borealis out from under its own rendering. */
    devcmd_t cmd;
    if (devcmd_parse(line, std::strlen(line), &cmd) && cmd.kind == DEVCMD_QUIT) {
        mnlog("[cmd] stop requested from the development machine");
        ShadowApp::instance().signalAbort();
        brls::Threading::sync([]() { brls::Application::quit(); });
        return;
    }

    /* === INJ-1 2026-09-12 - PRESSING BUTTONS, AT LAST ========================
     *
     * `btn`, `nav` and `tap` were documented in DEVLINK.md, parsed by devcmd.h
     * and sent by devlink.py since 2026-08-27 - and NOTHING served them: the
     * module returned false saying injection "belongs to the input module", and
     * that module had never been written. The channel could observe and not act,
     * which made `state` a position report with no way to move. Found by
     * navigating on the console and watching `state` refuse to budge.
     *
     * Recorded here on the log thread and applied by the render thread through
     * the platform's input manager, so every consumer downstream sees a press it
     * cannot distinguish from a real one. See devlink/inject.h for why that
     * layer and no other. */
    switch (cmd.kind) {
    case DEVCMD_BTN:
        if (devlink::pressButton(cmd.a, 0)) mnlog("[devlink] ok btn %s", cmd.name);
        else                                mnlog("[devlink] err btn inconnu");
        return;
    case DEVCMD_NAV:
        if (devlink::pressDirection(cmd.a, 0)) mnlog("[devlink] ok nav %s", cmd.name);
        else                                   mnlog("[devlink] err nav inconnu");
        return;
    case DEVCMD_HOLD:
        if (devlink::pressButton(cmd.a, cmd.b))
            mnlog("[devlink] ok hold %s %d", cmd.name, cmd.b);
        else
            mnlog("[devlink] err hold inconnu");
        return;
    case DEVCMD_STICK:
        devlink::moveStick(std::strcmp(cmd.name, "right") == 0,
                           (float)cmd.a / 100.0f, (float)cmd.b / 100.0f, cmd.e);
        mnlog("[devlink] ok stick %s %d %d", cmd.name, cmd.a, cmd.b);
        return;
    case DEVCMD_TAP:
        devlink::touch((float)cmd.a, (float)cmd.b, (float)cmd.a, (float)cmd.b, 0);
        mnlog("[devlink] ok tap %d %d", cmd.a, cmd.b);
        return;
    case DEVCMD_SWIPE:
        devlink::touch((float)cmd.a, (float)cmd.b, (float)cmd.c, (float)cmd.d, cmd.e);
        mnlog("[devlink] ok swipe %d %d %d %d", cmd.a, cmd.b, cmd.c, cmd.d);
        return;
    case DEVCMD_RELEASE:
        devlink::releaseAll();
        mnlog("[devlink] ok release");
        return;
    default:
        break;
    }

    /* === DEVL-5 2026-09-12 - WHICH BUILD IS RUNNING ==========================
     *
     * The channel could not say what it was talking to. The banner went out by
     * `brls::Logger::info` and `fprintf(stderr, ...)`, neither of which passes
     * through `journal_write` - so it reached neither the mirror nor the session
     * log, and a whole iteration was spent SUPPOSING which build was on the
     * console. The answer was "one that predates the command being sent to it",
     * and this one reply would have said so in a second.
     *
     * Answered right here on the log thread: it reads two constants and a string
     * that has not changed since `main` started, so nothing needs the render
     * thread - which also means it still answers when the window is not being
     * composited (see tools/DEVLINK.md). */
    if (cmd.kind == DEVCMD_VERSION) {
        const std::string &nro = devlink::selfPath();
        mnlog("[devlink] version %s v%s nro=%s", SHADOW_BUILD_ID, SHADOW_VERSION,
              nro.empty() ? "(inconnu)" : nro.c_str());
        return;
    }

    /* === DEVL-6 2026-09-12 - THE EXPERIMENT TOGGLES, SET FROM HERE ===========
     *
     * `env` lists, `env KEY=VALUE` sets, `env KEY=` removes. It writes the file
     * the NEXT launch reads - never the running environment, because nearly
     * every toggle is read once into a function `static` and setting it now
     * would apply to whichever module has not read it yet, and to no other. An
     * experiment that half applies is worse than one not run. See envfile.hpp.
     *
     * Answered on the log thread like `version`: it touches a file, not the
     * screen. */
    if (cmd.kind == DEVCMD_ENV) {
        if (cmd.a == 0) {
            /* ONE line, not one per toggle: the reply collector on the other end
             * concludes on the first line that answers, so a multi-line list
             * would leave it waiting after the first. The toggles are few and
             * short, and the line is bounded on the reading side anyway. */
            const std::vector<std::string> l = devlink::envList();
            std::string joint;
            for (const std::string &e : l) { if (!joint.empty()) joint += " "; joint += e; }
            mnlog("[devlink] env-list %s", joint.empty() ? "(none)" : joint.c_str());
            return;
        }
        std::string pourquoi;
        if (!devlink::envSet(cmd.env_key, cmd.env_val, pourquoi)) {
            mnlog("[devlink] err env %s", pourquoi.c_str());
            return;
        }
        /* Says explicitly that it takes effect on the NEXT launch: a reply that
         * merely said "ok" would let a script measure the run in progress and
         * attribute to the toggle a session that never saw it. */
        mnlog("[devlink] ok env %s=%s (au prochain lancement)",
              cmd.env_key, cmd.env_val[0] ? cmd.env_val : "(retire)");
        return;
    }

    /* === RELOAD-1 2026-09-12 - STOP, THEN LOAD ANOTHER .nro ==================
     *
     * Deliberately the SAME exit path as `quit` above, and for the same reason:
     * leaving from the log thread would close Borealis out from under its own
     * rendering. The only difference is the next-load slot, armed BEFORE the
     * exit is requested - the order matters, since after `Application::quit()`
     * nothing guarantees we still run.
     *
     * On failure we stay ALIVE. Exiting anyway would drop the session and land
     * on hbmenu, i.e. exactly the human gesture this command exists to remove,
     * and the dev machine would be left to guess why. See devlink/relaunch.hpp. */
    if (cmd.kind == DEVCMD_RELAUNCH) {
        std::string pourquoi;
        if (!devlink::relaunchInto(cmd.path, pourquoi)) {
            mnlog("[devlink] err relaunch %s", pourquoi.c_str());
            return;
        }
        mnlog("[devlink] ok relaunch %s",
              cmd.path[0] ? cmd.path : devlink::selfPath().c_str());
        ShadowApp::instance().signalAbort();
        brls::Threading::sync([]() { brls::Application::quit(); });
        return;
    }

    autotest::handleCommand(line);
}

/* === AUTH-1 2026-09-12 - THE QUESTION, ASKED WHERE IT CAN BE ANSWERED =====
 *
 * Until now the only guard on the [devlink] channel was the PRESENCE of
 * `logsink.txt`: drop that file and you can read the screen, press buttons,
 * change experiment toggles and relaunch the application. Reasonable on a
 * bench, poor on a console someone else can reach - and it asks nothing of the
 * person actually holding the device.
 *
 * So the Android model: the first time a machine opens the channel, the console
 * asks, naming it. Nothing passes meanwhile - neither the log nor the commands.
 *
 * Polled from the main loop because a dialogue can only be opened on the UI
 * thread, while the channel opens on the drain thread. The poll is two atomic
 * reads and a string compare when idle, and it raises a dialogue at most once
 * per connection. */
static void askDevlinkAuthorisation()
{
    static bool showing = false;
    if (showing) return;

    const std::string peer = devlink::pendingPeer();
    if (peer.empty()) return;
    showing = true;

    brls::Dialog *dlg = new brls::Dialog(ui::tr("devlink/body", peer));
    dlg->setCancelable(false);   /* an unanswered question must not become a yes */

    /* REFUSING COMES FIRST: the console puts the cursor on the first button, and
     * the safe answer is the one a distracted press should land on. Same rule as
     * every destructive dialogue in this application. */
    dlg->addButton(ui::tr("devlink/deny"), [] {
        devlink::answer(false, true);   /* remembered: do not pester on every launch */
        showing = false;
    });
    dlg->addButton(ui::tr("devlink/allow"), [] {
        devlink::answer(true, false);   /* this session only */
        showing = false;
    });
    dlg->addButton(ui::tr("devlink/allow_always"), [] {
        devlink::answer(true, true);
        showing = false;
    });
    dlg->open();
}

static void shadow_log_env_file(void)
{
    if (g_env_off > 0)        mnlog("[env] bascules actives : %s", g_env_summary);
    else if (g_env_file_seen) mnlog("[env] %senv.txt read, no toggle active",
                                          SHADOW_DATA_DIR);
    else                      mnlog("[env] no %senv.txt", SHADOW_DATA_DIR);
    /* Of those toggles, the ones a SETTING claims to control. The distinction
     * matters when reading a log after the fact: the ~200 others are experiment
     * reverts with no UI, and nothing on screen contradicts them. These ones the
     * screen DOES contradict - so a session opened with them behaves in a way
     * the settings file cannot explain. */
    if (env_override_count() > 0)
        mnlog("[env] %d setting(s) forced, the screen no longer controls them: %s",
              env_override_count(), env_override_summary());
}

/* The wake-up re-lock: pushed ON TOP of whatever is showing, popping ITSELF
 * once opened. Shared by the Switch wake hook and the desktop diagnostic below,
 * so the two cannot drift apart. */
static void push_wake_lock()
{
    brls::Application::pushActivity(new LockActivity(
        [](bool ok) { if (ok) brls::Application::popActivity(); },
        ui::tr("lock/after_wake")));
}

#if SHADOW_HAVE_DESKTOP_GL   /* GLFW: desktop only - see gl_compat.h */
/* === AF2 2026-09-10 - SHADOW_DIAG_RELOCK_S: THE WAKE-UP LOCK ON DESKTOP ===
 *
 * DIAGNOSTIC, OFF by default. Pushes the wake-up lock once, N seconds after
 * launch, behind exactly the guards of the Switch wake hook. That hook exists
 * only on Switch, and with it every way to see what a re-lock does to a live
 * session - which is where AF2 lived. It can only ADD a lock, never open one.
 * Process configuration, read once: not session state. */
static double g_diag_relock_at = -2.0;   /* -2 unread, -1 off or done */

static void diag_relock_tick()
{
    if (g_diag_relock_at == -2.0) {
        const char *e = std::getenv("SHADOW_DIAG_RELOCK_S");
        const int s = e ? std::atoi(e) : 0;
        g_diag_relock_at = s > 0 ? glfwGetTime() + s : -1.0;
    }
    if (g_diag_relock_at < 0.0 || glfwGetTime() < g_diag_relock_at) return;
    g_diag_relock_at = -1.0;
    if (Settings::instance().lock_on_wake && !lock_activity_is_up() &&
        applock_configured()) {
        mnlog("[lock] DIAG re-lock (SHADOW_DIAG_RELOCK_S)");
        push_wake_lock();
    } else {
        mnlog("[lock] DIAG re-lock had no effect: no lock set, already "
              "shown, or re-lock disabled in the settings");
    }
}

/* === CFG-1 2026-09-11 - SHADOW_DIAG_USER_BITRATE=<s>:<mbps>: A LIVE CHOICE ===
 *
 * DIAGNOSTIC, OFF by default. Plays the user's mid-session bitrate choice with
 * nobody at the controls: <s> seconds after the session came up, once per
 * session, it calls the entry point the pause menu ends in
 * (ctrl_session_set_video_config, what Settings::applyBitrateLive() calls),
 * from this same UI thread. devlink parses `btn`/`nav` but nothing executes
 * them, so without this the case CFG-1 is about - G19 climbing back over the
 * user's choice - could not be replayed off console. The saved setting is not
 * touched. The variable is read once (process configuration); the per-session
 * part is reset whenever no session runs. */
static int    g_diag_ub_at_s  = -2;     /* -2 unread, -1 off */
static int    g_diag_ub_mbps  = 0;
static double g_diag_ub_since = -1.0;   /* when the session was seen up, -1 = none */
static bool   g_diag_ub_done  = false;

static void diag_user_bitrate_tick()
{
    if (g_diag_ub_at_s == -2) {
        const char *e = std::getenv("SHADOW_DIAG_USER_BITRATE");
        const char *colon = e ? std::strchr(e, ':') : nullptr;
        const int s = e ? std::atoi(e) : 0, m = colon ? std::atoi(colon + 1) : 0;
        g_diag_ub_at_s = (s > 0 && m > 0) ? s : -1;
        g_diag_ub_mbps = m;
    }
    if (g_diag_ub_at_s < 0) return;
    if (!ctrl_session_active()) { g_diag_ub_since = -1.0; g_diag_ub_done = false; return; }
    const double now = glfwGetTime();
    if (g_diag_ub_since < 0.0) g_diag_ub_since = now;
    if (g_diag_ub_done || now - g_diag_ub_since < g_diag_ub_at_s) return;
    g_diag_ub_done = true;
    mnlog("[DIAG] choix de debit simule : %d Mbps (SHADOW_DIAG_USER_BITRATE)", g_diag_ub_mbps);
    ctrl_session_set_video_config((uint32_t)g_diag_ub_mbps);
}
#endif

/* === RES-1 2026-09-26 - A DESKTOP BUILD FINDS ITS FILES BESIDE ITSELF ===
 *
 * Borealis reads its font, its translations and its images from
 * "./resources/", relative to the CURRENT directory (BRLS_RESOURCES_DIR "." in
 * its CMakeLists). Started from anywhere else - the CI artifact unpacked in a
 * download folder - the window opened with every menu drawn and not one
 * character of text: no font was found, and nothing said so. When the current
 * directory has no resources/ but the executable's directory does, move there
 * first. It runs before anything else because the Windows data directory
 * ("./halyard-data/") is relative too, and should follow.
 *
 * Consoles are untouched: their resources path is absolute (romfs:/, app0:).
 * SHADOW_RESOURCES_FROM_EXE=0 keeps the current directory; it is read from the
 * real environment only, since env.txt has not been read yet at this point. */
static std::string g_res1_moved_to;
#if (defined(__linux__) || defined(_WIN32)) && !defined(__SWITCH__) && !defined(__ANDROID__)
static void desktop_find_resources(void)
{
    const char *e = std::getenv("SHADOW_RESOURCES_FROM_EXE");
    if (e && std::atoi(e) == 0) return;
    struct stat st;
    if (stat("resources/font", &st) == 0) return;   /* already where Borealis looks */

    std::string dir;
#if defined(_WIN32)
    char *pgm = nullptr;
    if (_get_pgmptr(&pgm) != 0 || !pgm || !*pgm) return;
    dir = pgm;
    const size_t cut = dir.find_last_of("/\\");
#else
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    dir = exe;
    const size_t cut = dir.find_last_of('/');
#endif
    if (cut == std::string::npos) return;
    dir.resize(cut);
    if (stat((dir + "/resources/font").c_str(), &st) != 0) return;
#if defined(_WIN32)
    if (_chdir(dir.c_str()) == 0) g_res1_moved_to = dir;
#else
    if (chdir(dir.c_str()) == 0) g_res1_moved_to = dir;
#endif
}
#else
static void desktop_find_resources(void) {}
#endif

int main(int argc, char *argv[]) {
    /* RELOAD-1: FIRST, because it only reads `argv[0]` and everything that
     * follows may want to hand the console over. */
    devlink::rememberSelf(argc, argv);
    desktop_find_resources();   /* RES-1: before the data dir, which may be relative */

    // S38: the toggles BEFORE everything else - shadow_sockets_init already reads one.
    /* Before the env file and before the journal opens its log: both want the
     * directory to exist, and on a console it is the only interface there is. */
    shadow_ensure_data_dir();
    /* PROVE it, rather than assume it. A `mkdir` that fails silently is exactly
     * what cost the first console run: no directory, no log, no token, and
     * nothing anywhere saying why. The probe writes and removes one byte; its
     * verdict is printed as soon as there is somewhere to print it. */
    bool data_dir_writable = false;
    {
        char probe[280];
        std::snprintf(probe, sizeof(probe), "%s.writable", SHADOW_DATA_DIR);
        if (std::FILE *t = std::fopen(probe, "w")) {
            data_dir_writable = (std::fputc('1', t) != EOF);
            std::fclose(t);
            shadow_file_remove(probe);
        }
    }
    shadow_load_env_file();
    /* Right after, and before ANY `setenv` of ours: records which settings the
     * file has taken over, so the screens can say so instead of displaying a
     * value the session does not obey. Taken later it would find our own
     * writes and report every setting as forced. See env_override.h. */
    env_override_snapshot();

    // Init Winsock on Windows (a no-op elsewhere) BEFORE any network I/O (curl global init, etc.).
    shadow_sockets_init();


    // Always redirect the logs into SHADOW_DATA_DIR - on Switch there is no
    // console; on Linux/Windows the binary may be launched from one, so allow
    // SHADOW_LOG_TO_STDERR=1 to skip the fopen.
    FILE *logf = nullptr;
    const char *log_to_stderr = std::getenv("SHADOW_LOG_TO_STDERR");
    if (!log_to_stderr || std::strcmp(log_to_stderr, "1") != 0) {
        logf = std::fopen(SHADOW_DATA_DIR "borealis.log", "w");
    }
    if (logf) {
        // No buffering - every write flushes straight away, so a crash loses NOTHING.
        std::setvbuf(logf, nullptr, _IONBF, 0);
        /* The first line, before anything else can bury it: whether this
         * directory can be written at all. If it cannot, the token will not
         * persist, no toggle can be set and no log mirror can be armed - and
         * every one of those failures is otherwise silent. */
        std::fprintf(logf, "[data] %s : %s\n", SHADOW_DATA_DIR,
                     data_dir_writable ? "writable"
                                       : "NOT WRITABLE - no token, no env.txt, no logsink");
        brls::Logger::setLogOutput(logf);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

        /* S1 2026-08-22 - REDIRECT stderr INTO A FILE.
         * The whole REST layer (`shadow/http.c`) reports its failures with
         * `fprintf(stderr, ...)`, including the exact curl message. On Switch
         * there is no console: those lines went nowhere, and a network failure at
         * startup ("data centre unreachable") left NO usable trace. So we capture
         * them into a file beside the main log. */
        FILE *errf = std::freopen(SHADOW_DATA_DIR "stderr.log", "w", stderr);
        if (errf) std::setvbuf(errf, nullptr, _IONBF, 0);
    }

    /* S5 2026-08-22 - THIS BLOCK MUST STAY AFTER THE stderr REDIRECTION.
     * It used to sit before it, so its diagnostics ("CA bundle ...",
     * "sslInitialize FAIL") went nowhere, and we searched blind believing
     * sslInitialize was succeeding. */
#ifdef __SWITCH__
    /* === S2 2026-08-22 - INITIALISING HOS'S ssl: SERVICE ===
     *
     * Without it EVERY verified HTTPS request fails on Switch: "SSL peer
     * certificate or SSH remote key was not OK" (CURLE_PEER_FAILED_VERIFICATION)
     * right from the data-centre discovery (TINAG) - DNS and TCP went through,
     * only the certificate verification failed. devkitPro's libcurl is compiled
     * with the `Curl_ssl_libnx` backend, which delegates TLS to the console's
     * `ssl:` service (and therefore to its certificate store). But Borealis's
     * `userAppInit()` initialises socket/nifm/romfs... and NOT `ssl:`, and
     * nothing else opened it: the authority store was unreachable. The
     * connectivity check did pass, because it targets a literal IP with
     * verification DISABLED - it proved neither DNS nor TLS, hence a misleading
     * diagnosis ("DNS or connection blocked").
     *
     * We keep certificate verification ON: the REST path carries the OAuth
     * tokens, and disabling it would open the door to interception.
     * `sslInitialize` is refcounted by libnx, so it is safe if another component
     * calls it too. */
    {
        /* S5: is the authority bundle even READABLE? If the romfs is not
         * mounted or the file is missing, CURLOPT_CAINFO fails silently and we
         * wrongly conclude the root was refused. */
        FILE *caf = std::fopen("romfs:/cacert.pem", "rb");
        if (caf) {
            std::fseek(caf, 0, SEEK_END);
            long casz = std::ftell(caf);
            std::fclose(caf);
            std::fprintf(stderr, "CA bundle romfs:/cacert.pem OK (%ld octets)\n", casz);
        } else {
            std::fprintf(stderr, "CA bundle romfs:/cacert.pem INTROUVABLE\n");
        }

        Result rc = sslInitialize(4);   /* 4 sessions: REST + the dual SSE */
        if (R_FAILED(rc))
            std::fprintf(stderr, "sslInitialize FAIL rc=0x%x — HTTPS verifie va echouer\n",
                         (unsigned)rc);
    }
#endif

    /* S3: the build stamp (time + git hash) lets you check, console in hand,
     * WHICH binary is running - the version alone never moves.
     * S85: the version no longer comes from a "v0.3.0" written here. It was
     * frozen in this line, out of reach of the CMake that already numbers the
     * package: the first published 0.4.0 would have kept logging itself as
     * 0.3.0. See version.h. */
    brls::Logger::info("=== halyard v" SHADOW_VERSION " (borealis) — build {} ===",
                       SHADOW_BUILD_ID);

    /* S88 - the UI sounds. `init()` only reads the files that are PRESENT:
     * without them every call to `play()` is a no-op and the application behaves
     * exactly as before. */
    ui::sfx::init();
    std::fprintf(stderr, "halyard build %s\n", SHADOW_BUILD_ID);

    /* DEVL-5: and through the JOURNAL, which is the only path that reaches the
     * mirror and the session log. The two lines above go to Borealis' logger and
     * to stderr; on console stderr lands in a separate file nobody reads while
     * diagnosing, and the mirror never sees either. Every log pulled after the
     * fact now says which build produced it - which is worth more than the
     * command, since it costs nothing to have already. */
    if (!g_res1_moved_to.empty())
        mnlog("[RES-1] no resources/ in the working directory; using the executable's: %s",
              g_res1_moved_to.c_str());
    mnlog("[version] %s v%s — nro %s", SHADOW_BUILD_ID, SHADOW_VERSION,
          devlink::selfPath().empty() ? "(hors hbloader)"
                                      : devlink::selfPath().c_str());

    /* The crypto bench, right after the version line: that is where someone
     * reading the log looks for "which binary, and is it healthy". It runs only
     * when env.txt asks for it, and it opens no connection. */
    /* The profile BEFORE the bench: otherwise it would measure a throttled
     * console, and its figures would describe nothing that runs afterwards. */
    shadow_power_profile_apply();
    (void)shadow_crypto_selftest();

    // Load the persisted preferences (stats overlay toggle, etc.).
    Settings::instance().load();

    /* The settings that drive C code through environment variables are set
     * HERE, right after the load and BEFORE any read: those `getenv` calls are
     * cached on their first invocation, and setting the variable later would have
     * no effect - a setting you believe is applied and is not is worse than no
     * setting at all. `env.txt` keeps priority. */
    Settings::instance().applyToggles();

    /* UI language. To be set BEFORE Application::init(): the locale is frozen
     * when the platform is created, and the catalogues are loaded right
     * after. */
    if (!Settings::instance().language.empty()) {
        brls::Platform::APP_LOCALE_DEFAULT = Settings::instance().language;
    }
#if !defined(__SWITCH__)
    else if (!getenv("BOREALIS_LANG")) {
        /* On Switch, "automatic" follows the console's language. On desktop,
         * Borealis looks ONLY at BOREALIS_LANG and otherwise falls back to en-US,
         * ignoring the system language: the UI then came out in English for the
         * translated text and in French for what is not translated yet. So we
         * read the POSIX locale ourselves. */
        const char *env = getenv("LC_ALL");
        if (!env || !*env) env = getenv("LC_MESSAGES");
        if (!env || !*env) env = getenv("LANG");
        if (env && *env && strncmp(env, "C", 2) != 0 && strncmp(env, "POSIX", 5) != 0) {
            std::string lang, region;
            for (const char *c = env; *c && *c != '.' && *c != '@'; c++) {
                if (*c == '_' || *c == '-') { region = ""; continue; }
                (region.empty() && lang.size() < 2 ? lang : region) += *c;
            }
            /* Codes Borealis expects: "fr", "en-US", "pt-BR", "zh-Hans". */
            std::string locale = lang;
            if (lang == "en")                        locale = "en-US";
            else if (lang == "pt" && region == "BR") locale = "pt-BR";
            else if (lang == "zh")                   locale = "zh-Hans";
            if (!locale.empty()) {
                brls::Platform::APP_LOCALE_DEFAULT = locale;
                brls::Logger::info("Locale systeme detectee : {} -> {}", env, locale);
            }
        }
    }
#endif

    // NB: no wolfSSL_Init -> wolfSSL_Cleanup sequence at startup (the old QUIC
    // smoke test did one): it broke the state shared with mbedTLS/curl, and
    // curl_global_init() then failed on the first TINAG call.

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        if (logf) std::fclose(logf);
        return EXIT_FAILURE;
    }
    brls::Logger::info("Application::init OK");

    /* The window's title is also the name the desktop matches it by - with
     * "Shadow PC" here, a Linux desktop that has the official client installed
     * drew THEIR logo on our window. A leftover of the 2026-09-13 rename,
     * found 2026-09-26. */
    brls::Application::createWindow(SHADOW_APP_NAME);
    brls::Logger::info("createWindow OK");

    /* The dev machine can now drive the runs through the same socket that
     * receives the log - no more quitting the application, launching ftpd and
     * dropping a file just to change three numbers. */
    journal_set_command_handler(shadow_command_handler);

    /* AUTH-1: and the gate that protects that very channel. The journal owns
     * the socket, so it is the journal that must not send a line before the
     * owner has said yes -- but WHO is allowed is decided by a prompt on
     * screen, which is this layer's business, not the logger's. Installed
     * beside the handler because the two are the same channel: with no gate,
     * the journal keeps the mirror closed and says so once. */
    journal_set_mirror_gate(devlink_journal_gate());

    /* S38: finally say, in the network log, which toggles are active. */
    shadow_log_env_file();

    /* TEMPORARY DIAGNOSTIC (2026-08-25) - the "a space around every accent"
     * bug. Everything I could measure offline is correct: catalogues in clean
     * UTF-8, `fmt` transparent, identical glyph advance with and without the
     * accent, loaded font verified. What remains is to know what the string
     * contains JUST BEFORE display. To be removed once the cause is found. */
    {
        /* The REAL width as the engine computes it, with the font actually
         * loaded. If "Réessayer" is wider than "Reessayer" by more than the e/é
         * difference, the accented glyph comes from a monospaced fallback font
         * (the Chinese one, chained by Borealis): the drawing keeps its size but
         * occupies a double cell, which reads as a space on each side. */
        {
            NVGcontext *vg = brls::Application::getNVGContext();
            if (vg) {
                nvgFontSize(vg, 24.0f);
                nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
                float b[4];
                float plain    = nvgTextBounds(vg, 0, 0, "Reessayer", NULL, b);
                float accented = nvgTextBounds(vg, 0, 0, "R\xc3\xa9""essayer", NULL, b);
                float e    = nvgTextBounds(vg, 0, 0, "e", NULL, b);
                float ea   = nvgTextBounds(vg, 0, 0, "\xc3\xa9", NULL, b);
                brls::Logger::info("[ACCENT] widths: Reessayer={:.1f} Réessayer={:.1f} "
                                   "(gap {:.1f}) | e={:.1f} é={:.1f} (gap {:.1f})",
                                   plain, accented, accented - plain, e, ea, ea - e);
            }
        }

        const char *keys[] = { "action/retry", "settings/title", "vm/datacenter" };
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            std::string v = ui::tr(keys[i]);
            std::string hex; char b[8];
            for (unsigned char c : v) { snprintf(b, sizeof(b), "%02x ", c); hex += b; }
            brls::Logger::info("[ACCENT] {} = \"{}\" | {}", keys[i], v, hex);
        }
    }
    brls::Application::setGlobalQuit(true);

    /* === 2026-09-02 - THE APPLICATION LOCK, BEFORE ANYTHING ELSE ===
     *
     * Pushed BEFORE BootActivity, so the lock stands in front of the OAuth
     * refresh and not behind it: the point is that nothing of the account is
     * touched - not even a token refresh - until whoever is holding the console
     * has proved they may.
     *
     * Borealis has no `swapActivity`, so the lock is left UNDERNEATH, exactly as
     * BootActivity is left underneath the VM list. `popActivity` refuses to
     * empty the stack, so nothing can surface it by accident.
     *
     * `applock_store_load` returns 0 for an absent file, an unreadable one, or a
     * record with no usable method - so the ordinary case, no lock configured,
     * costs one failed `fopen` and behaves exactly as before. */
    if (applock_configured()) {
        mnlog("[lock] locked at startup");
        brls::Application::pushActivity(new LockActivity([](bool ok) {
            if (ok) brls::Application::pushActivity(new BootActivity());
        }));
    } else {
        brls::Application::pushActivity(new BootActivity());
    }
    brls::Logger::info("pushActivity BootActivity OK, entering mainLoop");

    /* UX6 B3 2026-05-18 — Switch sleep/wake detection.
     * appletHookEnter() registers a FocusState callback -> when the console
     * sleeps we signal abort for a clean disconnect (= avoids the ZBC freeze and
     * hbloader dangling handles on wake). */
#ifdef __SWITCH__
    /* D14 2026-08-21 - Switch build RESTORED. This block redeclared appletHook
     * and appletGetFocusState by hand, in C++ and without extern "C": the symbols
     * therefore came out mangled (`appletHook(void**, void(*)(int,void*),
     * void*)`) and resolved against no C symbol in libnx - a link error. The
     * local AppletHookType typedef also shadowed libnx's own. We include
     * <switch.h> (it was not included here) and use its types. */
    static AppletHookCookie g_hook_cookie;
    auto sleep_cb = [](AppletHookType hook, void *param) {
        (void)param;
        if (hook == AppletHookType_OnFocusState) {
            AppletFocusState state = appletGetFocusState();
            /* Focused = console active; everything else = sleep or menu. */
            if (state != AppletFocusState_InFocus) {
                /* === 2026-08-27 - THIS IS NOT ONLY SLEEP ===
                 * `AppletFocusState != InFocus` also covers opening the HOME
                 * menu. Cutting unconditionally therefore meant that pressing
                 * HOME KILLED the session - a defensible behaviour (HOS is brutal
                 * with backgrounded applications) but invisible and not
                 * configurable, hence surprising.
                 * The setting makes the choice explicit. The default stays "cut",
                 * because keeping the session is a BET: nothing guarantees HOS
                 * will let us live, nor that the server will wait for us. */
                if (Settings::instance().cut_when_unfocused) {
                    mnlog("[focus] focus lost (sleep or HOME) - stop requested");
                    ShadowApp::instance().signalAbort();
                } else {
                    mnlog("[focus] focus lost - TRYING to keep the session");
                }
            } else {
                /* === 2026-09-02 - RE-ARMING ON WAKE, WITHOUT WHICH THE LOCK IS
                 * THEATRE ===
                 *
                 * Borealis calls `appletSetFocusHandlingMode(NoSuspend)`
                 * (switch_platform.cpp:112), so the process is NOT suspended
                 * when it loses focus: same process, same main loop, across
                 * HOME and across sleep. A lock that only gated startup would
                 * therefore be passed once and then never asked again for the
                 * whole life of the process - HOME out, HOME in, and you are
                 * inside somebody else's session.
                 *
                 * This branch is the one the application never had: before
                 * today `AppletFocusState_InFocus` appeared nowhere in
                 * the tree except in the test just above.
                 *
                 * Pushed ON TOP of whatever is showing, and pops ITSELF once
                 * opened - which puts the user back exactly where they were,
                 * mid-stream included. The guard matters: opening the HOME menu
                 * and closing it fires this same event, so without it a second
                 * lock would stack on the first. */
                if (Settings::instance().lock_on_wake &&
                    !lock_activity_is_up() &&
                    applock_configured()) {
                    mnlog("[lock] wake - re-locking");
                    brls::Threading::sync([]() { push_wake_lock(); });
                }
            }
        }
    };
    appletHook(&g_hook_cookie, sleep_cb, nullptr);
#endif

    /* `onFrame()` is the only place a screenshot can happen: this thread owns
     * the OpenGL context, and `glReadPixels` only means something there. It
     * serves at most ONE request per frame and drains only a bounded number of
     * lines of the capture in progress - emitting the ~500 lines of a 720p
     * capture in one go means that many blocking sends inside a single frame,
     * i.e. a freeze we would then blame on the change we are measuring.
     * Without `logsink.txt`, the call returns immediately. */
    /* === AF3 2026-09-10 - AT EXIT, REQUESTS IN FLIGHT BECOME ABANDONABLE ===
     *
     * `Application::exit()` fires this event BEFORE `Threading::stop()` joins
     * the thread that runs our tasks (application.cpp: exitEvent.fire(), then
     * Threading::stop()). That join is where a stuck boot request held the exit
     * for up to 30 s per request. Every platform and every way out goes through
     * `exit()` - the window's cross, HOME, Borealis' own quit - while "Rejouer"
     * does NOT, so a restart keeps its network. See shadow/http.c. */
    brls::Application::getExitEvent()->subscribe([]() { http_request_shutdown(); });

#if SHADOW_HAVE_DESKTOP_GL   /* GLFW: desktop only - see gl_compat.h */
    /* === UX10 2026-09-10 - HOLD THE WINDOW TO SHOW THE SHUTDOWN ===
     *
     * Measured 2026-09-10: window closed mid-session, the process lived on for
     * ~3 s - unregistering the eight streams, joining the threads, then TWO
     * requests deleting our clients on the server so that no ghost session is
     * left on the VM. That work is useful; it was invisible, because the window
     * went BEFORE it: `mainLoop` returns, and all the cleanup happens
     * afterwards, with no surface to show itself on.
     *
     * So the cross is intercepted, the immediate close CANCELLED, and the loop
     * left running: the window stays, the shutdown screen shows in it, and we
     * quit for good once no task is left.
     *
     * Borealis' GLFW callback is not chained: there is none on this event
     * (Borealis reads `glfwWindowShouldClose` in its loop), so nothing is
     * deprived of its event here. */
    if (GLFWwindow *win = glfwGetCurrentContext()) {
        glfwSetWindowCloseCallback(win, [](GLFWwindow *w) {
            glfwSetWindowShouldClose(w, GLFW_FALSE);
            if (!ui::shutdown::begin(glfwGetTime())) return;   /* deja en cours */
            ui::shutdown::setStep(ui::tr("shutdown/streams").c_str());
            ShadowApp::instance().signalAbort();
            brls::Application::pushActivity(new ShutdownActivity());
            mnlog("[UX10] shutdown requested - screen shown, %d task(s) in flight",
                  ui::shutdown::pending());
        });
    }
#endif

    /* === UI7 2026-09-13 - HOW LONG IS A UI FRAME, AND WHERE =============
     *
     * Reported from a PS Vita: the interface slows down on entering the
     * settings, more inside a page, more again inside a sub-page. Three
     * hypotheses were formed and all three were wrong on inspection - the
     * activity stack does NOT draw below an opaque view, the settings rebuild
     * does NOT run per frame, and the font change made the atlas smaller, not
     * bigger.
     *
     * So this measures instead of guessing, the same way `[L5]` settled the
     * video path. It times the whole frame and reports the distribution every
     * five seconds WITH the depth of the activity stack, which is the variable
     * the report points at. A mean is useless here: a slowdown that is felt is
     * a p99, and a stack that costs nothing at depth 1 and 40 ms at depth 3
     * says exactly where to look.
     *
     * `SHADOW_UI_FRAME_MS=1` in env.txt turns it on; it is off by default,
     * because a log line every five seconds is noise once the answer is
     * known. */
    static int g_ui_frame = -1;
    if (g_ui_frame < 0) {
        const char *e = getenv("SHADOW_UI_FRAME_MS");
        g_ui_frame = e ? atoi(e) : 0;
    }
    double ui_t0 = 0.0, ui_win = 0.0;
    unsigned ui_n = 0;
    double ui_max = 0.0;
    std::vector<double> ui_samples;
    if (g_ui_frame) ui_samples.reserve(512);
    auto ui_now_ms = []() -> double {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    };
    if (g_ui_frame) { ui_t0 = ui_now_ms(); ui_win = ui_t0; }

    double ui_after_loop = 0.0;
    double ui_ours = 0.0;            /* time spent in OUR per-frame calls */
    while (true) {
        const double ui_loop_a = g_ui_frame ? ui_now_ms() : 0.0;
        const bool ui_go = brls::Application::mainLoop();
        const double ui_loop_b = g_ui_frame ? ui_now_ms() : 0.0;
        if (!ui_go) break;
        if (g_ui_frame) {
            const double now = ui_loop_b;
            const double dt  = now - ui_t0;
            ui_t0 = now;
            ui_after_loop += (ui_loop_b - ui_loop_a);   /* Borealis' own frame */
            if (ui_n++) {                      /* skip the first, it spans startup */
                ui_samples.push_back(dt);
                if (dt > ui_max) ui_max = dt;
            }
            if (now - ui_win >= 5000.0 && ui_samples.size() > 8) {
                std::sort(ui_samples.begin(), ui_samples.end());
                const size_t n = ui_samples.size();
                /* WHERE the time goes, not just how much. `borealis` is
                 * everything inside `mainLoop()` - input, layout, draw, swap;
                 * `nous` is what this loop adds on top. And the name of the
                 * activity on top, because the report says the cost grows with
                 * depth and a depth without a name is not actionable. */
                const auto st = brls::Application::getActivitiesStack();
                const char *top = "?";
                if (!st.empty() && st.back()) top = typeid(*st.back()).name();
                /* HOW MANY VIEWS BOREALIS ACTUALLY DRAWS. It walks the stack
                 * from the top and stops at the first activity that is not
                 * translucent - and `isTranslucent()` is `fadeIn ||
                 * inFadeAnimation`, so an activity whose fade never completes
                 * keeps everything under it being drawn, every frame. That is
                 * the difference between a 3-row page costing 33 ms and the
                 * same widget costing 153. Counting it settles the question
                 * instead of arguing about it. */
                size_t drawn = 0;
                for (size_t d = 0; d < st.size(); d++) {
                    brls::Activity *a = st[st.size() - 1 - d];
                    drawn++;
                    if (a && !a->isTranslucent()) break;
                }
                mnlog("[UI7] frame n=%zu p50=%.1f p90=%.1f p99=%.1f pire=%.1f ms "
                      "-> %.0f img/s | borealis %.1f ms/img, nous %.1f | "
                      "pile=%zu dessinees=%zu sommet=%s",
                      n, ui_samples[n / 2], ui_samples[n * 90 / 100],
                      ui_samples[n * 99 / 100], ui_max,
                      1000.0 / (ui_samples[n / 2] > 0.01 ? ui_samples[n / 2] : 0.01),
                      ui_after_loop / (double)n, ui_ours / (double)n,
                      st.size(), drawn, top);
                ui_samples.clear(); ui_max = 0.0; ui_win = now;
                ui_after_loop = 0.0; ui_ours = 0.0;
            }
        }
        const double ui_ours_a = g_ui_frame ? ui_now_ms() : 0.0;
        devlink::onFrame();
        askDevlinkAuthorisation();
#if SHADOW_HAVE_DESKTOP_GL   /* GLFW: desktop only - see gl_compat.h */
        diag_relock_tick();   /* AF2 - SHADOW_DIAG_RELOCK_S, off by default */
        diag_user_bitrate_tick();   /* CFG-1 - SHADOW_DIAG_USER_BITRATE, off by default */
        if (ui::shutdown::active()) {
            const double dt = ui::shutdown::elapsed(glfwGetTime());
            /* The ceiling is a SAFETY NET, not the mechanism: a task that
             * never withdraws must not hold the application for ever. 12 s is
             * far above a clean shutdown (0.8 s measured, UX11) and above the
             * 1.5 s grace AF3 gives requests at exit, yet well below the
             * minute two stuck 30 s requests used to cost: past it, better to
             * leave and say so. */
            if (ui::shutdown::finished() || dt > 12.0) {
                mnlog("[UX10] shutdown - %s after %.1f s",
                      ui::shutdown::finished() ? "termine" : "PLAFOND ATTEINT", dt);
                brls::Application::quit();
            } else if (dt > 5.0) {
                ui::shutdown::setStep(ui::tr("shutdown/slow").c_str());
            }
        }
#endif
            if (g_ui_frame) ui_ours += ui_now_ms() - ui_ours_a;
    }

    // Explicit cleanup of the global subsystems. Each one releases its HOS
    // handles (sockets, TLS, etc.) so nx-hbloader does not inherit dangling
    // handles when our NRO is unloaded (the cause of the hbloader post-exit
    // crashes we kept observing).
    /* S24 2026-08-22 - EXIT MILESTONES.
     * A crash was reported when quitting from the VM list AFTER a streaming
     * session: the main log stops on "delete AppletFrame" and says nothing more,
     * because it is closed right after. So we write markers into stderr.log
     * (always open, unbuffered) at every teardown step: the last line present
     * names the faulty step. */
    std::fprintf(stderr, "[S24] exit: mainLoop finished\n");
    telemetry_stop();          // joins the background poster
    std::fprintf(stderr, "[S24] exit: telemetry_stop OK\n");
    http_global_cleanup();     // curl
    std::fprintf(stderr, "[S24] exit: http_global_cleanup OK\n");
    shadow_sockets_shutdown(); // WSACleanup on Windows
    std::fprintf(stderr, "[S24] exit: sockets OK\n");

    /* === S29 2026-08-22 - STOPPING THE LOG DRAIN THREAD ===
     *
     * `journal_close()` was only called from `webrtc/webrtc.c`, a file
     * EXCLUDED from the Switch build (the legacy WebRTC path). On console it
     * therefore never ran: the drain thread stayed alive until `_exit(0)` killed
     * it brutally, while it could be writing to the SD card, and the file was
     * never closed.
     *
     * Two symptoms explained at once:
     *   - a systematic crash when hbloader unloaded the NRO, but ONLY after a
     *     session - because that thread starts on the first logged line, i.e.
     *     when the stream starts (no session: no thread, no crash, which the
     *     measurement confirmed);
     *   - the end of the log systematically truncated (the buffer was never
     *     emptied), which deprived us of exactly the last diagnostic lines.
     *
     * The thread polls its abort flag every 100 ms: the join is immediate. This
     * is exactly the project's rule (KB §7.3): every long-lived thread must be
     * stopped cleanly before exit. */
    ui::sfx::close();   /* S88: before HOS kills the thread */
    journal_close();
    std::fprintf(stderr, "[S24] exit: journal closed OK\n");

#ifdef __SWITCH__
    /* S26 2026-08-22 - RELEASE THE ssl: SESSION HERE, with the other subsystems.
     * I had put this call just before `_exit(0)` (S2), then removed it (S24)
     * believing it was responsible for the exit crash. The markers showed it was
     * not: our whole teardown completes, "before _exit(0)" is printed. But leaving
     * it out is a real hole: with hbloader the NRO is loaded INSIDE the loader's
     * process, so an unreleased handle stays on its hands - which is exactly the
     * reason this cleanup block exists (cf. the comment above: "so nx-hbloader
     * does not inherit dangling handles"). Its place is therefore here, after
     * curl and the sockets, and not in the unstable end zone. */
    sslExit();
    std::fprintf(stderr, "[S24] exit: sslExit OK\n");
    device::releaseServices();   /* AF10 - the nifm session device_mode opened */
    std::fprintf(stderr, "[S24] exit: nifm OK\n");
#endif

    // Disable the logger BEFORE the static destructors (Logger::log -> fmt::
    // localtime can crash during teardown if the stdio runtime is already partly
    // gone).
    brls::Logger::setLogLevel((brls::LogLevel)-1);
    brls::Logger::setLogOutput(nullptr);
    if (logf) std::fclose(logf);

#ifdef __SWITCH__
    /* SUPERSEDED - noted by the 2026-09-10 shutdown audit. S26, just above,
     * puts sslExit() BACK: this block describes a removal that was reverted.
     * Its claim that `_exit` makes HOS reclaim every handle is also disputed by
     * S26 (hbloader keeps what the NRO did not give back) and has never been
     * measured; the audit left it open - relaunch the NRO N times from hbmenu
     * and watch for the first failing *Initialize. Kept for the ledger. */
    /* S24 2026-08-22 - `sslExit()` REMOVED from the exit path.
     * I had added it (S2) out of hygiene, but it falls in the zone this file
     * deliberately avoids: the comment below recalls that the
     * FFmpeg/libnx/Borealis teardown order is unstable, hence the `_exit(0)` that
     * skips EVERYTHING else. Releasing a service session right before that, while
     * other components may still use it, is exactly the kind of call that crashes
     * there. And it is pointless: `_exit` terminates the process, and the HOS
     * kernel reclaims the handles it owns - the leak I wanted to avoid concerns
     * THREADS killed brutally, not service sessions.
     * `SHADOW_SSL_EXIT=1` restores it for comparison. */
    std::fprintf(stderr, "[S24] exit: before _exit(0)\n");
#endif

    // _exit(0) skips atexit and the C++ static destructors. On Switch homebrew
    // the FFmpeg/libnx/Borealis teardown order is unstable and sporadically
    // crashes nx-hbloader. _exit makes a direct syscall.
    _exit(0);
    return EXIT_SUCCESS;  // unreachable
}
