/**
 * auth.h — per-session HMAC-SHA256 challenge/response authentication.
 *
 * Session lifecycle (single BLE peripheral connection):
 *
 *   CONNECTED          connect                 (host task)
 *   ──────► auth_on_connect()
 *              │ set state = IDLE, clear challenge/attempt counters
 *              ▼
 *   IDLE ──[client writes 0x01 BEGIN]──► CHALLENGE_ISSUED
 *              │ auth_handle_begin() generates 16 random bytes, notifies
 *              │ the challenge back to the client on AUTH characteristic
 *              ▼
 *   CHALLENGE_ISSUED ──[client writes 0x02 RESPONSE, 32B HMAC]──► verify
 *              │ HMAC-SHA256(device_secret, challenge) == response ?
 *              │  yes → OK            → authenticated=true
 *              │  no  → fail_count++  → state = IDLE
 *              │        3 failures → drop the BLE connection, 30-s cooldown
 *              ▼
 *   AUTHED    (authenticated=true for the rest of this connection)
 *
 * device_secret[32] = HMAC-SHA256(MASTER_KEY, ble_mac[6])
 * MASTER_KEY lives in main/secrets.h (gitignored).
 *
 * The same AUTH characteristic is used for client→server command writes
 * (BEGIN / RESPONSE) and for server→client notifies (CHALLENGE / STATUS).
 * Opcode table:
 *
 *   client → server                server → client
 *     0x01 BEGIN (no args)            0x81 CHALLENGE (16 B nonce)
 *     0x02 RESPONSE (32 B HMAC)       0x82 STATUS (1 B: 0=OK, 1=FAIL, 2=LOCKED)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute the device secret from the BLE MAC.  Called once from ble_mgmt
 * initialisation.  The secret is kept in a static buffer inside auth.c.
 */
void auth_init(const uint8_t ble_mac[6]);

/** Reset per-session state.  Called from the mgmt GAP-connect hook. */
void auth_on_connect(void);

/** Clear session state on disconnect. */
void auth_on_disconnect(void);

/**
 * Dispatch an incoming write to the AUTH characteristic.
 *
 *   data,len: raw bytes written by the client (opcode + arguments)
 *   notify_cb: invoked with the response to notify back to the client
 *              (opcode + arguments); caller is responsible for serving the
 *              notification via ble_gatts_notify_custom().
 *
 * Returns true if the write was well-formed; false if malformed (ATT layer
 * should reject with BLE_ATT_ERR_UNLIKELY).  Whether the challenge itself
 * succeeded is communicated in the notified 0x82 STATUS payload.
 */
typedef void (*auth_notify_cb_t)(const uint8_t *data, size_t len);

bool auth_handle_write(const uint8_t *data, size_t len,
                       auth_notify_cb_t notify);

/**
 * True if the current connection has completed a successful handshake.
 * CFG_CONTROL and DFU_CONTROL should refuse writes (return
 * BLE_ATT_ERR_INSUFFICIENT_AUTHEN) when this is false and cfg_values()
 * says auth_required != 0.
 */
bool auth_is_authenticated(void);

/**
 * True while the peer is in a post-failure cooldown window (3 fails in a row).
 * ble_mgmt can use this to drop the connection immediately on next write.
 */
bool auth_is_locked_out(void);

#ifdef __cplusplus
}
#endif
