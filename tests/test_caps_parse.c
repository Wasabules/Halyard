/* test_caps_parse - the /vms/{id}/capabilities reply (CAPS2).
 *
 * This body was fetched for months and four values were read out of it. The
 * rest sat in `raw_json`, unread, and when it was finally printed it held the
 * two things the client most visibly lacked: the per-session time ceiling the
 * official client counts down from, and the number of monitors the VM allows.
 *
 * The fixture below has the SHAPE of a real reply - every key in the order and
 * the nesting the server uses - with invented values. Not a captured body:
 * the real one carries an account's fair-use figures, which are personal and
 * have no business in a repository. What is being tested is the walk through
 * three levels of nesting, not somebody's usage.
 */
#include <stdio.h>
#include <string.h>

#include "../core/services/launcher.h"

static int checks = 0, failures = 0;

static void eqi(long got, long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-44s got %ld, expected %ld\n", what, got, want);
    }
}

static void eqs(const char *got, const char *want, const char *what)
{
    checks++;
    const char *g = got ? got : "(null)";
    if (strcmp(g, want) != 0) {
        failures++;
        printf("  FAIL %-44s got \"%s\", expected \"%s\"\n", what, g, want);
    }
}

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

/* The shape, with invented numbers. */
static const char *kBody =
"{\"version\":\"1.0\","
" \"streaming\":{\"channels\":{"
"   \"video\":{\"allowed\":true,\"chroma\":[\"444\",\"420\"],"
"             \"codec\":[\"h264\",\"h265\",\"av1\"],\"frame_rate\":144,"
"             \"max_monitor_count\":3,"
"             \"max_resolution\":{\"height\":2160,\"width\":3840}},"
"   \"audio\":{\"allowed\":true,\"codec\":[\"opus\",\"flac\"]},"
"   \"micro\":{\"allowed\":false},"
"   \"clipboard\":{\"allowed\":true},"
"   \"filetransfer\":{\"allowed\":true},"
"   \"gamepad\":{\"allowed\":false},"
"   \"cursor\":{\"allowed\":true},"
"   \"input\":{\"allowed\":true}}},"
" \"usage\":{\"end_of_streaming_session\":null,"
"           \"fair_use_alert_threshold\":0.8,"
"           \"fair_use_renew_date\":\"2030-01-01T00:00:00Z\","
"           \"fair_use_usage\":1234,\"max_duration\":360000,"
"           \"max_session_length\":14400,\"time_slots\":[],"
"           \"time_slots_enabled\":false,"
"           \"time_slots_timezone\":\"Europe/Paris\"},"
" \"vdb\":{\"queue_priority\":\"basic\"}}";

int main(void)
{
    VmCapabilities c;
    printf("== /capabilities parsing (CAPS2 2026-10-03) ==\n");

    /* --- the whole shape -------------------------------------------------- */
    memset(&c, 0, sizeof c);
    ok(launcher_parse_capabilities(kBody, &c), "a real-shaped body parses");

    /* The four that were already read. */
    ok(c.video_allowed, "video allowed");
    eqi(c.max_frame_rate, 144, "frame rate");
    eqi(c.max_width, 3840, "max width");
    eqi(c.max_height, 2160, "max height");
    eqs(c.video_codecs, "h264,h265,av1", "video codecs joined");
    ok(c.audio_allowed, "audio allowed");
    eqs(c.audio_codecs, "opus,flac", "audio codecs joined");

    /* CAPS2/DISP1 - the display ceiling, three levels down. This is the
     * number ShadowStreamer enforces as "VM does not support more than %d
     * display(s)". */
    eqi(c.max_monitor_count, 3, "max_monitor_count");
    eqs(c.video_chroma, "444,420", "chroma joined");

    /* The per-channel permissions. `micro` and `gamepad` are false in the
     * fixture ON PURPOSE: a parser that ignored the value and reported the
     * key's presence would pass every all-true test. */
    ok(c.clipboard_allowed, "clipboard allowed");
    ok(c.filetransfer_allowed, "file transfer allowed");
    ok(!c.micro_allowed, "micro REFUSED is read as refused");
    ok(!c.gamepad_allowed, "gamepad REFUSED is read as refused");

    /* The usage block - the session countdown's source. */
    eqi(c.usage.max_session_length, 14400, "max_session_length");
    eqi(c.usage.max_duration, 360000, "max_duration");
    eqi(c.usage.fair_use_usage, 1234, "fair_use_usage");
    checks++;
    if (c.usage.fair_use_alert_threshold < 0.79 ||
        c.usage.fair_use_alert_threshold > 0.81) {
        failures++;
        printf("  FAIL alert threshold is not 0.8\n");
    }
    eqs(c.usage.fair_use_renew_date, "2030-01-01T00:00:00Z", "renew date");
    ok(!c.usage.time_slots_enabled, "time slots disabled");
    eqs(c.usage.time_slots_timezone, "Europe/Paris", "time slots timezone");
    /* Null in every sample so far, and null must not become a string. */
    ok(c.usage.end_of_streaming_session == NULL, "a null hard stop stays NULL");
    vmcaps_free(&c);

    /* --- a body that is not JSON ----------------------------------------- */
    memset(&c, 0, sizeof c);
    ok(!launcher_parse_capabilities("not json at all", &c),
       "a non-JSON body is refused");
    ok(!launcher_parse_capabilities(NULL, &c), "a null body is refused");
    vmcaps_free(&c);

    /* --- a body that parses but says nothing we know ---------------------- *
     *
     * A SUCCESS with an empty struct, not a failure: that is a server that
     * changed, not a client that broke, and the caller decides differently
     * about the two. */
    memset(&c, 0, sizeof c);
    ok(launcher_parse_capabilities("{}", &c), "an empty object is a success");
    eqi(c.max_monitor_count, 0, "with nothing filled in");
    eqi(c.usage.max_session_length, 0, "and no session ceiling");
    ok(!c.video_allowed, "and nothing allowed");
    vmcaps_free(&c);

    /* --- partial bodies, which is how a server evolves -------------------- */
    memset(&c, 0, sizeof c);
    ok(launcher_parse_capabilities(
           "{\"streaming\":{\"channels\":{\"video\":{\"allowed\":true}}}}", &c),
       "video with no limits parses");
    ok(c.video_allowed, "and video is still allowed");
    eqi(c.max_monitor_count, 0, "with no monitor count");
    eqi(c.max_width, 0, "and no resolution");
    vmcaps_free(&c);

    memset(&c, 0, sizeof c);
    ok(launcher_parse_capabilities("{\"usage\":{\"max_session_length\":60}}", &c),
       "a usage block alone parses");
    eqi(c.usage.max_session_length, 60, "and gives the ceiling");
    vmcaps_free(&c);

    /* A wrong TYPE must be ignored, not coerced: the integer accessors are
     * guarded, and a string where a number belongs is the shape a server
     * change takes most often. */
    memset(&c, 0, sizeof c);
    ok(launcher_parse_capabilities(
           "{\"streaming\":{\"channels\":{\"video\":"
           "{\"max_monitor_count\":\"two\"}}}}", &c),
       "a string where the count belongs still parses");
    eqi(c.max_monitor_count, 0, "and is ignored rather than coerced");
    vmcaps_free(&c);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
