/* demo.hpp - DEMO-1 (2026-09-26): the app with fictional data, for screenshots.
 *
 * The README and the website want pictures of the real interface: the home
 * screen, the machine list, the settings, a stream with its overlay. Taken from
 * a real session they would carry an account, machine names, a datacenter and
 * addresses. So SHADOW_DEMO=1 replaces every network call with fictional data at
 * the four places the screens get theirs:
 *
 *   boot        no connectivity check, no datacenter lookup, no OAuth - it
 *               walks the same status lines and lands on the machine list
 *               (SHADOW_DEMO_PAIRING=1 stops on the sign-in code screen instead)
 *   VM list     three fictional machines and a fictional plan
 *   connecting  the seven steps, timed like a real connection
 *   stream      the app's own background image as the picture, 60 frames a
 *               second, and plausible counters for the overlay
 *
 * Nothing is read from or written to the account: no token is loaded, none is
 * saved, no request leaves. Off by default, and read from the real environment
 * like any toggle (env.txt works too).
 */
#pragma once

#include <functional>
#include <string>

namespace demo {

bool enabled();        /* SHADOW_DEMO=1 */
bool holdPairing();    /* SHADOW_DEMO_PAIRING=1: the boot stops on the sign-in code */

/* Fictional values the screens display. */
const char *datacenter();
const char *pairingCode();
const char *pairingUrl();
void account(std::string &plan, std::string &drive);
void fillVms();        /* replaces ShadowApp::instance().vms */

/* The picture and the counters of a stream, until `abandon()` says stop. Runs
 * on the caller's thread; the stream screen must already be pushed. */
void runStream(const std::function<bool()> &abandon);

}  // namespace demo
