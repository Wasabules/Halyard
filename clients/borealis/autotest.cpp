/* autotest - see the header. */
#include "autotest.hpp"
extern "C" {
#include "../../core/services/atomic_file.h"   /* shadow_file_remove/_rename */
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../../core/services/config.h"
#include "../../core/services/log.h"
/* S81 - this module's category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define atlog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)
#define atdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

}

namespace autotest {

const Plan &plan()
{
    static Plan p;
    static bool read_once = false;
    if (read_once) return p;
    read_once = true;

    char path[256];
    std::snprintf(path, sizeof(path), "%sautotest.txt", SHADOW_DATA_DIR);
    std::FILE *f = std::fopen(path, "r");
    if (!f) return p;

    char line[256] = {0};
    if (std::fgets(line, sizeof(line), f)) {
        /* A deliberately flat format: `runs=10 duration=30 pause=5`, in any
         * order, absent keys keeping their default. A format you can type by
         * hand without getting it wrong beats a JSON you have to re-read in
         * order to write. */
        for (char *tok = std::strtok(line, " \t\r\n"); tok;
             tok = std::strtok(nullptr, " \t\r\n")) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            const int v = std::atoi(eq + 1);
            if      (!std::strcmp(tok, "runs")     && v >= 1 && v <= 200) p.runs = v;
            else if (!std::strcmp(tok, "duration") && v >= 0 && v <= 600) p.duration = v;
            else if (!std::strcmp(tok, "pause")    && v >= 0 && v <= 120) p.pause = v;
        }
        p.active = (p.runs > 1) || (p.duration > 0);
    }
    std::fclose(f);

    /* The plan is NOT deleted here - see `consumePlan()`.
     *
     * It used to be, and that backfired: a launch that dies before chaining its
     * runs (application closed remotely, log mirror down, connection abandoned)
     * took the plan down with it. Two measurements were lost that way, and the
     * file had to be put back every time. So we only delete when the series has
     * REALLY run to the end.
     *
     * The old comment, still true in substance:
     *
     * Without that it stayed on the card and relaunched a full series on every
     * application start - the user opens the client to play and ends up with
     * twenty chained automatic sessions they never asked for. The file is a
     * one-off REQUEST, not a setting: `tools/switch-logsink.sh autotest N D P`
     * puts it back when another one is wanted. The `autotest` command over the
     * bidirectional log does not go through here (see setPlan) and therefore
     * stays usable on a running session. */
    atlog("[AUTOTEST] plan read: %d runs of %d s (erased at the end of the series)",
               p.runs, p.duration);
    return p;
}

void consumePlan()
{
    char path[256];
    std::snprintf(path, sizeof(path), "%sautotest.txt", SHADOW_DATA_DIR);
    if (shadow_file_remove(path) == 0)
        atlog("[AUTOTEST] serie terminee — plan efface");
    /* Absent: nothing to say, the plan came from a live command. */

    /* AF12 2026-09-10 - and DISARMED in memory. `plan()` reads the file once
     * per process into a static, so with only the file gone the next
     * connection in the SAME process ran the whole series again. */
    Plan &p = const_cast<Plan &>(plan());
    p.runs = 1; p.duration = 0; p.pause = 0; p.active = false;
}

void setPlan(int runs, int duration, int pause)
{
    Plan &p = const_cast<Plan &>(plan());   /* forces the initial read */
    if (runs     >= 1 && runs     <= 200) p.runs = runs;
    if (duration >= 0 && duration <= 600) p.duration = duration;
    if (pause    >= 0 && pause    <= 120) p.pause = pause;
    p.active = (p.runs > 1) || (p.duration > 0);
}

void handleCommand(const char *line)
{
    if (!line) return;

    /* `autotest N D P` - arms a series. `autotest 1 0 0` disarms it.
     * `ping` - checks that the channel answers, without changing anything.
     * Any other line is ignored silently: this channel can receive noise, and an
     * unknown command must above all not stop the run in progress. */
    int runs = 0, dur = 0, pause = 0;
    if (std::sscanf(line, "autotest %d %d %d", &runs, &dur, &pause) == 3) {
        setPlan(runs, dur, pause);
        const Plan &p = plan();
        atlog("[CMD] test plan: %d sessions of %d s, %d s of rest (active=%d)",
                   p.runs, p.duration, p.pause, (int)p.active);
    } else if (std::strncmp(line, "ping", 4) == 0) {
        atlog("[CMD] pong");
    }
}

}  // namespace autotest
