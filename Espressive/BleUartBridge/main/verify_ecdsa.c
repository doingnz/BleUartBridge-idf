/**
 * verify_ecdsa.c — ECDSA P-256 verify (FIPS 186-4 § 6.4).
 *
 * Given a public key Q on the P-256 curve, a 32-byte message digest e, and a
 * signature (r, s), the signature is valid iff:
 *
 *   0 < r < n   and   0 < s < n
 *   w  = s^(-1) mod n
 *   u1 = e * w mod n
 *   u2 = r * w mod n
 *   R  = u1·G + u2·Q
 *   R is not the point at infinity
 *   R.x mod n  ==  r
 *
 * We rely on mbedTLS's ECP / MPI primitives, both of which ESP-IDF's mbedTLS
 * port exposes through its "private identifiers" shim.  We do NOT use
 * mbedtls_ecdsa_verify() directly because its header has been moved into
 * the private tree in mbedTLS 4.x and isn't reachable through a public
 * include path in IDF v6.0.
 */
#include "verify_ecdsa.h"

#include <string.h>

/* Grant access to mbedTLS 4.x private struct members (X, Y, Z on ecp_point,
 * among others).  By default these are renamed to private_X via the
 * MBEDTLS_PRIVATE(member) macro in mbedtls/private_access.h; defining
 * MBEDTLS_ALLOW_PRIVATE_ACCESS BEFORE any mbedTLS include turns the rename
 * off and restores the pre-4.x names we use below.  Must stay the first
 * mbedTLS-related directive in this file.  */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "esp_log.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"

static const char *TAG = "VERIFY";

/* Convenience: bail out of verify_ecdsa_p256 on any mbedTLS error. */
#define CHK(expr)                                                 \
    do {                                                          \
        int _rc = (expr);                                         \
        if (_rc != 0) {                                           \
            ESP_LOGD(TAG, "%s -> %d at %s:%d", #expr, _rc,        \
                     __FILE__, __LINE__);                         \
            ok = false;                                           \
            goto cleanup;                                         \
        }                                                         \
    } while (0)

bool verify_ecdsa_p256(const uint8_t pubkey[65],
                       const uint8_t hash[32],
                       const uint8_t sig[64])
{
    bool ok = true;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q, R;
    mbedtls_mpi r, s, e, w, u1, u2, Rx_mod_n, one;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_point_init(&R);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&w); mbedtls_mpi_init(&u1); mbedtls_mpi_init(&u2);
    mbedtls_mpi_init(&Rx_mod_n); mbedtls_mpi_init(&one);

    CHK(mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1));
    CHK(mbedtls_ecp_point_read_binary(&grp, &Q, pubkey, 65));
    CHK(mbedtls_ecp_check_pubkey(&grp, &Q));

    CHK(mbedtls_mpi_read_binary(&r, sig,       32));
    CHK(mbedtls_mpi_read_binary(&s, sig + 32,  32));
    CHK(mbedtls_mpi_read_binary(&e, hash,      32));
    CHK(mbedtls_mpi_lset(&one, 1));

    /* Scalar-range checks: 1 <= r, s <= n-1 */
    if (mbedtls_mpi_cmp_mpi(&r, &one)    < 0)  { ok = false; goto cleanup; }
    if (mbedtls_mpi_cmp_mpi(&s, &one)    < 0)  { ok = false; goto cleanup; }
    if (mbedtls_mpi_cmp_mpi(&r, &grp.N)  >= 0) { ok = false; goto cleanup; }
    if (mbedtls_mpi_cmp_mpi(&s, &grp.N)  >= 0) { ok = false; goto cleanup; }

    /* w = s^(-1) mod n */
    CHK(mbedtls_mpi_inv_mod(&w, &s, &grp.N));

    /* u1 = e * w mod n */
    CHK(mbedtls_mpi_mul_mpi(&u1, &e, &w));
    CHK(mbedtls_mpi_mod_mpi(&u1, &u1, &grp.N));

    /* u2 = r * w mod n */
    CHK(mbedtls_mpi_mul_mpi(&u2, &r, &w));
    CHK(mbedtls_mpi_mod_mpi(&u2, &u2, &grp.N));

    /* R = u1*G + u2*Q */
    CHK(mbedtls_ecp_muladd(&grp, &R, &u1, &grp.G, &u2, &Q));

    if (mbedtls_ecp_is_zero(&R)) { ok = false; goto cleanup; }

    /* v = R.x mod n;  valid iff v == r */
    CHK(mbedtls_mpi_mod_mpi(&Rx_mod_n, &R.X, &grp.N));
    if (mbedtls_mpi_cmp_mpi(&Rx_mod_n, &r) != 0) ok = false;

cleanup:
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&w); mbedtls_mpi_free(&u1); mbedtls_mpi_free(&u2);
    mbedtls_mpi_free(&Rx_mod_n); mbedtls_mpi_free(&one);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_group_free(&grp);

    return ok;
}
