/* test_sse_event - the VM's event stream taxonomy (SSE1).
 *
 * Both clients hold two SSE streams open for the whole session because the
 * control port does not open without them, and until now neither read a byte
 * of what came back. The taxonomy comes from an RE of `ShadowPCDisplay`
 * (`memory/project_sse_event_stream_RE.md`, C95); the one event ever captured
 * is reproduced below verbatim as the first case, because a parser for a
 * stream nobody has read is worth pinning against the one real sample there
 * is.
 */
#include <stdio.h>
#include <string.h>

#include "../core/services/sse_event.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

static void eqs(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got ? got : "(null)", want) != 0) {
        failures++;
        printf("  FAIL %-44s got \"%s\", expected \"%s\"\n", what,
               got ? got : "(null)", want);
    }
}

int main(void)
{
    sse_event e;
    printf("== SSE event taxonomy (SSE1 2026-10-03) ==\n");

    /* --- THE one captured event, byte for byte from the note ------------- */
    {
        const char *s =
            "{\"type\": \"event\", \"event\": \"shadow-manager\","
            " \"data\": {\"target\": \"VMP\", \"sender\": \"ShadowManager\","
            " \"type\": \"encoding_is_ready\","
            " \"value\": {\"encoding_type\": \"hardware\", \"is_banner\": false}}}";
        ok(sse_event_parse(s, 0, &e), "the captured event parses");
        ok(e.type == SSE_TYPE_EVENT, "type = event");
        ok(e.event == SSE_EV_SHADOW_MANAGER, "event = shadow-manager");
        eqs(e.event_name, "shadow-manager", "the name is kept verbatim");
        eqs(e.sub, "encoding_is_ready", "data.type is the sub-type");
        /* `value` is an OBJECT here, and detail is a line for a human: an
         * object flattened into it would be a wall, not a summary. */
        eqs(e.detail, "", "an object value does not become the detail");
    }

    /* --- the two worth showing the moment they arrive -------------------- */
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"bsod\"}", 0, &e),
           "bsod parses");
        ok(e.event == SSE_EV_BSOD, "bsod is recognised");
    }
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"get-out\"}", 0, &e),
           "get-out parses");
        ok(e.event == SSE_EV_GET_OUT, "get-out is recognised");
    }

    /* --- the machine's run state ----------------------------------------- *
     *
     * BOTH spellings. The display binary's handler is named for
     * `status-changed`; `launcher.c`'s own note about the machine list writes
     * `status_changed`. Neither observation is worth betting the feature on. */
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"status-changed\","
                           "\"data\":{\"status\":\"started\"}}", 0, &e),
           "status-changed parses");
        ok(e.event == SSE_EV_STATUS_CHANGED, "hyphen spelling recognised");
        eqs(e.detail, "started", "the new state is the detail");
    }
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"status_changed\","
                           "\"data\":{\"state\":\"stopped\"}}", 0, &e),
           "status_changed parses");
        ok(e.event == SSE_EV_STATUS_CHANGED, "underscore spelling recognised");
        eqs(e.detail, "stopped", "data.state is read when data.status is absent");
    }

    /* --- the rest of the taxonomy ---------------------------------------- */
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"l2tp\"}", 0, &e), "l2tp");
        ok(e.event == SSE_EV_L2TP, "l2tp recognised");
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"vm-reachable\"}", 0, &e),
           "vm-reachable");
        ok(e.event == SSE_EV_VM_REACHABLE, "vm-reachable recognised");
    }

    /* --- level 1 types that are not events ------------------------------- */
    {
        ok(sse_event_parse("{\"type\":\"session\",\"length\":245}", 0, &e),
           "a session frame parses");
        ok(e.type == SSE_TYPE_SESSION, "type = session");
        ok(e.event == SSE_EV_NONE, "with no event sub-type");
        ok(sse_event_parse("{\"type\":\"main\",\"opaque\":\"...\"}", 0, &e), "main");
        ok(e.type == SSE_TYPE_MAIN, "type = main");
        ok(sse_event_parse("{\"type\":\"error\"}", 0, &e), "error");
        ok(e.type == SSE_TYPE_ERROR, "type = error");
        ok(sse_event_parse("{\"type\":\"forward\"}", 0, &e), "forward");
        ok(e.type == SSE_TYPE_FORWARD, "type = forward");
    }

    /* --- an event the taxonomy does not have ----------------------------- *
     *
     * Reported as UNKNOWN with the name kept, not dropped. The taxonomy was
     * read off a binary in May and a server may add to it; a name we throw
     * away is a name that stays unknown. */
    {
        ok(sse_event_parse("{\"type\":\"event\",\"event\":\"teleport\"}", 0, &e),
           "an unknown event still parses");
        ok(e.event == SSE_EV_UNKNOWN, "and is flagged unknown");
        eqs(e.event_name, "teleport", "with its name kept for the log");
    }

    /* --- what a stream actually delivers between events ------------------ *
     *
     * Comments, blank keepalive lines and partial frames all arrive on the
     * same callback, so the parser must refuse them quietly rather than make
     * the caller filter first. */
    ok(!sse_event_parse("", 0, &e), "an empty line is refused");
    ok(!sse_event_parse(": keepalive", 0, &e), "an SSE comment is refused");
    ok(!sse_event_parse("\n", 0, &e), "a bare newline is refused");
    ok(!sse_event_parse("{\"type\":\"eve", 0, &e), "a truncated frame is refused");
    ok(!sse_event_parse("[1,2,3]", 0, &e), "a JSON ARRAY is refused, not accepted");
    ok(!sse_event_parse("\"a string\"", 0, &e), "a bare JSON string is refused");
    ok(!sse_event_parse(NULL, 0, &e), "NULL is refused");

    /* Trailing whitespace is the normal shape of an SSE frame, and rejecting
     * it on its own newline would have refused every real event. */
    ok(sse_event_parse("{\"type\":\"event\",\"event\":\"bsod\"}\n\n", 0, &e),
       "a frame with its trailing newlines parses");

    /* An explicit length, since the caller has a buffer and not a string. */
    {
        const char *buf = "{\"type\":\"event\",\"event\":\"bsod\"}GARBAGE";
        ok(sse_event_parse(buf, 31, &e), "an explicit length is honoured");
        ok(e.event == SSE_EV_BSOD, "and stops at it");
    }

    /* --- the output is always initialised -------------------------------- *
     *
     * A caller that ignores the return value must not read a stale struct. */
    {
        memset(&e, 0xAA, sizeof e);
        (void)sse_event_parse("not json", 0, &e);
        ok(e.type == SSE_TYPE_UNKNOWN && e.event == SSE_EV_NONE
               && e.event_name[0] == '\0' && e.detail[0] == '\0',
           "a refused parse leaves the struct zeroed");
    }

    /* --- truncation rather than refusal ---------------------------------- */
    {
        char big[1024];
        snprintf(big, sizeof big,
                 "{\"type\":\"event\",\"event\":\"bsod\",\"data\":{\"status\":\"%0*d\"}}",
                 400, 7);
        ok(sse_event_parse(big, 0, &e), "an over-long detail still parses");
        ok(strlen(e.detail) == sizeof(e.detail) - 1, "and is truncated to fit");
    }

    eqs(sse_event_kind_name(SSE_EV_GET_OUT), "get-out", "kind names round-trip");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
