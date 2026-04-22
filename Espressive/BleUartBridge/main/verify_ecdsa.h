/**
 * verify_ecdsa.h — ECDSA-P256 signature verification for a pre-hashed message.
 *
 * FIPS 186-4 § 6.4, implemented on top of mbedTLS's ECP and big-int primitives
 * (which are the public surface exposed by ESP-IDF v6.0's mbedTLS 4.x port,
 * even though mbedtls/ecdsa.h has been privatised).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param pubkey   65-byte uncompressed SEC1 point: 0x04 || X[32] || Y[32]
 * @param hash     message digest (32 bytes, SHA-256)
 * @param sig      signature as 64 bytes: r[32] || s[32], big-endian
 *
 * @return true if the signature is valid under the given public key.
 *         false on any failure — invalid point, malformed scalars, mismatch.
 */
bool verify_ecdsa_p256(const uint8_t pubkey[65],
                       const uint8_t hash[32],
                       const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
