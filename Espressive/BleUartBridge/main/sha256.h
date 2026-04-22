/**
 * sha256.h — self-contained SHA-256 + HMAC-SHA256 (RFC 4634 / FIPS 180-4 / RFC 2104).
 *
 * We ship our own implementation because ESP-IDF v6.0 bundles mbedTLS 4.x,
 * which has moved the legacy C hash APIs (mbedtls/sha256.h, mbedtls/md.h)
 * into private headers — only PSA crypto is public, and PSA is overkill for
 * an in-process HMAC challenge check plus a rolling image hash.
 *
 * Both functions work in-place and do not allocate; safe to call from any
 * task context including interrupt-safe code paths (no mutex, no malloc).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All public symbols use a bp_ prefix because the ESP-IDF wpa_supplicant
 * component exports unqualified hmac_sha256()/sha256() symbols of its own
 * (see crypto_mbedtls.c); without the prefix the linker errors out with
 * "multiple definition of hmac_sha256" on the ESP32 build. */

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buf[64];
    size_t   buf_len;
} bp_sha256_ctx_t;

void bp_sha256_init  (bp_sha256_ctx_t *ctx);
void bp_sha256_update(bp_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void bp_sha256_final (bp_sha256_ctx_t *ctx, uint8_t out[32]);

/** One-shot SHA-256. */
void bp_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/** HMAC-SHA256 (one-shot). */
void bp_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

#ifdef __cplusplus
}
#endif
