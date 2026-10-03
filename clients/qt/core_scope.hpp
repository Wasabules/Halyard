/* core_scope - RAII over the core's C out-structs.
 *
 * === QT2 2026-10-03 — WHY THIS IS THE FIRST THING THE QT CLIENT NEEDED =====
 *
 * Every function in `core/services/` that fills a struct comes with a matching
 * `_free()`: `vminfo_free`, `vmconn_free`, `launcher_token_free`,
 * `proximus_credentials_free`, `vmcaps_free`, `turn_servers_free`,
 * `gapinfo_free`, `oauth_state_free`, `oauth_discovery_free`,
 * `oauth_device_init_free`, `vmpage_free`. Eleven of them, and the bootstrap
 * touches nine in sequence.
 *
 * In C that is a `goto fin`. In C++ it is a leak waiting for the first early
 * `return` — and a bootstrap IS a chain of early returns, one per step that can
 * fail with an HTTP code. The Borealis client gets away with it because its
 * bootstrap is one long function written in C style; a Qt client with steps,
 * signals and exceptions would not.
 *
 * So: one scope guard per struct, freed on the way out of the block whichever
 * way control leaves it. No ownership subtleties, no shared pointers, no
 * templates to read — a struct, a destructor, and a `get()`.
 *
 * NOT a generic `unique_ptr` with a custom deleter: these are stack structs
 * whose fields are owned pointers, not heap objects. `_free()` releases the
 * FIELDS and leaves the struct; a `unique_ptr` would model it wrongly and the
 * first reader would try to `delete` it.
 */
#pragma once

#include <QString>

extern "C" {
#include "core/services/launcher.h"
#include "core/services/oauth.h"
#include "core/services/proximus.h"
#include "core/services/tinag.h"
}

namespace halyard {

/* The one pattern, written once.
 *
 * `T` is the core struct, `F` its `_free`. The struct is zero-initialised,
 * handed out by reference so a core function can fill it, and freed exactly
 * once — including when the block is left by an exception or an early return.
 *
 * Copying is forbidden: two guards over one struct would free it twice, and
 * `_free()` on freed fields is a double free, not a no-op. */
template <typename T, void (*F)(T *)>
class Scoped {
public:
    Scoped() : v_{} {}
    ~Scoped() { F(&v_); }

    Scoped(const Scoped &) = delete;
    Scoped &operator=(const Scoped &) = delete;
    Scoped(Scoped &&) = delete;
    Scoped &operator=(Scoped &&) = delete;

    /* For passing to a core function that fills it. */
    T *out() { return &v_; }
    /* For reading afterwards. */
    const T &get() const { return v_; }
    T &get() { return v_; }
    const T *operator->() const { return &v_; }
    T *operator->() { return &v_; }

private:
    T v_;
};

/* The eleven, named so a call site reads as prose:
 *     ScopedGapInfo gap;
 *     if (!tinag_get_datacenter(email, gap.out(), &http)) return;
 *     use(gap->launcher_api_url);
 *
 * If core gains a twelfth, the compiler will not notice — so the rule is
 * written here rather than only in a habit: a core struct with a `_free()`
 * gets a line in this list before it gets a call site in the Qt client. */
using ScopedGapInfo        = Scoped<GapInfo, gapinfo_free>;
using ScopedDiscovery      = Scoped<OidcDiscovery, oauth_discovery_free>;
using ScopedDeviceInit     = Scoped<DeviceGrantInit, oauth_device_init_free>;
using ScopedAuthState      = Scoped<ShadowAuthState, oauth_state_free>;
using ScopedVmPage         = Scoped<VmPage, vmpage_free>;
using ScopedVmInfo         = Scoped<VmInfo, vminfo_free>;
using ScopedVmConn         = Scoped<VmConnectionInfo, vmconn_free>;
using ScopedSessionToken   = Scoped<LauncherSessionToken, launcher_token_free>;
using ScopedProxCreds      = Scoped<ProximusCredentials, proximus_credentials_free>;
using ScopedVmCaps         = Scoped<VmCapabilities, vmcaps_free>;
using ScopedSubscription   = Scoped<Subscription, subscription_free>;   /* ACC1 */
using ScopedTurnServers    = Scoped<TurnServers, turn_servers_free>;
using ScopedProxLauncher   = Scoped<ProximusLauncherSession, proximus_launcher_session_free>;
using ScopedProxMain       = Scoped<ProximusMainSession, proximus_main_session_free>;

/* A core string field into a QString, treating NULL as empty.
 *
 * Almost every field above is documented "may be NULL", and
 * `QString::fromUtf8(nullptr)` is fine but `std::string(nullptr)` is not — so
 * the conversion is written once here rather than guarded at forty call sites.
 */
inline QString str(const char *s) { return s ? QString::fromUtf8(s) : QString(); }

}  // namespace halyard
