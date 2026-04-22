/**
 * dfu.h  —  OTA firmware update over the BP+ Mgmt DFU_CONTROL / DFU_DATA
 * characteristics.
 *
 * State machine
 * ─────────────
 *   IDLE ──START─►  RECEIVING  ──(all bytes)──►  READY_TO_APPLY  ──APPLY──► (reboot)
 *      ▲             │ │                              │
 *      │             │ └─ABORT / timeout / bad seq ───┤
 *      └─────────────┴──────────────────────────────────┘
 *
 * Wire protocol
 * ─────────────
 * Client → server on DFU_CONTROL (write):
 *   0x01 START     u32 size_le, u8 sha256[32], u32 version_le, u8 sig[64]
 *   0x03 VERIFY    (no args)   — check hash (and, from M6, signature)
 *   0x04 APPLY     (no args)   — set boot partition and reboot
 *   0x05 ABORT     (no args)
 *   0x06 STATUS    (no args)
 *
 * Client → server on DFU_DATA (write_no_rsp):
 *   u16 seq_le, u8 payload[…]
 *
 * Server → client notifies on DFU_CONTROL:
 *   0x81 START_RSP    u8 status, u16 max_chunk_le, u16 window_le  (if status==0)
 *                     u8 status                                   (otherwise)
 *   0x82 CHUNK_ACK    u16 next_seq_le, u32 bytes_received_le
 *   0x83 VERIFY_RSP   u8 status
 *   0x84 APPLY_RSP    u8 status
 *   0x85 ABORT_EVT    u8 reason
 *   0x86 STATUS_RSP   u8 state, u32 received_le, u32 expected_le
 *
 * Status / reason codes (1 byte):
 *   0  OK
 *   1  NOT_AUTHED        handshake incomplete and auth_required is set
 *   2  DFU_DISABLED      cfg TLV dfu_enabled == 0
 *   3  BUSY              state != IDLE when START issued
 *   4  BAD_STATE         e.g. APPLY before VERIFY, CHUNK before START
 *   5  BAD_ARG           malformed message (bad size, truncation, etc.)
 *   6  SIZE_TOO_BIG      image larger than the OTA partition
 *   7  OTA_ERR           esp_ota_* failure (flash write, begin, end)
 *   8  HASH_MISMATCH     SHA-256 over received bytes != START's hash
 *   9  SIG_MISMATCH      ECDSA verify failed (M6)
 *  10  ABORTED           client ABORT received
 *  11  TIMEOUT           no DFU_DATA for DFU_CHUNK_TIMEOUT_MS
 *  12  OUT_OF_SEQ        wrong seq (duplicate, skip) or oversized chunk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DFU_IDLE             = 0,
    DFU_RECEIVING        = 1,
    DFU_READY_TO_APPLY   = 2,
    DFU_APPLYING         = 3,
    DFU_ABORTED          = 4,
} dfu_state_t;

typedef void (*dfu_notify_cb_t)(const uint8_t *data, size_t len);

/**
 * Initialise the DFU module.  Creates the worker task and the internal chunk
 * queue.  notify_ctrl is invoked to deliver DFU_CONTROL notifications; the
 * function may be called from any task context.
 */
void dfu_init(dfu_notify_cb_t notify_ctrl);

/** Reset to IDLE on BLE disconnect — mid-transfer state is discarded. */
void dfu_on_disconnect(void);

/**
 * Dispatch a DFU_CONTROL write.  Returns one of the ATT error codes (or 0)
 * to propagate to the client; payload-specific statuses are sent via the
 * notify callback.
 */
int dfu_handle_control(const uint8_t *data, size_t len);

/**
 * Dispatch a DFU_DATA write.  Returns 0 if accepted, non-zero to ask ATT to
 * return an error to the client (write_no_rsp clients won't observe this).
 * The data is copied internally before the function returns.
 */
int dfu_handle_data(const uint8_t *data, size_t len);

/** True while state != IDLE.  Used to suspend the NUS bridge during DFU. */
bool dfu_is_active(void);

dfu_state_t dfu_get_state(void);

/**
 * Called once at boot by app_main to complete the rollback-enabled OTA
 * workflow: if the running image is in ESP_OTA_IMG_PENDING_VERIFY, waits a
 * short period for the system to prove it works, then calls
 * esp_ota_mark_app_valid_cancel_rollback().  Spawns a helper task.
 */
void dfu_start_health_monitor(void);

#ifdef __cplusplus
}
#endif
