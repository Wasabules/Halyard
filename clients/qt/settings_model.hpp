/* settings_model - the settings as DATA, so the window is generated from it.
 *
 * === QT3 2026-10-03 — ONE TABLE, AND WHY ===================================
 *
 * Core is configured by 258 `SHADOW_*` environment variables. A settings
 * window could be thirty hand-written widgets; the Borealis client started that
 * way and the result was four defects in one screen, all of them from the UI
 * owning the knowledge (a toggle that showed a sentence instead of its state, a
 * translation that never applied, text that overflowed, and a crash on going
 * back). So the knowledge lives HERE, in one array, and the window is
 * generated from it. Adding a setting is a row in the array. Nothing else.
 *
 * === QT4 2026-10-03 — THE FULL SET, EVERY ROW READ OUT OF CORE =============
 *
 * The first version held ten rows, all added during the session that built the
 * window, and was reported as "only the things we added recently and not at all
 * the complete list available on Borealis". It now covers Borealis's nine
 * sections (`settings/sec_...`) and four sub-screens (quality, pad, eq,
 * gestures), minus what cannot mean anything on a desktop, plus what only a
 * desktop has.
 *
 * Growing it surfaced a worse problem: descriptions in the first draft were
 * written from the variable's NAME, and the name lied. Each row was then
 * checked against its `getenv` site, and these were wrong:
 *
 *   - `SHADOW_JOURNAL_NIVEAU` was offered as Debug/Info/Warning/Error = 0..3.
 *     journal.h is ERROR=0 .. TRACE=4, so choosing "Debug" wrote ERROR - the
 *     setting would have silenced the log it was meant to open up;
 *   - `SHADOW_CURL_VERBOSE` is tested with `getenv(...) != NULL`: ANY value
 *     enables it, "0" included. Its "off" must unset, never write 0;
 *   - `SHADOW_INPUT_TCP` defaults to ON; the draft described it as opt-in;
 *   - `SHADOW_ERR_CONCEAL` defaults to 0 because concealment was MEASURED to
 *     invent macroblocks; the draft said the opposite;
 *   - `SHADOW_JOURNAL_CATEGORIES` is a numeric BITMASK (strtoul, base 0), not
 *     a list of names;
 *   - `SHADOW_INPUT_WHEEL`, `SHADOW_HID_F3`, `SHADOW_INPUT_KEY_HOLD_MS` and
 *     `SHADOW_PROBE_PORTS` are campaign instruments (a synthetic wheel burst, a
 *     byte-exactness field, a test key's hold, a cleartext port probe), not the
 *     settings their names suggest. Removed;
 *   - Borealis's "4:4:4" row writes `SHADOW_REG_F5`, not
 *     `SHADOW_HIGH_COLOR_FIDELITY`.
 *
 * Rule from now on: a description states what the read site or its comment
 * establishes, cites the campaign where there is one, and says "not measured"
 * where nothing was. A plausible sentence is not a source.
 *
 * === QT5 2026-10-03 — READABLE WITHOUT KNOWING CORE =========================
 *
 * Reported next, and right: "no default values, or numbers to type; many
 * settings should already have something selected; instead of 0/1/2/3, a
 * drop-down; and a category within the category". Four changes follow:
 *
 *   - `def` records what core does when the variable is ABSENT, read out of
 *     each `getenv` site, and the window shows it: a default-ON toggle is drawn
 *     checked, a choice preselects its default and labels it "(default)", a
 *     number shows its default value. Drawing absent as unchecked or as 0 was a
 *     lie that the first click then "fixed" by writing the opposite;
 *   - numbers that are really a handful of sensible values (frame rate,
 *     bitrate, requested height, clipboard size, concealment) are CHOICES with
 *     presets now, and the bitmask is a set of checkboxes (`Kind::Flags`). A
 *     raw number survives only where it is a genuine quantity with a unit;
 *   - `group` splits each page into sections (Video: Quality / Adaptation /
 *     Decoding / Experimental), so a page reads as a structure, not a list;
 *   - `unit` moves "ms", "%" out of the label and into the control.
 *
 * Rows use C++20 designated initializers so each field is named where it is
 * set: a positional row of fifteen fields is how `def` and `unsetValue` end up
 * swapped without anyone seeing it.
 *
 * Deliberately absent, so nobody looks for them: the Vita's rear touchpad and
 * gesture table, the equaliser's docked/handheld profiles, the lock screen and
 * `ui_scale` - console concepts with nothing to mean on a desktop. The
 * interface LANGUAGE is not here either, but it is not absent: it is a client
 * preference, not a core variable, and lives on the window's "General" page
 * (see i18n.hpp). The ~190 remaining variables are campaign instruments
 * (`SHADOW_DIAG_`, `SHADOW_DUMP_`, `SHADOW_PARITY_`, `SHADOW_REG_F`...); they
 * belong in `env.txt`, which still overrides every row below.
 *
 * === FIVE FACTS A SETTINGS WINDOW MUST RESPECT =============================
 *
 * 1. MOST SETTINGS TAKE EFFECT ON THE NEXT SESSION: read with `getenv` at
 *    first use and cached. `appliesLive` says which are not.
 * 2. `env.txt` WINS: `env_override_active()` names variables set from
 *    outside; such a row is shown as taken over and cannot be edited.
 * 3. ABSENT IS NOT ZERO: `unsetValue` names the value that unsets rather than
 *    writes; an EMPTY entry in `values` does the same for a choice; a number
 *    whose default is unknown gets an "automatic" position below `min`.
 * 4. THE VALUE IS NOT ALWAYS THE INDEX: `SHADOW_CURSOR_MODE` takes words and
 *    refuses "0"; `SHADOW_AUDIO_CODEC` starts at 1. `values` carries the
 *    literal per choice.
 * 5. WHAT ABSENT MEANS IS CORE'S DECISION, NOT THE WINDOW'S: `def`.
 *
 * === TRANSLATION ===========================================================
 *
 * User-facing strings are wrapped in `QT_TRANSLATE_NOOP("Settings", ...)`. The
 * macro expands to the bare literal, so the table stays plain data (and the
 * test, which links only Qt6Core, still reads it); it is a marker `lupdate`
 * extracts into `clients/qt/i18n/halyard_*.ts`. The window translates at
 * display time. Variable names and `values` are never translated: they are
 * identifiers, and they are what the log prints.
 *
 * Pure: no Qt widgets, no I/O, no getenv. Tested by tests/test_qt_settings.cpp.
 */
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

namespace halyard {

struct Setting {
    enum class Kind { Toggle, Choice, Number, Text, Flags };

    const char *env = nullptr;      /* the variable - the only real identity */
    const char *page = nullptr;     /* rail entry (translatable) */
    const char *group = nullptr;    /* section within the page (translatable) */
    const char *label = nullptr;    /* translatable */
    const char *description = nullptr;  /* what it COSTS (translatable) */
    Kind kind = Kind::Toggle;
    /* Choice: the labels, in order. Flags: one label per bit, bit 0 first. */
    QStringList choices = {};
    /* Choice only, optional: the literal written per choice, parallel to
     * `choices`. Empty list = the index is the value. An empty STRING at
     * position i = that choice unsets the variable ("Automatic"). */
    QStringList values = {};
    /* Number only: shown in the control, not the label (translatable). */
    const char *unit = nullptr;
    int min = 0, max = 0;           /* Number only */
    /* Toggle/Number/plain Choice: the value that unsets rather than writes. */
    int unsetValue = -1;
    /* What core does when absent, in this row's terms (0/1, an index, a value,
     * a mask), or -1 when it is not a fixed value. Fact 5. */
    int def = -1;
    bool appliesLive = false;       /* core re-reads it during a session */
    bool isPath = false;            /* Text only: offer a file picker */
};

inline const QVector<Setting> &settings()
{
    using K = Setting::Kind;
    static const QVector<Setting> kTable = {

    /* ================================================== Connection ======= */

    { .env = "SHADOW_FORCE_IPV4",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Network"),
      .label = QT_TRANSLATE_NOOP("Settings", "IPv4 only"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Dial only IPv4. On a network where one family is a black hole, half "
        "of every retry round is spent waiting on an address that will never "
        "answer - measured at 8.6 s lost on one start-up, where the working "
        "family answered in 22 ms. Forcing the WRONG one prevents any "
        "connection: it has already stopped the control channel from opening "
        "(KB 2026-08-25)."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_FORCE_IPV6",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Network"),
      .label = QT_TRANSLATE_NOOP("Settings", "IPv6 only"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The same, the other way. Consulted only when IPv4 only is off, so the "
        "two cannot contradict each other."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_RESOLVE_ONCE",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Network"),
      .label = QT_TRANSLATE_NOOP("Settings", "Resolve the VM address once"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The VM's first resolved address is reused by every channel instead of "
        "each one resolving again. An experiment switch; the difference has "
        "not been measured."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_VIDEO_NET_TCP",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Transport"),
      .label = QT_TRANSLATE_NOOP("Settings", "Video over TCP"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Reliability instead of latency - and it costs the adaptive bitrate: "
        "in TCP the server refuses EnableDynamicBitrate, so the picture keeps "
        "the rate it started with."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_INPUT_TCP",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Transport"),
      .label = QT_TRANSLATE_NOOP("Settings", "Input over TCP"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Keyboard and mouse travel on the TCP input channel. Off selects the "
        "other transport, which has not been the measured path."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_UDP_REG_13",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Transport"),
      .label = QT_TRANSLATE_NOOP("Settings", "Cleartext registration on port +13"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Off for a reason (K9 2026-08-21): this 25-byte packet is OUR OWN "
        "addition - the official client never sends it and only emits "
        "encrypted packets on that port. The suspicion is that it invalidates "
        "the input channel server-side."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_CA_BUNDLED",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Certificates"),
      .label = QT_TRANSLATE_NOOP("Settings", "Use the shipped certificate store"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Requests are verified against resources/cacert.pem. Off uses the "
        "library's own store - which on a Windows build whose DLLs sit beside "
        "the binary is wrong: MSYS2's libcurl derives its CA path from its own "
        "DLL's directory. That is what produced \"data centre unreachable\" at "
        "boot (WIN2)."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_CA_FILE",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Certificates"),
      .label = QT_TRANSLATE_NOOP("Settings", "Certificate store file"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "An explicit PEM bundle, taking precedence over the setting above. A "
        "path that does not exist fails every request with a certificate "
        "error, not with a file error."),
      .kind = K::Text, .isPath = true },

    { .env = "SHADOW_CHAN_RETRIES",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Reconnection"),
      .label = QT_TRANSLATE_NOOP("Settings", "Channel open attempts"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "How many times a media channel is reopened before the session gives "
        "up. Each attempt costs the delay below, so a large count turns a dead "
        "port into a long, silent wait."),
      .kind = K::Number, .min = 0, .max = 20, .def = 4 },

    { .env = "SHADOW_CHAN_RETRY_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Reconnection"),
      .label = QT_TRANSLATE_NOOP("Settings", "Delay between attempts"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Paid once per failed attempt, so the worst case is this times the "
        "attempt count. Too short and a port that is merely slow to bind is "
        "declared dead."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 1, .max = 5000, .def = 250 },

    { .env = "SHADOW_SSE_RECONNECT",
      .page = QT_TRANSLATE_NOOP("Settings", "Connection"),
      .group = QT_TRANSLATE_NOOP("Settings", "Reconnection"),
      .label = QT_TRANSLATE_NOOP("Settings", "Reconnect the event stream"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The SSE stream holds the server-side binding (KB 3.5), so its silent "
        "death means teardown. Off returns to the one-shot behaviour: one "
        "network blip on that stream and the session ends."),
      .kind = K::Toggle, .def = 1 },

    /* ======================================================= Video ======= */

    { .env = "SHADOW_CODEC",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Quality"),
      .label = QT_TRANSLATE_NOOP("Settings", "Codec"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Sent as the request's wire value (K13). A codec this machine's "
        "libavcodec lacks falls back to H.264 with a line in the log, so the "
        "request and the decoder can disagree: the log says what is really "
        "decoding."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "H.264"),
                   QT_TRANSLATE_NOOP("Settings", "HEVC (H.265)"),
                   QT_TRANSLATE_NOOP("Settings", "AV1") },
      .def = 0 },

    { .env = "SHADOW_BITRATE_MBPS",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Quality"),
      .label = QT_TRANSLATE_NOOP("Settings", "Bitrate"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Automatic uses the session's own value. Also re-read when the "
        "adaptation applies a new rate, so it caps that too."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   "5 Mb/s", "10 Mb/s", "15 Mb/s", "20 Mb/s", "30 Mb/s",
                   "40 Mb/s", "50 Mb/s", "75 Mb/s", "100 Mb/s" },
      .values = { QString(), "5", "10", "15", "20", "30", "40", "50", "75",
                  "100" } },

    { .env = "SHADOW_FPS",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Quality"),
      .label = QT_TRANSLATE_NOOP("Settings", "Frame rate"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Automatic uses the session's own value. Above the display's refresh "
        "rate it costs decode time and bandwidth for frames nothing shows."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   "30 fps", "60 fps", "90 fps", "120 fps", "144 fps" },
      .values = { QString(), "30", "60", "90", "120", "144" } },

    { .env = "SHADOW_PROFILE_ID",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Quality"),
      .label = QT_TRANSLATE_NOOP("Settings", "Encoder profile"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The same three choices as the Borealis quality screen. Automatic "
        "sends nothing; Speed is the value the official client hardcodes "
        "(CI_BODY_5)."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   QT_TRANSLATE_NOOP("Settings", "Speed"),
                   QT_TRANSLATE_NOOP("Settings", "Quality") },
      .values = { QString(), "1", "2" } },

    { .env = "SHADOW_REG_F5",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Quality"),
      .label = QT_TRANSLATE_NOOP("Settings", "Full colour (4:4:4)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Sharper text and thin coloured lines, at a large bandwidth cost. On "
        "the Switch it is never requested whatever this says: the hardware "
        "refuses it in four places, and asking would give a black screen at "
        "triple the bitrate."),
      .kind = K::Toggle, .def = 0 },

    { .env = "SHADOW_ADAPT_BITRATE",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Adaptation"),
      .label = QT_TRANSLATE_NOOP("Settings", "Adapt the bitrate to losses"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "From the third second, the measured chunk loss moves the rate. Off "
        "keeps the rate and loses frames instead. No effect over TCP, where "
        "the server refuses the message."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_ADAPT_USER_CAP",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Adaptation"),
      .label = QT_TRANSLATE_NOOP("Settings", "Never exceed the chosen bitrate"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Your bitrate choice is the adaptation's ceiling (CFG-1 2026-09-11). "
        "Off restores the earlier rule: a ceiling frozen from the session "
        "parameters, or 20, with your choice ignored."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_HWACCEL",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Decoding"),
      .label = QT_TRANSLATE_NOOP("Settings", "Hardware decoding"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Automatic follows the platform and a marker file left by an earlier "
        "attempt. An explicit choice wins over both, which is how the software "
        "fallback is tested."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   QT_TRANSLATE_NOOP("Settings", "Off"),
                   QT_TRANSLATE_NOOP("Settings", "On") },
      .values = { QString(), "0", "1" } },

    { .env = "SHADOW_ERR_CONCEAL",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Decoding"),
      .label = QT_TRANSLATE_NOOP("Settings", "Error concealment"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "MEASURED: an A/B of four modes showed no concealment alone gives a "
        "clean picture with the taskbar legible, while guessing invents "
        "macroblocks for the lost chunks - a smear of vertical stripes. "
        "Guessing is the earlier (N40) behaviour."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "None"),
                   QT_TRANSLATE_NOOP("Settings", "Guess lost blocks") },
      .values = { "0", "3" },
      .def = 0 },

    { .env = "SHADOW_DEC_LOWDELAY",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Decoding"),
      .label = QT_TRANSLATE_NOOP("Settings", "Low-delay decoding (debug)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "libavcodec's LOW_DELAY flag is a deviation from the official client, "
        "which does not set it; the decoder was returned to defaults to match "
        "it bit for bit. Kept for debugging."),
      .kind = K::Toggle, .def = 0 },

    { .env = "SHADOW_HDR",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Experimental"),
      .label = QT_TRANSLATE_NOOP("Settings", "Request HDR"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Sets the HDR flag of the video request. What the server and this "
        "client then do with it has not been measured."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_DISPLAY_HEIGHT",
      .page = QT_TRANSLATE_NOOP("Settings", "Video"),
      .group = QT_TRANSLATE_NOOP("Settings", "Experimental"),
      .label = QT_TRANSLATE_NOOP("Settings", "Requested height"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A test override (N26) of the height asked of the server, introduced "
        "to find out whether the server adapts its stream to a smaller "
        "request. Not a validated setting."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Session default (1080p)"),
                   "540p", "720p", "1080p", "1440p", "2160p" },
      .values = { QString(), "540", "720", "1080", "1440", "2160" } },

    /* ======================================================= Audio ======= */

    { .env = "SHADOW_AUDIO_CODEC",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Sound"),
      .label = QT_TRANSLATE_NOOP("Settings", "Audio quality"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "What the official client calls high fidelity is not a bitrate, it is "
        "a codec: lossless FLAC instead of Opus (K14). The account allows both; "
        "FLAC costs several times the bandwidth."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Standard (Opus)"),
                   QT_TRANSLATE_NOOP("Settings", "High fidelity (FLAC, lossless)") },
      .values = { "1", "2" },
      .def = 0 },

    { .env = "SHADOW_VOLUME",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Sound"),
      .label = QT_TRANSLATE_NOOP("Settings", "Forced volume"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Not forced, the volume is the client's own. Forced, it applies from "
        "the moment the decoder opens - which is how a measurement starts "
        "silent. Above 100 % is amplification."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "%"),
      .min = 0, .max = 300 },

    { .env = "SHADOW_AUDIO_MAX_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Latency"),
      .label = QT_TRANSLATE_NOOP("Settings", "Catch-up threshold"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Past this much queued audio, the queue is trimmed back - for gaming, "
        "sound a third of a second late is worse than a glitch. The trim is "
        "faded, not cut (OUT-5). 0 disables the catch-up; values below 20 are "
        "raised to 20."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 0, .max = 2000, .def = 60 },

    { .env = "SHADOW_AUDIO_FLOOR_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Latency"),
      .label = QT_TRANSLATE_NOOP("Settings", "Minimum buffer"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "How much audio is kept queued. Measured on console: 20 gave 33-48 ms "
        "of latency, 16 gave 25-29 ms, 12 gave 21-24 ms; 16 keeps a margin the "
        "measurement could not yet justify removing. 0 brings back the "
        "starving behaviour."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 0, .max = 500, .def = 16 },

    { .env = "SHADOW_AUDIO_XFADE_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Latency"),
      .label = QT_TRANSLATE_NOOP("Settings", "Trim cross-fade"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The length of the fade across a trim. 0 restores the hard cut "
        "exactly; the maximum is 20."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 0, .max = 20, .def = 5 },

    { .env = "SHADOW_ALSA_LATENCE_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Latency"),
      .label = QT_TRANSLATE_NOOP("Settings", "ALSA latency (Linux)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The latency asked of ALSA: lower underruns, higher is lip-sync error "
        "that cannot be removed later."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 5, .max = 500, .def = 20 },

    { .env = "SHADOW_WIN_AUDIO",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Output"),
      .label = QT_TRANSLATE_NOOP("Settings", "Play through WASAPI (Windows)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Off returns to the path of before 2026-09-11, where audio was decoded "
        "and counted but never played (OUT-3). No effect elsewhere."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_WIN_AUDIO_MMCSS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Output"),
      .label = QT_TRANSLATE_NOOP("Settings", "High-priority audio thread (Windows)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Registers the output thread with Windows' multimedia scheduler. No "
        "load cell measured worse with it. It cannot help the session's "
        "receive thread, which is where starvation actually begins."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_AUDIO_REVIVE",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Output"),
      .label = QT_TRANSLATE_NOOP("Settings", "Revive a silent audio channel"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Tries to recover an audio channel that stops delivering, and logs how "
        "long it stayed silent. Its own comment calls it an executable "
        "hypothesis: a real revival has not yet been observed."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_HWOPUS",
      .page = QT_TRANSLATE_NOOP("Settings", "Audio"),
      .group = QT_TRANSLATE_NOOP("Settings", "Output"),
      .label = QT_TRANSLATE_NOOP("Settings", "Hardware Opus decoder (Switch)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Switch only: decodes Opus on the console's hardware decoder instead "
        "of libopus. No effect on a desktop."),
      .kind = K::Toggle, .def = 0 },

    /* ======================================================= Input ======= */

    { .env = "SHADOW_GAMEPAD_PLUG",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Gamepad"),
      .label = QT_TRANSLATE_NOOP("Settings", "Announce a gamepad"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Automatic announces one when a pad is actually present. Always "
        "announces one with no pad attached; Never leaves the VM with no "
        "controller at all."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   QT_TRANSLATE_NOOP("Settings", "Never"),
                   QT_TRANSLATE_NOOP("Settings", "Always") },
      .values = { QString(), "0", "1" } },

    { .env = "SHADOW_GAMEPAD_TYPE",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Gamepad"),
      .label = QT_TRANSLATE_NOOP("Settings", "Controller type"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Which controller the VM creates. Xbox 360 (XInput) is what Windows "
        "games support best; it was DualShock 4 until the guided capture turned "
        "out to have been made with one. The server refuses any other value, "
        "and then no rumble ever follows."),
      .kind = K::Choice,
      .choices = { "Xbox 360", "Xbox One", "DualShock 4",
                   QT_TRANSLATE_NOOP("Settings", "Joy-Con (left)"),
                   QT_TRANSLATE_NOOP("Settings", "Joy-Con (right)"),
                   QT_TRANSLATE_NOOP("Settings", "Generic") },
      .def = 0 },

    { .env = "SHADOW_RUMBLE_PCT",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Gamepad"),
      .label = QT_TRANSLATE_NOOP("Settings", "Rumble intensity"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Scales every rumble packet; 0 is silence with the channel still live. "
        "Read per packet, so it applies at once."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "%"),
      .min = 0, .max = 100, .def = 100, .appliesLive = true },

    { .env = "SHADOW_GAMEPAD",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Gamepad"),
      .label = QT_TRANSLATE_NOOP("Settings", "Read local pads (Linux)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The Linux desktop's evdev reader, opt-in while unvalidated. It decides "
        "whether THIS machine's pads are read, not whether a pad is announced "
        "to the VM - that is \"Announce a gamepad\"."),
      .kind = K::Toggle, .def = 0 },

    { .env = "SHADOW_INPUT_ABS",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Mouse"),
      .label = QT_TRANSLATE_NOOP("Settings", "Pointer mode"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Automatic is absolute on the desktop and relative once a game "
        "captures the mouse. Forcing relative brings back the deltas whose "
        "clamping produced an offset between the mouse, the cursor shown and "
        "the click."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Automatic"),
                   QT_TRANSLATE_NOOP("Settings", "Always relative"),
                   QT_TRANSLATE_NOOP("Settings", "Always absolute") },
      .values = { QString(), "0", "1" } },

    { .env = "SHADOW_CURSOR_MODE",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Mouse"),
      .label = QT_TRANSLATE_NOOP("Settings", "Cursor coordinates"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "How the cursor position received from the VM is interpreted. Centred "
        "matches the observed bytes."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Centred"),
                   QT_TRANSLATE_NOOP("Settings", "Normalized"),
                   QT_TRANSLATE_NOOP("Settings", "Delta") },
      .values = { "centered", "normalized", "delta" },
      .def = 0 },

    { .env = "SHADOW_KBD_ETENDU",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyboard"),
      .label = QT_TRANSLATE_NOOP("Settings", "Extended keys"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Off sends every key on the plain template - a fallback in case the "
        "server ever refuses the extended one - and the extended keys then "
        "fall back to their keypad namesakes: the Up arrow types \"8\"."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_WIN_KBD_HOOK",
      .page = QT_TRANSLATE_NOOP("Settings", "Input"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyboard"),
      .label = QT_TRANSLATE_NOOP("Settings", "Send system shortcuts to the VM (Windows)"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A low-level hook sends Alt+Tab and the Windows key to the VM. A "
        "captured key is eaten: neither the Windows shell nor this window sees "
        "it."),
      .kind = K::Toggle, .def = 1 },

    /* =================================================== Clipboard ======= */

    { .env = "SHADOW_CLIPBOARD",
      .page = QT_TRANSLATE_NOOP("Settings", "Clipboard"),
      .group = QT_TRANSLATE_NOOP("Settings", "Sharing"),
      .label = QT_TRANSLATE_NOOP("Settings", "Direction"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "One way protects what must not leave: otherwise anything copied on "
        "the VM lands in this machine's clipboard, where any application can "
        "read it. Enforced at the pull - with \"VM to this PC\" off, the text "
        "is never asked for."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Off"),
                   QT_TRANSLATE_NOOP("Settings", "Both ways"),
                   QT_TRANSLATE_NOOP("Settings", "This PC to the VM only"),
                   QT_TRANSLATE_NOOP("Settings", "The VM to this PC only") },
      .def = 1, .appliesLive = true },

    { .env = "SHADOW_CLIPBOARD_POLL_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Clipboard"),
      .group = QT_TRANSLATE_NOOP("Settings", "Sharing"),
      .label = QT_TRANSLATE_NOOP("Settings", "Check for changes every"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "How often the local clipboard is looked at. One call with no lock and "
        "no conversion, so being wrong here is cheap either way."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 1, .max = 5000, .def = 300 },

    { .env = "SHADOW_CLIP_MAX",
      .page = QT_TRANSLATE_NOOP("Settings", "Clipboard"),
      .group = QT_TRANSLATE_NOOP("Settings", "Limits"),
      .label = QT_TRANSLATE_NOOP("Settings", "Largest clipboard content"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The ceiling on one clipboard transfer. Parsed with strtoul: a number "
        "past INT_MAX used to wrap to a ceiling of almost 4 GiB."),
      .kind = K::Choice,
      .choices = { "64 KiB", "1 MiB", "4 MiB", "16 MiB" },
      .values = { "65536", "1048576", "4194304", "16777216" },
      .def = 2 },

    { .env = "SHADOW_CLIP_STRICT",
      .page = QT_TRANSLATE_NOOP("Settings", "Clipboard"),
      .group = QT_TRANSLATE_NOOP("Settings", "Limits"),
      .label = QT_TRANSLATE_NOOP("Settings", "Strict frame parsing"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A clipboard frame that does not parse exactly is dropped. Off accepts "
        "what the lenient parser can recover."),
      .kind = K::Toggle, .def = 1 },

    /* =============================================== File transfer ======= */

    { .env = "SHADOW_FT_REVEAL",
      .page = QT_TRANSLATE_NOOP("Settings", "File transfer"),
      .group = QT_TRANSLATE_NOOP("Settings", "Access"),
      .label = QT_TRANSLATE_NOOP("Settings", "Write the SFTP credential to a file"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Writes halyard-data/sftp.txt and sftp_password.bin for the session. "
        "THE PASSWORD GRANTS READ AND WRITE ON THE VM'S WHOLE FILESYSTEM. Both "
        "files are removed when the session ends cleanly; a crash leaves them. "
        "No third-party client can use it: it is a 395-byte PEM, newlines "
        "included, used as a password (FT5)."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_FT_ALLOW_ABSOLUTE",
      .page = QT_TRANSLATE_NOOP("Settings", "File transfer"),
      .group = QT_TRANSLATE_NOOP("Settings", "Access"),
      .label = QT_TRANSLATE_NOOP("Settings", "Allow absolute remote paths"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The session log says so when it is on. Absolute paths reach anywhere "
        "the credential can - which is everywhere."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    /* ================================================== Resilience ======= */

    { .env = "SHADOW_NACK",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Lost chunks"),
      .label = QT_TRANSLATE_NOOP("Settings", "Ask again for lost chunks"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A missing video chunk is requested again rather than waited out until "
        "the next keyframe."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_REORDER_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Lost chunks"),
      .label = QT_TRANSLATE_NOOP("Settings", "Wait for late chunks"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Measured as enough for late tails (G35). Latency paid on frames that "
        "arrive out of order, to rescue them instead of dropping them."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 0, .max = 1000, .def = 150 },

    { .env = "SHADOW_FLUSH_PREV_INCOMPLETE",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Lost chunks"),
      .label = QT_TRANSLATE_NOOP("Settings", "Decode incomplete frames"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A frame still missing chunks when the next one begins is given to the "
        "decoder instead of being discarded. Off is the earlier behaviour, "
        "which the test suite runs as the defective counter-case."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_IDR_ON_LOSS",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyframes and stalls"),
      .label = QT_TRANSLATE_NOOP("Settings", "Request a keyframe on loss"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A damaged frame propagates into every frame that references it, so "
        "without this a lost chunk smears until the server's next scheduled "
        "keyframe."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_IDR_ON_STALL",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyframes and stalls"),
      .label = QT_TRANSLATE_NOOP("Settings", "Request a keyframe on a stall"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "The same for a stream that goes QUIET rather than lossy - the case "
        "the loss path cannot see, because nothing arrives to measure losses "
        "in."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_DROP_INCOMPLETE_IDR",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyframes and stalls"),
      .label = QT_TRANSLATE_NOOP("Settings", "Drop an incomplete keyframe"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Every later frame references the keyframe, so a damaged one damages "
        "them all; dropping it costs a freeze until the next."),
      .kind = K::Toggle, .def = 1 },

    { .env = "SHADOW_STALL_MS",
      .page = QT_TRANSLATE_NOOP("Settings", "Resilience"),
      .group = QT_TRANSLATE_NOOP("Settings", "Keyframes and stalls"),
      .label = QT_TRANSLATE_NOOP("Settings", "Stall after"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "How long without a frame counts as a stall. The minimum is 50."),
      .kind = K::Number, .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
      .min = 50, .max = 5000, .def = 300 },

    /* ================================================= Diagnostics ======= */

    { .env = "SHADOW_JOURNAL_NIVEAU",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Log"),
      .label = QT_TRANSLATE_NOOP("Settings", "Log detail"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Debug adds periodic measurements; Trace logs per packet and per frame "
        "and is unreadable and expensive. The variable keeps its old French "
        "name: env.txt files in the wild use it."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Errors only"),
                   QT_TRANSLATE_NOOP("Settings", "Warnings"),
                   QT_TRANSLATE_NOOP("Settings", "Information"),
                   QT_TRANSLATE_NOOP("Settings", "Debug"),
                   QT_TRANSLATE_NOOP("Settings", "Trace") },
      .def = 2, .appliesLive = true },

    { .env = "SHADOW_JOURNAL_CATEGORIES",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Log"),
      .label = QT_TRANSLATE_NOOP("Settings", "Logged categories"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Written as a bitmask in journal.h's order. Unchecking everything "
        "means all of them again: core ignores an empty mask rather than "
        "logging nothing."),
      .kind = K::Flags,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Legacy"),
                   QT_TRANSLATE_NOOP("Settings", "Session"),
                   QT_TRANSLATE_NOOP("Settings", "Video"),
                   QT_TRANSLATE_NOOP("Settings", "Audio"),
                   QT_TRANSLATE_NOOP("Settings", "Input"),
                   QT_TRANSLATE_NOOP("Settings", "Gamepad"),
                   QT_TRANSLATE_NOOP("Settings", "Network"),
                   QT_TRANSLATE_NOOP("Settings", "Authentication"),
                   QT_TRANSLATE_NOOP("Settings", "Interface"),
                   QT_TRANSLATE_NOOP("Settings", "System") },
      .appliesLive = true },

    { .env = "SHADOW_JOURNAL_SESSIONS",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Log"),
      .label = QT_TRANSLATE_NOOP("Settings", "Session logs kept"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Logs contain live credentials (SECURITY.md), so every extra one kept "
        "is another copy of them on disk."),
      .kind = K::Number, .min = 0, .max = 20, .def = 3 },

    { .env = "SHADOW_CURL_VERBOSE",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Log"),
      .label = QT_TRANSLATE_NOOP("Settings", "Trace HTTP requests"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Its PRESENCE enables it, whatever the value - which is why off "
        "removes the variable instead of writing 0. The trace includes request "
        "headers: treat it as containing credentials."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_HID_LOCK_PROBE",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Tests and probes"),
      .label = QT_TRANSLATE_NOOP("Settings", "Lock-key probe"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "One Caps/Num/Scroll Lock request per session (HID1). Asserting claims "
        "all three off, which makes the VM toggle its own keys to match - its "
        "states cannot be read without asserting yours. Asking sends an empty "
        "request, which the server answers with nothing."),
      .kind = K::Choice,
      .choices = { QT_TRANSLATE_NOOP("Settings", "Off"),
                   QT_TRANSLATE_NOOP("Settings", "Ask (answers nothing)"),
                   QT_TRANSLATE_NOOP("Settings", "Assert all three off") },
      .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_GAMEPAD_SELFTEST",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Tests and probes"),
      .label = QT_TRANSLATE_NOOP("Settings", "Gamepad self-test"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "Sends Cross, then a sweep of the left stick, to check the wire "
        "without a pad plugged in. Needs an announced gamepad, and moves the "
        "stick in whatever the VM is running."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_FT_SELFTEST",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Tests and probes"),
      .label = QT_TRANSLATE_NOOP("Settings", "SFTP self-test"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "LIST, PUT, STAT, GET, a byte-for-byte round trip and REMOVE against "
        "the VM, reported in the log. Needs a build with "
        "-DSHADOW_FILETRANSFER=ON."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },

    { .env = "SHADOW_FRAME_HASH",
      .page = QT_TRANSLATE_NOOP("Settings", "Diagnostics"),
      .group = QT_TRANSLATE_NOOP("Settings", "Tests and probes"),
      .label = QT_TRANSLATE_NOOP("Settings", "Fingerprint every frame"),
      .description = QT_TRANSLATE_NOOP("Settings",
        "A hash of each decoded frame's luma, to find the first frame where "
        "the live decode parts ways with an offline decode of the same stream. "
        "A full pass over the luma per frame: a measurement tool."),
      .kind = K::Toggle, .unsetValue = 0, .def = 0 },
    };
    return kTable;
}

/* Pages and groups, in table order, derived so they cannot disagree with the
 * rows. Untranslated: the window translates, the test compares. */
inline QStringList settingsPages()
{
    QStringList out;
    for (const Setting &s : settings()) {
        const QString p = QString::fromUtf8(s.page);
        if (!out.contains(p)) out << p;
    }
    return out;
}

inline QStringList settingsGroups(const QString &page)
{
    QStringList out;
    for (const Setting &s : settings()) {
        if (QString::fromUtf8(s.page) != page) continue;
        const QString g = QString::fromUtf8(s.group);
        if (!out.contains(g)) out << g;
    }
    return out;
}

/* === reading and writing a value ==========================================
 *
 * Kept here rather than in the window, because the rules are not obvious and
 * the test can reach them.
 */

/* Every bit a Flags row can set. */
inline int flagsAll(const Setting &s)
{
    return (s.choices.size() >= 31) ? 0x7fffffff : ((1 << s.choices.size()) - 1);
}

/* The lowest position of a number's control: one step below `min` when the
 * default is NOT a fixed value, where the control shows "automatic" and the
 * variable is unset. A known default needs no such position - the control
 * shows the default itself. */
inline int numberFloor(const Setting &s)
{
    return (s.def < 0 && s.unsetValue != s.min) ? s.min - 1 : s.min;
}

/* The position `raw` selects, or -1 when it says nothing readable.
 *
 * `raw` is the environment string, or null when unset. A value with no digits
 * reads as -1 and NOT as 0 - `atoi` answers 0 for "off", "O" and "", and 0 is
 * a meaningful setting for several of these keys. A choice carrying `values`
 * is compared against those strings only. A Flags row reads the mask the way
 * core does (strtoul, base 0, so 0x... works), and 0 reads as -1 because core
 * ignores it. */
inline int settingValue(const Setting &s, const char *raw)
{
    if (!raw || !*raw) return -1;

    if (s.kind == Setting::Kind::Choice && !s.values.isEmpty()) {
        const QString v = QString::fromUtf8(raw).trimmed();
        for (int i = 0; i < s.values.size(); i++)
            if (!s.values[i].isEmpty() && s.values[i] == v) return i;
        return -1;
    }

    if (s.kind == Setting::Kind::Flags) {
        bool ok = false;
        const qulonglong m = QString::fromUtf8(raw).trimmed().toULongLong(&ok, 0);
        if (!ok || m == 0) return -1;
        return (int)(m & (qulonglong)flagsAll(s));
    }

    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    if (*p < '0' || *p > '9') return -1;
    long long v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
        if (v > 1000000000LL) break;
    }
    v *= sign;

    switch (s.kind) {
    case Setting::Kind::Toggle:
        return (v == 0 || v == 1) ? (int)v : -1;
    case Setting::Kind::Choice:
        return (v >= 0 && v < s.choices.size()) ? (int)v : -1;
    case Setting::Kind::Number:
        return (v >= s.min && v <= s.max) ? (int)v : -1;
    case Setting::Kind::Text:
    case Setting::Kind::Flags:
        return -1;
    }
    return -1;
}

/* The position a control must SHOW for `raw`. Absent is where this differs
 * from `settingValue`, and fact 5 is why: a choice that can unset shows its
 * unsetting entry ("Automatic"); anything with a known `def` shows it; a
 * number without one shows its floor ("automatic"); a Flags row shows every
 * bit. -1 = nothing to show. */
inline int settingDisplayIndex(const Setting &s, const char *raw)
{
    if (raw && *raw) return settingValue(s, raw);

    switch (s.kind) {
    case Setting::Kind::Choice:
        for (int i = 0; i < s.values.size(); i++)
            if (s.values[i].isEmpty()) return i;
        return s.def;
    case Setting::Kind::Number:
        return s.def >= 0 ? s.def : numberFloor(s);
    case Setting::Kind::Toggle:
        return s.def;
    case Setting::Kind::Flags:
        return flagsAll(s);
    case Setting::Kind::Text:
        return -1;
    }
    return -1;
}

/* The choice to label "(default)", or -1. An unsetting entry is not labelled:
 * "Automatic" already says what it is. */
inline int settingDefaultChoice(const Setting &s)
{
    if (s.kind != Setting::Kind::Choice) return -1;
    for (const QString &v : s.values) if (v.isEmpty()) return -1;
    return (s.def >= 0 && s.def < s.choices.size()) ? s.def : -1;
}

/* What to write for `value`, or an empty QString when the variable must be
 * UNSET instead (fact 3). */
inline QString settingWrite(const Setting &s, int value)
{
    if (s.kind == Setting::Kind::Choice && !s.values.isEmpty()) {
        if (value < 0 || value >= s.values.size()) return QString();
        return s.values[value];   /* may be empty = unset, which is the point */
    }
    if (s.kind == Setting::Kind::Flags) {
        const int m = value & flagsAll(s);
        /* All bits is core's default, and an empty mask is ignored by core:
         * both are honestly written as "no variable". */
        if (m == 0 || m == flagsAll(s)) return QString();
        return QStringLiteral("0x%1").arg(m, 0, 16);
    }
    if (s.kind == Setting::Kind::Number && value < s.min) return QString();
    if (s.unsetValue >= 0 && value == s.unsetValue) return QString();
    return QString::number(value);
}

}  // namespace halyard
