/* test_errors.c - translating a REST failure into a usable sentence.
 *
 * WHY THIS SUITE EXISTS. This module has one job: bring a CODE together with the
 * STEP it happened at. Getting it wrong crashes nothing and logs nothing - it
 * simply shows the wrong sentence, and the user does the wrong thing. That is
 * exactly the kind of mistake no live session run catches, because every code
 * would have to be provoked on demand.
 *
 * Each check names the REAL case it pins. The two most important were met on
 * 2026-08-29, an hour apart: a 401 that prevented starting a machine, and a 409
 * when opening the main client.
 */
#include "../core/services/errors.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static int same(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* An explanation is ALWAYS usable: a non-null, non-empty key. Returning NULL
 * would display an empty line, that is, a screen that looks broken at the exact
 * moment one is trying to understand. */
static void always_a_sentence(void)
{
    const long CODES[] = { 0, 200, 204, 301, 400, 401, 403, 404, 405, 409, 418,
                           429, 470, 500, 502, 503, 504, 599, 999 };
    const error_net_t RES[] = { ERR_NET_NONE, ERR_NET_DNS,
                                    ERR_NET_UNREACHABLE, ERR_NET_TIMEOUT,
                                    ERR_NET_TLS, ERR_NET_OTHER };
    int fautes = 0;
    for (int et = 0; et < ERR_STEP_COUNT; et++)
        for (unsigned c = 0; c < sizeof CODES / sizeof CODES[0]; c++)
            for (unsigned r = 0; r < sizeof RES / sizeof RES[0]; r++) {
                error_explanation_t x =
                    error_explain((error_step_t)et, CODES[c], RES[r]);
                if (!x.key || !x.key[0]) fautes++;
            }
    CHECK(fautes == 0,
            "every step x code x reason combination returns a non-empty sentence");
}

/* COUNTER-CASE - THE 409 OF 2026-08-29. "I get a stream session 409": a Shadow
 * session had been left open elsewhere. The screen showed "The connection
 * failed. See the log" and, in small type, `HTTP 409`. The cause and the
 * answer were both precise; neither was on screen.
 *
 * The same 409 at STARTUP does not mean that - the machine is simply already
 * starting - and that is the whole reason the `step` parameter exists. If both
 * returned the same sentence, the module would be useless. */
static void conflict(void)
{
    error_explanation_t cli = error_explain(ERR_STEP_CLIENT, 409, ERR_NET_NONE);
    error_explanation_t dem = error_explain(ERR_STEP_START, 409, ERR_NET_NONE);

    CHECK(same(cli.key, "error/409_client"),
            "409 when opening the client = another session holds the machine");
    CHECK(cli.action_key != NULL,
            "... and the screen offers what to do: it is within the user's reach");
    CHECK(cli.reessayable,
            "... and retrying makes sense: the slot frees itself");

    CHECK(!same(dem.key, cli.key),
            "the SAME code at another step does NOT return the same sentence");
    CHECK(same(dem.key, "error/409_starting"),
            "409 at startup = the machine is already starting");
}

/* COUNTER-CASE - THE 401 OF 2026-08-29. "I cannot even start a machine any
 * more, I get a 401 error". Retrying could not work: the token was no longer
 * accepted. Offering "retry" in that case is worse than offering nothing - it
 * sends the user to press a button ten times that cannot succeed. */
static void token(void)
{
    for (int et = 0; et < ERR_STEP_COUNT; et++) {
        error_explanation_t x = error_explain((error_step_t)et, 401, ERR_NET_NONE);
        CHECK(same(x.key, "error/401"),
                "401 means the same thing at every step: the token has expired");
        CHECK(!x.reessayable,
                "401 is NOT retryable - the screen must not offer a useless button");
    }
    /* 403 either: it is a server decision, not a fluke. */
    CHECK(!error_explain(ERR_STEP_CLIENT, 403, ERR_NET_NONE).reessayable,
            "403 is not retryable: the account does not have the right");
    /* And offering nothing beats offering a useless action. */
    CHECK(error_explain(ERR_STEP_CLIENT, 403, ERR_NET_NONE).action_key == NULL,
            "403 offers NO action: offering the useless spends trust");
}

/* COUNTER-CASE - THE 470. A Shadow-SPECIFIC code, documented as "not ready
 * yet" and explicitly "to be treated as a retry, not as a failure"
 * (memory/project_shadow_protocol_har.md). Filing it among the generic 4xx would
 * display "the server refused" for a machine that is starting normally, and
 * would give `retryable = false` - hence no button, in the one case where
 * retrying is exactly the right thing. */
static void code_maison(void)
{
    error_explanation_t x = error_explain(ERR_STEP_ADDRESS, 470, ERR_NET_NONE);
    CHECK(same(x.key, "error/470"), "470 has its own sentence: the machine is still starting");
    CHECK(x.reessayable, "470 is retryable - it is a wait signal, not a refusal");
    CHECK(!same(x.key, error_explain(ERR_STEP_ADDRESS, 400, ERR_NET_NONE).key),
            "470 is not confused with an ordinary 4xx");
}

/* When nothing left at all, the network cause must be readable in the
 * sentence. A "name not found" on console is the documented symptom of a
 * degraded HOS, whose answer is to REBOOT the console - not to retry, not to
 * check one's subscription. */
static void reseau(void)
{
    error_explanation_t dns = error_explain(ERR_STEP_ACCOUNT, 0, ERR_NET_DNS);
    error_explanation_t del = error_explain(ERR_STEP_ACCOUNT, 0, ERR_NET_TIMEOUT);

    CHECK(same(dns.key, "error/dns"), "a name that cannot be found has its own sentence");
    CHECK(same(dns.action_key, "error/action_reboot"),
            "... and its action is rebooting the console (degraded HOS)");
    CHECK(!same(dns.key, del.key),
            "a timeout does not read like a name that cannot be found");

    /* `net` is read ONLY when http == 0: a 500 with a stray network reason is
     * still a 500. Without that rule, a caller that leaves an old CURL code
     * lying around would display "check your network" on a server failure. */
    CHECK(same(error_explain(ERR_STEP_ACCOUNT, 500, ERR_NET_DNS).key,
                 "error/server"),
            "a present HTTP code wins over a network reason left in place");
}

/* The detail shown beside the step: short, bounded, always terminated. It is
 * what you ask someone to read out to you over the phone, so it must contain the
 * number. */
static void detail(void)
{
    char buf[32];

    CHECK(strcmp(error_detail(buf, sizeof buf, 409, ERR_NET_NONE), "HTTP 409") == 0,
            "an HTTP code is displayed as is");
    CHECK(strstr(error_detail(buf, sizeof buf, 0, ERR_NET_TIMEOUT), "delai") != NULL,
            "with no HTTP code, the detail names the network cause");

    /* COUNTER-CASE - THE TOO-SHORT BUFFER. It comes from a stack, often from a
     * `char d[16]`. `snprintf` truncates and terminates; a hand-written version
     * using `strcpy` would write past the end. */
    char court[4];
    const char *r = error_detail(court, sizeof court, 409, ERR_NET_NONE);
    CHECK(r == court && strlen(court) < sizeof court,
            "a too-short buffer is truncated and terminated, never overrun");

    /* Zero size: touch nothing and return a valid string. */
    CHECK(error_detail(court, 0, 409, ERR_NET_NONE)[0] == '\0',
            "zero size: nothing is written, and the return stays displayable");
    CHECK(error_detail(NULL, 32, 409, ERR_NET_NONE)[0] == '\0',
            "null output: an empty result, never a dereference");
}

int main(void)
{
    printf("== REST errors: from a code to a sentence that says what to do ==\n");
    always_a_sentence();
    conflict();
    token();
    code_maison();
    reseau();
    detail();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
