/* Stubs for the headless measurement binary.
 *
 * === WHAT THIS FILE REPLACES, AND WHY IT IS SO SHORT ===
 *
 * `halyard-cli` compiles the protocol core without the application, so it
 * lacks what the UI normally provides.
 *
 * It used to inherit these symbols from `main_test_stubs.c` AND from the
 * in-house WebRTC stack, compiled here and nowhere else. That stack is gone
 * (S112, then the whole WebRTC path on 2026-09-26); what remained necessary
 * fits in two empty functions.
 *
 * BOTH ARE LEGITIMATE NO-OPS, not holes:
 *
 *   `stream_view_push_yuv`  - there is no screen. A video latency measurement
 *                             stays valid: the path stops at "frame decoded",
 *                             and the reading says so.
 *   `shadow_link_info`      - the "docked or handheld" question is meaningless
 *                             on a desktop. We return "unknown", which callers
 *                             already treat as "change nothing" (cf.
 *                             `device_mode.hpp`) - and NOT "wired", which would
 *                             wrongly raise the bitrate.
 */
#include <stdint.h>
#include <stddef.h>

void stream_view_push_yuv(int width, int height,
                          const uint8_t *data_y, int linesize_y,
                          const uint8_t *data_u, int linesize_u,
                          const uint8_t *data_v, int linesize_v,
                          int format, int64_t pts, void *user) {
    (void)width; (void)height;
    (void)data_y; (void)linesize_y;
    (void)data_u; (void)linesize_u;
    (void)data_v; (void)linesize_v;
    (void)format; (void)pts; (void)user;
}

int shadow_link_info(int *docked, int *strength) {
    if (docked)   *docked   = 0;
    if (strength) *strength = 0;
    return 0;   /* unknown - and above all not "wired" */
}
