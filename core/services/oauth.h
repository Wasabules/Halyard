// OAuth 2.0 Device Authorization Grant (RFC 8628) for Shadow / Hydra.
// Flow:
//   1. oauth_discover()    - fetch <issuer>/.well-known/openid-configuration
//   2. oauth_device_init() - POST device_authorization_endpoint, gets user_code
//   3. show user_code to the user, who confirms on shadow.tech/device
//   4. oauth_device_poll() - POST token_endpoint in a loop, until success/denied
//   5. ShadowAuthState then holds the bearer and the refresh token

#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    char *device_authorization_endpoint;
    char *token_endpoint;
    char *revocation_endpoint;
    char *userinfo_endpoint;
    char *issuer;
} OidcDiscovery;

void oauth_discovery_free(OidcDiscovery *d);
bool oauth_discover(OidcDiscovery *out, long *http_status);

typedef struct {
    char *device_code;                // secret used for polling
    char *user_code;                  // to be shown to the user
    char *verification_uri;           // ex: https://shadow.tech/device
    char *verification_uri_complete;  // ex: https://shadow.tech/device?user_code=XYZ
    int   expires_in;                 // seconds before the overall expiry
    int   interval;                   // seconds between two polls
} DeviceGrantInit;

void oauth_device_init_free(DeviceGrantInit *g);
bool oauth_device_init(const OidcDiscovery *d, DeviceGrantInit *out, long *http_status);

typedef struct {
    char *access_token;
    char *refresh_token;   // may be NULL
    char *id_token;        // may be NULL
    char *token_type;      // typiquement "Bearer"
    time_t expires_at;     // Unix epoch local
} ShadowAuthState;

void oauth_state_free(ShadowAuthState *s);

// Result of one poll:
typedef enum {
    OAUTH_POLL_PENDING       = 0,    // keep waiting
    OAUTH_POLL_SUCCESS       = 1,    // out filled in with the tokens
    OAUTH_POLL_SLOW_DOWN     = 2,    // server demande de polling moins vite
    OAUTH_POLL_DENIED        = -1,   // the user refused
    OAUTH_POLL_EXPIRED       = -2,   // device_code expired
    OAUTH_POLL_NETWORK_ERROR = -3,   // HTTP or network error
    OAUTH_POLL_OTHER_ERROR   = -4,
} OAuthPollResult;

OAuthPollResult oauth_device_poll(const OidcDiscovery *d, const char *device_code,
                                   const char *client_id, ShadowAuthState *out);

// Refreshes an access_token through the refresh_token. Updates `s` in place.
bool oauth_refresh(const OidcDiscovery *d, const char *client_id, ShadowAuthState *s);

// Persist / load into /switch/halyard/refresh_token.
// UX3 B1 2026-05-18: format v2 is obfuscated (XOR keystream). Automatic
// backward compatibility with v1 plaintext (= legacy tokens are read
// correctly).
bool oauth_save_refresh(const ShadowAuthState *s);

/* === CHANGING THE TOKEN'S FORM, WITHOUT CHANGING THE TOKEN ===
 *
 * Reads the stored refresh token and writes it straight back. Which FORMAT it
 * lands in is decided by whether the application lock is open at that moment
 * (see the magic bytes in oauth.c), so this one call is what moves a token
 * between the weakly-obfuscated form and the sealed one.
 *
 * Call it at the two moments the answer changes:
 *   - just after the FIRST lock method is armed, so the token stops being
 *     readable from a computer immediately rather than at the next rotation,
 *     which may be days away;
 *   - just BEFORE the lock is removed, while the master key can still open it -
 *     afterwards nothing could, and turning the lock off would throw the
 *     session away.
 *
 * Returns true when there was a token and it was rewritten. No token at all
 * returns false and is not an error: there is simply nothing to convert. */
bool oauth_reencrypt_refresh(void);

/* Takes the token OUT of the sealed form, for when the lock is being removed.
 *
 * The three steps - read while the key is still loaded, forget the key, write
 * back - are one call because the order between them is the whole thing, and it
 * is the kind of order a later edit reverses without noticing. Getting it wrong
 * leaves a file nothing in the world can decrypt: the lock gone and the session
 * with it, for having turned off a setting.
 *
 * Returns true when a token was converted. */
bool oauth_unseal_refresh(void);
bool oauth_load_refresh(char **refresh_out);   // allocated, to be freed

/* AUTH9 2026-10-03 - sign out: drop the stored refresh token.
 *
 * There was no way out. The token is written at every successful sign-in and
 * rotated at every refresh, so once a machine had been paired it stayed paired
 * for ever as far as the application was concerned - changing account meant
 * knowing where the file lived and deleting it by hand.
 *
 * The bytes are OVERWRITTEN before the file is unlinked. Not a secure-erase
 * claim (an SD card's wear levelling may well keep the old block, and a
 * journalling filesystem certainly can), but it costs one write and it defeats
 * the case that actually happens: the file being undeleted, or read out of
 * free space, on a card that was handed to someone else.
 *
 * Returns true when there is no token left afterwards - including when there
 * was none to begin with, because that is the state the caller asked for. */
bool oauth_forget_refresh(void);

/* === AUTH10 2026-10-03 — SIGNING OUT MUST REACH THE SERVER ================
 *
 * `oauth_forget_refresh` deletes the token from this machine, which is what
 * someone sitting at it can see and all the application was doing. The token
 * itself stayed VALID at Shadow: anybody holding a copy - a backup, an old
 * SD card, a log that should never have had it - could keep refreshing it
 * indefinitely, and "sign out" had told them nothing.
 *
 * The discovery document has carried `revocation_endpoint` since the first
 * version of this file and nothing in the repository ever read it.
 *
 * RFC 7009: POST token=<the token>&token_type_hint=refresh_token, and the
 * server answers 200 for a token it revoked AND for one it never knew - the
 * two are deliberately indistinguishable, so a true return means "the server
 * was told", never "the token existed".
 *
 * Revoking the REFRESH token is what matters: an access token expires on its
 * own within the hour, and the refresh token is the one that lives for
 * months. Pass the refresh token.
 *
 * Returns false when the endpoint is missing (an authorisation server is not
 * required to offer one), the network failed, or the server refused. The
 * caller should still forget the token locally either way - a sign-out that
 * left the credential on disk because the network was down would be the
 * worse failure of the two. */
bool oauth_revoke_token(const OidcDiscovery *d, const char *client_id,
                        const char *token);

// UX3 B5 2026-05-18: checks whether expiry < now + threshold_sec (= a refresh
// is advised). Returns false when the state is empty or expires_at is not
// set.
bool oauth_token_needs_refresh(const ShadowAuthState *s, int threshold_sec);
