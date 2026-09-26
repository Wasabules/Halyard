/* errors - turning a REST failure into a sentence that says what to do.
 *
 * === WHY THIS MODULE EXISTS (S82, 2026-08-29) ===
 *
 * The connection screen displayed "The connection failed. See the log for
 * details." and, in small type beside the step, `HTTP 409`. Together the two
 * say nothing to someone who wants to play:
 *
 *   - "see the log" requires a computer, a cable, and knowing where to look. On
 *     a console that is a dead end;
 *   - `409` is exact and unusable. It has a precise cause and a precise answer -
 *     another Shadow session is open elsewhere, it has to be closed - and
 *     nothing on screen said either.
 *
 * This module does the one thing that was missing: bring the CODE together with
 * the STEP it happened at, and return the matching sentence. The same 409 does
 * not mean the same thing while creating a client and while starting a machine.
 *
 * === WHAT IT IS, AND WHAT IT IS NOT ===
 *
 * It is a PURE FUNCTION: no allocation, no I/O, no state. It returns translation
 * keys, never text - the text lives in the catalogues, like the rest of the UI.
 * It is therefore checkable offline, and `tests/test_errors.c` pins the cases
 * this repo has actually met.
 *
 * It does NOT relay the server's message. The temptation is strong - it is right
 * there, in the JSON body - but it is in English, written for a developer
 * ("vm not started yet: instance is in state PENDING"), and showing it would
 * replace an incomprehensible code with an incomprehensible sentence. The raw
 * code stays displayed in small type beside the step: that is what you ask
 * someone to read out to you over the phone.
 */
#ifndef SHADOW_ERREURS_H
#define SHADOW_ERREURS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What we were doing. This is half the information: a 409 while creating the
 * main client means "another session is open"; the same 409 elsewhere does
 * not. */
typedef enum {
    ERR_STEP_UNKNOWN = 0,
    ERR_STEP_ACCOUNT,          /* jeton, identification */
    ERR_STEP_INVENTORY,      /* listing the machines */
    ERR_STEP_START,       /* starting the machine */
    ERR_STEP_ADDRESS,         /* waiting for its address */
    ERR_STEP_PERMISSIONS,     /* the streaming tokens */
    ERR_STEP_CLIENT,          /* opening a streaming client */
    ERR_STEP_STREAM,            /* bootstrapping the stream itself */
    ERR_STEP_COUNT
} error_step_t;

/* Why the request never left. `http == 0` with no network reason stays
 * possible - a failure we know nothing about - and then returns the generic
 * sentence rather than an invented cause. */
typedef enum {
    ERR_NET_NONE = 0,      /* the request left; look at `http` */
    ERR_NET_DNS,            /* nom introuvable */
    ERR_NET_UNREACHABLE,    /* no route, connection refused */
    ERR_NET_TIMEOUT,          /* delai depasse */
    ERR_NET_TLS,            /* poignee de main chiffree echouee */
    ERR_NET_OTHER
} error_net_t;

typedef struct {
    /* i18n key of the explanation, in one sentence. Never null. */
    const char *key;
    /* i18n key of what can be done, or NULL when there is nothing to offer.
     * NULL is a legitimate result: offering a useless action spends the trust
     * we will need next time. */
    const char *action_key;
    /* Does a fresh attempt have a chance of succeeding WITHOUT anything
     * changing? False for a permission refusal: retrying will not help, and the
     * screen must not offer a button that cannot work. */
    bool reessayable;
} error_explanation_t;

/* `http` = the HTTP code, or 0 when the request never left.
 * `net` is only read when `http == 0`. */
error_explanation_t error_explain(error_step_t step, long http,
                                      error_net_t reseau);

/* The code, for the detail line beside the step: "HTTP 409",
 * "reseau : delai depasse". Written into `out` (bounded, always terminated).
 * Returns `out`. */
const char *error_detail(char *out, unsigned size, long http,
                          error_net_t reseau);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_ERREURS_H */
