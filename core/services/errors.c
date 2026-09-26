/* errors - see errors.h for the why. Pure functions, no allocation. */
#include "errors.h"

#include <stdio.h>

/* A local factory: it avoids repeating the three fields in every case, and
 * above all it makes what is RETRYABLE visible at a glance. */
static error_explanation_t e(const char *key, const char *action, bool reessayable)
{
    error_explanation_t x;
    x.key = key;
    x.action_key = action;
    x.reessayable = reessayable;
    return x;
}

error_explanation_t error_explain(error_step_t step, long http,
                                      error_net_t reseau)
{
    /* --- The request never left ---------------------------------------- */
    if (http == 0) {
        switch (reseau) {
            case ERR_NET_DNS:
                /* On console this is the most frequent symptom of a degraded HOS:
                 * the resolver answers badly, often on the AAAA records. A
                 * reboot is the real answer, and it is documented
                 * (feedback_switch_hos_degraded.md). */
                return e("error/dns", "error/action_reboot", true);
            case ERR_NET_UNREACHABLE:
                return e("error/unreachable", "error/action_network", true);
            case ERR_NET_TIMEOUT:
                return e("error/timeout", "error/action_retry", true);
            case ERR_NET_TLS:
                return e("error/tls", "error/action_clock", true);
            default:
                return e("error/network", "error/action_network", true);
        }
    }

    /* --- Codes that mean the same thing everywhere ---------------------- */

    /* 401 - the token is no longer accepted. This is the error one meets after
     * several days without launching the application; it is resolved by signing
     * in again, never by another attempt. */
    if (http == 401) return e("error/401", "error/action_relogin", false);

    /* 403 - the account does not have the right. Retrying will not help: it is a
     * server decision, not a fluke. */
    if (http == 403) return e("error/403", NULL, false);

    /* 429 - too many requests. The only case where "wait" is the complete
     * answer, and saying so avoids pressing ten times, which makes it worse. */
    if (http == 429) return e("error/429", "error/action_wait", true);

    /* 470 - a Shadow-SPECIFIC code: "not ready yet". This is NOT an error, it
     * is the wait signal of the address polling
     * (memory/project_shadow_protocol_har.md). If it reaches here, the polling
     * timed out; the server did not refuse. */
    if (http == 470) return e("error/470", "error/action_retry", true);

    if (http >= 500 && http <= 599)
        return e("error/server", "error/action_later", true);

    /* --- Codes whose meaning depends on the step ------------------------ */

    switch (step) {
    case ERR_STEP_CLIENT:
        /* 409 HERE, AND ONLY HERE, means "another session already occupies the
         * machine". This is the case that motivated this module: the generic
         * message did not say what to do, while the answer is simple and
         * entirely within the user's reach. The slot also frees itself after
         * about a minute - saying so avoids closing an application that has
         * nothing to do with it. */
        if (http == 409) return e("error/409_client", "error/action_409", true);
        if (http == 404) return e("error/client_404", "error/action_retry", true);
        break;

    case ERR_STEP_START:
        /* 409 at startup: the machine is already starting. This is not a failure,
         * it is a race with ourselves. */
        if (http == 409) return e("error/409_starting", "error/action_wait", true);
        if (http == 404) return e("error/vm_404", NULL, false);
        break;

    case ERR_STEP_ADDRESS:
        /* Here, any 4xx means in practice "not ready yet": the polling gave up
         * before the machine answered. */
        if (http >= 400 && http <= 499)
            return e("error/address", "error/action_retry", true);
        break;

    case ERR_STEP_PERMISSIONS:
        if (http == 404) return e("error/vm_404", NULL, false);
        break;

    case ERR_STEP_INVENTORY:
    case ERR_STEP_ACCOUNT:
        if (http == 404) return e("error/account_404", "error/action_relogin", false);
        break;

    default:
        break;
    }

    if (http == 404) return e("error/404", NULL, false);
    if (http >= 400 && http <= 499) return e("error/refused", NULL, false);

    /* A 2xx or 3xx reaches here when the caller treated the reply as a failure
     * for another reason - unreadable body, missing field. We say so as is
     * rather than inventing a network cause. */
    return e("error/reply", "error/action_retry", true);
}

const char *error_detail(char *out, unsigned size, long http,
                          error_net_t reseau)
{
    if (!out || size == 0) return "";

    if (http != 0) {
        snprintf(out, size, "HTTP %ld", http);
        return out;
    }

    const char *m;
    switch (reseau) {
        case ERR_NET_DNS:         m = "nom introuvable";   break;
        case ERR_NET_UNREACHABLE: m = "injoignable";       break;
        case ERR_NET_TIMEOUT:       m = "delai depasse";     break;
        case ERR_NET_TLS:         m = "chiffrement";       break;
        default:                     m = "echec";             break;
    }
    snprintf(out, size, "reseau : %s", m);
    return out;
}
