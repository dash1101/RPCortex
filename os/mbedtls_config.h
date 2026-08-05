// mbedTLS, cut down to what a package manager on a microcontroller needs.
//
// Explicit rather than "the defaults minus things": MBEDTLS_CONFIG_FILE REPLACES
// the default configuration wholesale, so a file that only lists differences
// silently disables everything it forgot to mention. This list is derived from
// the Pico SDK's own working configuration, which is the one combination known
// to link against pico_mbedtls and lwIP's altcp glue.
//
// The tuning at the bottom is the part that is ours.

/* Workaround for some mbedtls source files using INT_MAX without including limits.h */
#include <limits.h>

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME

#define MBEDTLS_CIPHER_MODE_CBC
// THE CURVES, and this one setting does two jobs that pull in opposite
// directions.
//
// It decides what a certificate may be SIGNED on, and it decides what the
// client OFFERS for the key exchange — mbedtls derives the advertised group
// list from whatever is enabled here, strongest first. There is no way to
// separate them without reaching inside an altcp_tls_config, and lwIP keeps
// that struct private to its own .c file.
//
// So the list is the intersection of what is needed and what is affordable.
//
// Needed: a public CA root is on P-256 or P-384. Trimming to two curves broke
// the bundle immediately and cacerts_test said so — "1 certificate(s) did not
// parse" — because one of the three roots in /os/ca.pem is P-384.
//
// Affordable: the handshake runs in the cyw43 background context, which is an
// INTERRUPT on core 0, and core 0 is the only core that feeds the watchdog.
// Nothing reaches the scheduler while a scalar multiplication runs there, so an
// expensive curve is not a slow connection - it is a reboot, and no timeout in
// the calling task can prevent it because the task is not running either.
//
// Gone: secp521r1 and brainpoolP512r1, the two most expensive things a server
// could have picked; the Brainpool and Koblitz curves, which no public CA uses;
// and 192/224, which nothing has offered in a decade. What is left is what the
// web actually runs on.
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_AES_FEWER_TABLES

/* TLS 1.2 */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

// ECDHE with an RSA CERTIFICATE, which is most of the web.
//
// Without this the client offers exactly two families: static RSA, which every
// modern server refuses because it has no forward secrecy and is gone in TLS
// 1.3, and ECDHE_ECDSA, which needs the server to present an ECDSA
// certificate. So the OS could talk to hosts serving ECDSA and to nothing
// else — raw.githubusercontent.com and api.duckduckgo.com worked, and
// en.wikipedia.org, which serves RSA, had no cipher suite in common and the
// handshake never completed.
//
// That looked like a package bug for four rounds because `search` was the only
// thing reaching an RSA host. `curl` on the same URL from the shell hangs
// identically, which is what finally placed it here rather than in the sandbox.
//
// Everything it needs was already on: ECDH_C, RSA_C, PKCS1_V15,
// X509_CRT_PARSE_C. It is one line because it was one line missing.
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_GCM_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_PLATFORM_MS_TIME_ALT

// --- what this build actually needs ----------------------------------------

// SNI is mandatory in practice: a shared host answers with the wrong
// certificate without it, and raw.githubusercontent.com is exactly such a host.
#ifndef MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#endif

// SHA-384. mbedtls 3.x gates it behind its OWN flag rather than folding it into
// SHA512_C, and the SDK's configuration does not set it — so a certificate
// signed with ecdsa-with-SHA384 fails to parse with "unknown signature
// algorithm". ISRG Root X2 is exactly that, and one unparseable certificate
// made the ENTIRE trust store unusable, because lwIP treats any non-zero parse
// result as total failure.
#ifndef MBEDTLS_SHA384_C
#define MBEDTLS_SHA384_C
#endif

// PEM parsing, so the trusted roots can ship as text on the filesystem and be
// replaced without reflashing when a root rotates.
#ifndef MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#endif
#ifndef MBEDTLS_BASE64_C
#define MBEDTLS_BASE64_C
#endif

// --- record buffers: the biggest RAM decision here -------------------------
//
// Incoming stays at the full 16 KB. TLS lets a server send records up to that
// size and there is no way to refuse one, so a smaller input buffer works
// against most servers right up until it meets one that does not, and then it
// fails looking like a network fault rather than a configuration choice.
//
// Outgoing is cut hard. The only thing ever sent from here is a GET with four
// headers, and 2 KB is already generous for that.
#undef  MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN      16384
#undef  MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN     2048

// Nothing here ever listens, and none of this is reachable from an outbound GET.
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_RENEGOTIATION
