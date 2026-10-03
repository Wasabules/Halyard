/* sse_event.c - see the header for the taxonomy and for why the stream is
 * probably going to be quiet. */
#include "sse_event.h"

#include <jansson.h>
#include <string.h>

static void copy_into(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;   /* truncate: a short detail still reports */
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* The text of a value that may be a string, a number or a bool. Objects and
 * arrays are NOT flattened: `detail` is a line for a human, and a nested
 * object rendered into it would be a wall rather than a summary. */
static void text_of(char *dst, size_t cap, json_t *v)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!v) return;
    if (json_is_string(v))      copy_into(dst, cap, json_string_value(v));
    else if (json_is_true(v))   copy_into(dst, cap, "true");
    else if (json_is_false(v))  copy_into(dst, cap, "false");
    else if (json_is_integer(v)) {
        char b[32];
        snprintf(b, sizeof b, "%lld", (long long)json_integer_value(v));
        copy_into(dst, cap, b);
    } else if (json_is_real(v)) {
        char b[32];
        snprintf(b, sizeof b, "%.3f", json_real_value(v));
        copy_into(dst, cap, b);
    }
}

static sse_type type_of(const char *s)
{
    if (!s) return SSE_TYPE_UNKNOWN;
    if (strcmp(s, "event")   == 0) return SSE_TYPE_EVENT;
    if (strcmp(s, "forward") == 0) return SSE_TYPE_FORWARD;
    if (strcmp(s, "error")   == 0) return SSE_TYPE_ERROR;
    if (strcmp(s, "session") == 0) return SSE_TYPE_SESSION;
    if (strcmp(s, "main")    == 0) return SSE_TYPE_MAIN;
    return SSE_TYPE_UNKNOWN;
}

static sse_event_kind kind_of(const char *s)
{
    if (!s || !*s) return SSE_EV_NONE;
    if (strcmp(s, "l2tp") == 0)           return SSE_EV_L2TP;
    if (strcmp(s, "vm-reachable") == 0)   return SSE_EV_VM_REACHABLE;
    if (strcmp(s, "bsod") == 0)           return SSE_EV_BSOD;
    if (strcmp(s, "shadow-manager") == 0) return SSE_EV_SHADOW_MANAGER;
    /* BOTH spellings, and the hyphenless one is the REAL one: the taxonomy
     * was read off the display binary's handler name
     * (`handleSSEDataEventGetOut`), from which `get-out` was inferred - and a
     * live session logs `{"type":"event","event":"getout",...}`. The guess
     * was wrong and only a capture could say so. */
    if (strcmp(s, "get-out") == 0)        return SSE_EV_GET_OUT;
    if (strcmp(s, "getout") == 0)         return SSE_EV_GET_OUT;
    /* BOTH spellings. The RE of the display binary names the handler
     * `handleSSEDataEventStatusChanged` for `status-changed`, while
     * `launcher.c`'s own note about the machine list writes
     * `status_changed`. One of the two observations is of a different
     * surface and neither is worth betting on, so both are accepted. */
    if (strcmp(s, "status-changed") == 0) return SSE_EV_STATUS_CHANGED;
    if (strcmp(s, "status_changed") == 0) return SSE_EV_STATUS_CHANGED;
    return SSE_EV_UNKNOWN;
}

const char *sse_event_kind_name(sse_event_kind k)
{
    switch (k) {
    case SSE_EV_NONE:           return "none";
    case SSE_EV_L2TP:           return "l2tp";
    case SSE_EV_VM_REACHABLE:   return "vm-reachable";
    case SSE_EV_STATUS_CHANGED: return "status-changed";
    case SSE_EV_BSOD:           return "bsod";
    case SSE_EV_SHADOW_MANAGER: return "shadow-manager";
    case SSE_EV_GET_OUT:        return "get-out";
    case SSE_EV_UNKNOWN:        return "unknown";
    }
    return "unknown";
}

bool sse_event_parse(const char *json, int len, sse_event *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!json) return false;

    const size_t n = len > 0 ? (size_t)len : strlen(json);
    if (n == 0) return false;

    json_error_t err;
    /* DISABLE_EOF_CHECK so a line with trailing whitespace - which every SSE
     * frame has - parses instead of being rejected on its own newline. */
    json_t *root = json_loadb(json, n, JSON_DISABLE_EOF_CHECK, &err);
    if (!root) return false;
    if (!json_is_object(root)) { json_decref(root); return false; }

    json_t *t = json_object_get(root, "type");
    out->type = json_is_string(t) ? type_of(json_string_value(t))
                                  : SSE_TYPE_UNKNOWN;

    json_t *e = json_object_get(root, "event");
    if (json_is_string(e)) {
        copy_into(out->event_name, sizeof out->event_name, json_string_value(e));
        out->event = kind_of(json_string_value(e));
    }

    json_t *d = json_object_get(root, "data");
    if (json_is_object(d)) {
        /* `data.type` is the sub-type for shadow-manager
         * (encoding_is_ready, display-is-ready, install-status) and is often
         * the useful word for the others too. */
        text_of(out->sub, sizeof out->sub, json_object_get(d, "type"));

        /* Whichever of the three a given event chose to use. Checked in that
         * order because `status` is the one `status-changed` carries and it
         * is the one a UI would show. */
        json_t *v = json_object_get(d, "status");
        if (!v) v = json_object_get(d, "state");
        if (!v) v = json_object_get(d, "value");
        text_of(out->detail, sizeof out->detail, v);
    } else if (d) {
        /* A scalar `data`, which the taxonomy does not describe but which a
         * server is free to send. */
        text_of(out->detail, sizeof out->detail, d);
    }

    json_decref(root);
    return true;
}
