/* authz.hpp - see authz.h for WHY, and for what is decided where. */
#ifndef DEVLINK_AUTHZ_HPP
#define DEVLINK_AUTHZ_HPP

#include <string>

#include "../../../core/services/journal.h"

namespace devlink {

/* Announces the machine at the other end, "host:port". Called by the drain
 * thread the moment the channel opens. Resolves at once from the stored list;
 * otherwise raises the question. */
void offerPeer(const char *peer);

/* May anything at all pass - log or command? */
bool allowed();

/* The peer awaiting an answer, or "" when there is no question. Polled by the
 * UI, which is the only thread allowed to open a dialogue. */
std::string pendingPeer();

/* The owner's answer. `remember` writes it to the card so the question is asked
 * once per machine rather than once per launch. */
void answer(bool yes, bool remember);

/* Forgets the session's decision - used when the channel drops, so a NEW
 * connection from elsewhere is questioned again rather than inheriting the
 * previous machine's yes. */
void forget();

}  // namespace devlink

/* The gate `main.cpp` hands to `journal_set_mirror_gate()`. Outside the
 * namespace and with C linkage, because the journal is C.
 *
 * The header is included rather than the type forward-declared: journal.h
 * typedefs an ANONYMOUS struct, so a `struct journal_mirror_gate_t;` here would
 * name a different, incomplete type and the definition would not match. */
extern "C" const journal_mirror_gate_t *devlink_journal_gate(void);

namespace devlink {

}  // namespace devlink

#endif /* DEVLINK_AUTHZ_HPP */
