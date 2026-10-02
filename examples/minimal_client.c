/* minimal_client - the smallest thing that uses `halyard-core` and is not the
 * Borealis application.
 *
 * === LIB1 2026-10-02 — WHY THIS EXISTS =====================================
 *
 * `core/` is meant to be reusable by a second desktop client (Qt, Tauri, GTK).
 * That claim was true and unverified for months: `core/` was globbed into the
 * application target, so nothing ever linked it on its own, and the two
 * symbols it quietly expected from `clients/borealis/` would have been
 * discovered by whoever wrote that second client.
 *
 * This file is the check. It compiles against the library's public headers,
 * provides nothing but a frame sink, and links. If it stops linking, `core/`
 * has grown a dependency on a client — which is exactly what
 * `tools/check-core-independence.py` and this target exist to catch, from two
 * different angles: that script reads the source, this one asks the linker.
 *
 * Build it with:   cmake -DSHADOW_BUILD_EXAMPLES=ON ...
 *
 * === THE THREE CONTRACTS A NEW CLIENT MUST RESPECT =========================
 *
 * They are not obvious from the headers, so they are written here, where
 * somebody starting a client will read them.
 *
 * 1. ONE SESSION PER PROCESS. `ctrl_session_run()` takes no handle, and
 *    `ctrl_session_set_bitrate()`, `ctrl_session_request_idr()` and
 *    `ctrl_session_active()` are global. There is a lot of module state behind
 *    them. One stream at a time, and no two windows sharing the library.
 *
 * 2. YOU GIVE CORE A THREAD. `ctrl_session_glue_run()` blocks for the whole
 *    session, and the video, audio and cursor callbacks arrive ON ITS OWN
 *    THREADS. The bytes are valid only during the call. So: a worker thread
 *    (QThread, std::thread, a Rust thread) and a queue towards the UI. Neither
 *    Qt's event loop nor Tauri's main thread may call into core directly.
 *
 * 3. CONFIGURATION IS THE ENVIRONMENT. Around 260 `SHADOW_*` variables gate
 *    the streaming path, read with `getenv` at first use and CACHED in
 *    statics. A client sets them before starting the session;
 *    `clients/borealis/settings.cpp::applyToggles()` is the worked example.
 *    Consequence: most settings take effect on the NEXT session.
 *
 * This client does not open a session — that needs an account, a VM and a
 * network, none of which belongs in a build check. It exercises the API
 * surface that a client touches first: register the sink, read the grant
 * snapshot, ask whether a session is up.
 */
#include <stdio.h>
#include <stdint.h>

#include "core/protocol/ctrl_session.h"
#include "core/protocol/ctrl_session_glue.h"
#include "core/protocol/session_caps.h"

/* Where decoded pictures arrive. Called on the DECODE thread, and the planes
 * die with the call — a real client copies them or uploads them here. */
static void frame_sink(int width, int height,
                       const uint8_t *data_y, int linesize_y,
                       const uint8_t *data_u, int linesize_u,
                       const uint8_t *data_v, int linesize_v,
                       int format, int64_t pts, void *user)
{
    (void)data_y; (void)linesize_y;
    (void)data_u; (void)linesize_u;
    (void)data_v; (void)linesize_v;
    (void)format; (void)pts; (void)user;
    printf("  frame %dx%d\n", width, height);
}

int main(void)
{
    printf("halyard-core, used from outside the Borealis client\n");

    /* Contract 2, the part a client gets wrong first: this must be registered
     * BEFORE the session runs, or every picture is decoded and dropped (core
     * says so once in the log rather than staying silent). */
    ctrl_session_glue_set_frame_sink(frame_sink);

    /* The grant snapshot: which channels exist, which port to dial for each,
     * and the transport the server actually chose. False before a session has
     * completed its bootstrap, which is the case here. */
    shadow_session_caps caps;
    const bool have = ctrl_session_caps(&caps);
    printf("  session active : %s\n", ctrl_session_active() ? "yes" : "no");
    printf("  grant snapshot : %s\n", have ? "present" : "none yet (expected)");

    if (have) {
        printf("  port base      : %d\n", caps.port_base);
        printf("  channels       : %u of 8 granted\n", caps.n_granted);
        for (int i = 0; i < 8; i++) {
            if (caps.chan[i].granted)
                printf("    [%d] %s :%u\n", i,
                       caps.chan[i].tcp ? "TCP" : "UDP",
                       (unsigned)caps.chan[i].port);
        }
    }

    printf("linked and ran: core/ needs no client\n");
    return 0;
}
