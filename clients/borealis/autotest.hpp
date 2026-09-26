/* autotest - chaining several sessions with no human intervention.
 *
 * An INTERMITTENT defect cannot be characterised in one session. The
 * cursor/audio channel is the example: it starts six times out of seven on the
 * desktop and never on the console - finding the cause takes dozens of
 * measurements, and until now each one cost the user a manipulation.
 *
 * This mode is triggered by a file dropped on the SD card, never by a UI option:
 * it must not be possible to enable it by accident, and it must be possible to
 * push it over FTP without touching the console.
 *
 *   SHADOW_DATA_DIR "autotest.txt"   ->   runs=10 duration=30 pause=5
 *
 * Each session writes one machine-readable summary line, and the log mirror
 * (cf. logsink.txt) sends it to the development machine as it goes. One launch =
 * N samples.
 *
 * File absent = a single run, normal behaviour.
 *
 * The file is CONSUMED when the series goes to the END: one request = one
 * series. Without that it relaunched twenty automatic sessions every time the
 * client was opened. An INTERRUPTED series keeps it, so the next launch resumes
 * the measurement. (AF12 2026-09-10 - this line used to say "when read", which
 * the code never did.)
 */
#pragma once

namespace autotest {

struct Plan {
    int  runs     = 1;    /* number of sessions to chain */
    int  duration = 0;    /* seconds of streaming; 0 = until it ends naturally */
    int  pause    = 5;    /* seconds between two sessions */
    bool active   = false;
};

/* Reads the file once and keeps the result. */
const Plan &plan();

/* Deletes the plan file. To be called when the series has run TO THE END: an
 * interrupted launch must not take the plan down with it. */
void consumePlan();

/* Replaces the current plan without going through the SD card. Used by the
 * bidirectional channel: changing three numbers must not cost a round trip
 * through the console's menu. Takes effect on the NEXT session - we do not cut a
 * running series out from under whoever is watching it. */
void setPlan(int runs, int duration, int pause);

/* Interprets a line coming from the development machine. Called from the log
 * drain thread: it must stay brief and block nothing. */
void handleCommand(const char *line);

}  // namespace autotest
