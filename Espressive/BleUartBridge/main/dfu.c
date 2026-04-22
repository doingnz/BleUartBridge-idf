/**
 * dfu.c — OTA firmware update state machine.
 *
 * Two entry points:
 *   dfu_handle_control() — called from the NimBLE host task (GATT write to
 *                          DFU_CONTROL).  Parses the opcode, maintains state.
 *   dfu_handle_data()    — called from the NimBLE host task (GATT write to
 *                          DFU_DATA).  Validates seq + length and posts the
 *                          chunk to s_chunk_q.  Returns immediately.
 *
 * dfu_task (core 1) drains s_chunk_q: updates the rolling SHA-256, calls
 * esp_ota_write(), and notifies CHUNK_ACK back to the client.  This keeps
 * flash I/O off the BLE host task.
 *
 * The DFU_DATA chunk queue is sized to match the protocol's sliding window
 * so the client, if it respects ACK-based flow control, will never overrun
 * our buffer.  Clients using writeWithoutResponse cannot see an ATT error,
 * so under-sizing the queue means silently dropping chunks → hash mismatch.
 */
#include "dfu.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "cfg.h"
#include "auth.h"
#include "sha256.h"
#include "signing_pubkey.h"
#include "verify_ecdsa.h"
#include "ble_mgmt.h"

static const char *TAG = "DFU";

/* ── Tunables ─────────────────────────────────────────────────────────────── */

/* 510 B payload + 2 B seq header = 512 B total, the per-write GATT-value
 * ceiling enforced by Chrome's Web Bluetooth.  Going higher triggers
 * "Value can't exceed 512 bytes" on writeValueWithoutResponse. */
#define DFU_MAX_CHUNK_PAYLOAD   510
#define DFU_WINDOW              4     /* outstanding chunks the client may send */
#define DFU_Q_DEPTH             (DFU_WINDOW + 2)
#define DFU_CHUNK_TIMEOUT_MS    60000
#define DFU_HEALTH_WAIT_MS      30000 /* delay before mark_app_valid          */

/* ── Opcodes ──────────────────────────────────────────────────────────────── */

#define DFU_OP_START     0x01
#define DFU_OP_VERIFY    0x03
#define DFU_OP_APPLY     0x04
#define DFU_OP_ABORT     0x05
#define DFU_OP_STATUS    0x06

#define DFU_RSP_START    0x81
#define DFU_RSP_CHUNK    0x82
#define DFU_RSP_VERIFY   0x83
#define DFU_RSP_APPLY    0x84
#define DFU_EVT_ABORT    0x85
#define DFU_RSP_STATUS   0x86

/* ── Status bytes ─────────────────────────────────────────────────────────── */

#define DFU_ST_OK            0
#define DFU_ST_NOT_AUTHED    1
#define DFU_ST_DISABLED      2
#define DFU_ST_BUSY          3
#define DFU_ST_BAD_STATE     4
#define DFU_ST_BAD_ARG       5
#define DFU_ST_SIZE_TOO_BIG  6
#define DFU_ST_OTA_ERR       7
#define DFU_ST_HASH_MISMATCH 8
#define DFU_ST_SIG_MISMATCH  9
#define DFU_ST_ABORTED      10
#define DFU_ST_TIMEOUT      11
#define DFU_ST_OUT_OF_SEQ   12

/* ── State ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t seq;
    uint16_t len;
    uint8_t  data[DFU_MAX_CHUNK_PAYLOAD];
} dfu_chunk_t;

static volatile dfu_state_t    s_state = DFU_IDLE;
static dfu_notify_cb_t         s_notify_ctrl = NULL;
static QueueHandle_t           s_chunk_q = NULL;

/* Per-session state — reset on START and on transitions back to IDLE. */
static const esp_partition_t  *s_part;
static esp_ota_handle_t        s_ota_handle;
static uint32_t                s_expected_size;
static uint32_t                s_bytes_received;
static uint16_t                s_expected_seq;
static bp_sha256_ctx_t         s_sha;
static uint8_t                 s_expected_sha[32];
static uint8_t                 s_expected_sig[64];       /* used in M6       */
static uint32_t                s_image_version;
static int64_t                 s_last_chunk_us;          /* for timeout      */

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void emit(const uint8_t *buf, size_t n)
{
    if (s_notify_ctrl) s_notify_ctrl(buf, n);
}

static void emit_status(uint8_t op, uint8_t status)
{
    uint8_t r[2] = { op, status };
    emit(r, sizeof(r));
}

static void emit_abort(uint8_t reason)
{
    uint8_t r[2] = { DFU_EVT_ABORT, reason };
    emit(r, sizeof(r));
}

static void reset_to_idle(void)
{
    if (s_state == DFU_RECEIVING || s_state == DFU_READY_TO_APPLY) {
        if (s_part != NULL) esp_ota_abort(s_ota_handle);
    }
    s_part           = NULL;
    s_ota_handle     = 0;
    s_expected_size  = 0;
    s_bytes_received = 0;
    s_expected_seq   = 0;
    s_last_chunk_us  = 0;
    memset(&s_sha, 0, sizeof(s_sha));
    memset(s_expected_sha, 0, sizeof(s_expected_sha));
    memset(s_expected_sig, 0, sizeof(s_expected_sig));
    s_state = DFU_IDLE;

    /* Drain any queued chunks left behind. */
    if (s_chunk_q) {
        dfu_chunk_t drop;
        while (xQueueReceive(s_chunk_q, &drop, 0) == pdTRUE) { /* discard */ }
    }
}

static bool gate_authed(void)
{
    if (!cfg_values()->auth_required) return true;
    return auth_is_authenticated();
}

/* ── Public: disconnect ───────────────────────────────────────────────────── */

void dfu_on_disconnect(void)
{
    if (s_state != DFU_IDLE) {
        ESP_LOGW(TAG, "disconnect during DFU — aborting");
        reset_to_idle();
    }
}

/* ── Helpers for control opcodes ──────────────────────────────────────────── */

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void do_start(const uint8_t *args, size_t n)
{
    /* size[4] + sha[32] + version[4] + sig[64] = 104 bytes */
    if (n != 4 + 32 + 4 + 64) {
        emit_status(DFU_RSP_START, DFU_ST_BAD_ARG);
        return;
    }
    if (!cfg_values()->dfu_enabled) {
        emit_status(DFU_RSP_START, DFU_ST_DISABLED);
        return;
    }
    if (!gate_authed()) {
        emit_status(DFU_RSP_START, DFU_ST_NOT_AUTHED);
        return;
    }
    if (s_state != DFU_IDLE) {
        emit_status(DFU_RSP_START, DFU_ST_BUSY);
        return;
    }

    uint32_t size = rd_u32_le(&args[0]);
    memcpy(s_expected_sha, &args[4], 32);
    uint32_t version = rd_u32_le(&args[4 + 32]);
    memcpy(s_expected_sig, &args[4 + 32 + 4], 64);
    (void)version; /* logged below; policy applied at VERIFY stage in M6     */

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        emit_status(DFU_RSP_START, DFU_ST_OTA_ERR);
        return;
    }
    if (size == 0 || size > part->size) {
        ESP_LOGW(TAG, "START: size %lu exceeds slot size %lu",
                 (unsigned long)size, (unsigned long)part->size);
        emit_status(DFU_RSP_START, DFU_ST_SIZE_TOO_BIG);
        return;
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, size, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        emit_status(DFU_RSP_START, DFU_ST_OTA_ERR);
        return;
    }

    s_part           = part;
    s_ota_handle     = h;
    s_expected_size  = size;
    s_bytes_received = 0;
    s_expected_seq   = 0;
    s_image_version  = version;
    s_last_chunk_us  = esp_timer_get_time();
    bp_sha256_init(&s_sha);
    s_state = DFU_RECEIVING;

    /* Cap the chunk size at the negotiated ATT MTU minus 3 bytes ATT header
     * minus 2 bytes seq header.  Without this, Chrome on Windows (default
     * MTU ~247) rejects full-sized writes with "GATT operation failed" even
     * though our compile-time maximum would allow more. */
    uint16_t mtu = ble_mgmt_get_mtu();
    uint16_t max_chunk = DFU_MAX_CHUNK_PAYLOAD;
    if (mtu > 5 && (uint32_t)(mtu - 5) < max_chunk) max_chunk = (uint16_t)(mtu - 5);
    if (max_chunk < 32) max_chunk = 32;       /* floor: don't degrade below 32 B */

    ESP_LOGI(TAG, "START accepted  size=%lu  ver=%lu  slot=%s@0x%06lx  "
                  "mtu=%u chunk=%u window=%d",
             (unsigned long)size, (unsigned long)version,
             part->label, (unsigned long)part->address,
             (unsigned)mtu, (unsigned)max_chunk, DFU_WINDOW);

    uint8_t r[6];
    r[0] = DFU_RSP_START;
    r[1] = DFU_ST_OK;
    r[2] =  max_chunk        & 0xFF;
    r[3] = (max_chunk >>  8) & 0xFF;
    r[4] =  DFU_WINDOW        & 0xFF;
    r[5] = (DFU_WINDOW >>  8) & 0xFF;
    emit(r, sizeof(r));
}

static void do_verify(size_t n)
{
    if (n != 0) { emit_status(DFU_RSP_VERIFY, DFU_ST_BAD_ARG); return; }
    if (s_state != DFU_RECEIVING) {
        emit_status(DFU_RSP_VERIFY, DFU_ST_BAD_STATE);
        return;
    }
    if (s_bytes_received != s_expected_size) {
        emit_status(DFU_RSP_VERIFY, DFU_ST_BAD_STATE);
        return;
    }

    uint8_t actual_sha[32];
    /* Finalise a *copy* so we can repeat VERIFY if the client wants. */
    bp_sha256_ctx_t tmp = s_sha;
    bp_sha256_final(&tmp, actual_sha);

    if (memcmp(actual_sha, s_expected_sha, 32) != 0) {
        ESP_LOGW(TAG, "VERIFY: SHA-256 mismatch");
        emit_status(DFU_RSP_VERIFY, DFU_ST_HASH_MISMATCH);
        reset_to_idle();
        return;
    }

    /* ECDSA P-256 signature check over the 32-byte SHA-256 of the image.
     * Until a real pubkey is provisioned (FW_SIGNING_PUBKEY[0] still 0x00
     * rather than the 0x04 SEC1 uncompressed-point tag) this will always
     * fail — which is the desired fail-closed behaviour for un-provisioned
     * units, as documented in main/signing_pubkey.h. */
    if (!verify_ecdsa_p256(FW_SIGNING_PUBKEY, actual_sha, s_expected_sig)) {
        ESP_LOGW(TAG, "VERIFY: signature invalid");
        emit_status(DFU_RSP_VERIFY, DFU_ST_SIG_MISMATCH);
        reset_to_idle();
        return;
    }

    s_state = DFU_READY_TO_APPLY;
    ESP_LOGI(TAG, "VERIFY ok — %lu bytes, SHA + ECDSA match",
             (unsigned long)s_bytes_received);
    emit_status(DFU_RSP_VERIFY, DFU_ST_OK);
}

static void do_apply(size_t n)
{
    if (n != 0) { emit_status(DFU_RSP_APPLY, DFU_ST_BAD_ARG); return; }
    if (s_state != DFU_READY_TO_APPLY) {
        emit_status(DFU_RSP_APPLY, DFU_ST_BAD_STATE);
        return;
    }

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        emit_status(DFU_RSP_APPLY, DFU_ST_OTA_ERR);
        reset_to_idle();
        return;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        emit_status(DFU_RSP_APPLY, DFU_ST_OTA_ERR);
        reset_to_idle();
        return;
    }

    ESP_LOGW(TAG, "APPLY: boot partition set to %s — rebooting in 200 ms",
             s_part->label);
    s_state = DFU_APPLYING;
    emit_status(DFU_RSP_APPLY, DFU_ST_OK);

    /* Give the notify a chance to go out and the client a chance to receive
     * it before we yank the BLE stack. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void do_status(size_t n)
{
    if (n != 0) { emit_status(DFU_RSP_STATUS, DFU_ST_BAD_ARG); return; }
    uint8_t r[10];
    r[0] = DFU_RSP_STATUS;
    r[1] = (uint8_t)s_state;
    r[2] =  s_bytes_received         & 0xFF;
    r[3] = (s_bytes_received >>  8)  & 0xFF;
    r[4] = (s_bytes_received >> 16)  & 0xFF;
    r[5] = (s_bytes_received >> 24)  & 0xFF;
    r[6] =  s_expected_size          & 0xFF;
    r[7] = (s_expected_size  >>  8)  & 0xFF;
    r[8] = (s_expected_size  >> 16)  & 0xFF;
    r[9] = (s_expected_size  >> 24)  & 0xFF;
    emit(r, sizeof(r));
}

/* ── Public: control writes ───────────────────────────────────────────────── */

int dfu_handle_control(const uint8_t *data, size_t len)
{
    if (len < 1) return 1;
    uint8_t op = data[0];

    switch (op) {
    case DFU_OP_START:
        do_start(&data[1], len - 1);
        break;
    case DFU_OP_VERIFY:
        do_verify(len - 1);
        break;
    case DFU_OP_APPLY:
        do_apply(len - 1);
        break;
    case DFU_OP_ABORT:
        if (s_state != DFU_IDLE) {
            ESP_LOGW(TAG, "client ABORT during state %d", (int)s_state);
        }
        emit_abort(DFU_ST_ABORTED);
        reset_to_idle();
        break;
    case DFU_OP_STATUS:
        do_status(len - 1);
        break;
    default:
        return 1;
    }
    return 0;
}

/* ── Public: data writes ──────────────────────────────────────────────────── */

int dfu_handle_data(const uint8_t *data, size_t len)
{
    if (s_state != DFU_RECEIVING)   return 0;  /* silently ignore             */
    if (len < 3 || len > 2 + DFU_MAX_CHUNK_PAYLOAD) {
        emit_abort(DFU_ST_BAD_ARG);
        reset_to_idle();
        return 1;
    }

    uint16_t seq = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (seq != s_expected_seq) {
        ESP_LOGW(TAG, "out-of-seq: got %u expected %u", seq, s_expected_seq);
        emit_abort(DFU_ST_OUT_OF_SEQ);
        reset_to_idle();
        return 1;
    }

    dfu_chunk_t chunk;
    chunk.seq = seq;
    chunk.len = (uint16_t)(len - 2);
    memcpy(chunk.data, &data[2], chunk.len);

    /* Non-blocking send: if the queue is full the client has over-run the
     * window.  Clients that honour the window size returned by START_RSP
     * never hit this; anything else is treated as a protocol error. */
    if (xQueueSend(s_chunk_q, &chunk, 0) != pdTRUE) {
        ESP_LOGE(TAG, "chunk queue full at seq=%u (client ignored window)", seq);
        emit_abort(DFU_ST_OUT_OF_SEQ);
        reset_to_idle();
        return 1;
    }

    s_expected_seq++;
    s_last_chunk_us = esp_timer_get_time();
    return 0;
}

/* ── DFU worker task (core 1) ─────────────────────────────────────────────── */

static void dfu_task(void *arg)
{
    (void)arg;
    dfu_chunk_t chunk;
    ESP_LOGI(TAG, "dfu_task started  queue_depth=%d  chunk_max=%d",
             DFU_Q_DEPTH, DFU_MAX_CHUNK_PAYLOAD);

    for (;;) {
        if (xQueueReceive(s_chunk_q, &chunk, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Idle — check timeout on the active session. */
            if (s_state == DFU_RECEIVING && s_last_chunk_us != 0 &&
                (esp_timer_get_time() - s_last_chunk_us) >
                    (int64_t)DFU_CHUNK_TIMEOUT_MS * 1000) {
                ESP_LOGW(TAG, "chunk timeout — aborting");
                emit_abort(DFU_ST_TIMEOUT);
                reset_to_idle();
            }
            continue;
        }

        /* Writing to OTA partition is the slow step — keeping it off the
         * host task is the whole reason we have this queue. */
        esp_err_t err = esp_ota_write(s_ota_handle, chunk.data, chunk.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write (seq=%u, %u B): %s",
                     chunk.seq, chunk.len, esp_err_to_name(err));
            emit_abort(DFU_ST_OTA_ERR);
            reset_to_idle();
            continue;
        }

        bp_sha256_update(&s_sha, chunk.data, chunk.len);
        s_bytes_received += chunk.len;

        uint8_t r[7];
        r[0] = DFU_RSP_CHUNK;
        r[1] = (uint8_t)((chunk.seq + 1) & 0xFF);
        r[2] = (uint8_t)((chunk.seq + 1) >> 8);
        r[3] =  s_bytes_received         & 0xFF;
        r[4] = (s_bytes_received >>  8)  & 0xFF;
        r[5] = (s_bytes_received >> 16)  & 0xFF;
        r[6] = (s_bytes_received >> 24)  & 0xFF;
        emit(r, sizeof(r));
    }
}

/* ── Public: init ─────────────────────────────────────────────────────────── */

void dfu_init(dfu_notify_cb_t notify_ctrl)
{
    s_notify_ctrl = notify_ctrl;
    s_chunk_q = xQueueCreate(DFU_Q_DEPTH, sizeof(dfu_chunk_t));
    configASSERT(s_chunk_q != NULL);
    xTaskCreatePinnedToCore(dfu_task, "dfu", 6144, NULL, 4, NULL, 1);
}

bool dfu_is_active(void)
{
    return s_state != DFU_IDLE;
}

dfu_state_t dfu_get_state(void)
{
    return s_state;
}

/* ── Rollback-completion ("healthy") task ────────────────────────────────── */

static void dfu_health_task(void *arg)
{
    (void)arg;

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t  st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);

    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        /* Either already valid, or booted from factory — nothing to do. */
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "running from pending-verify image — will mark valid in %d s",
             DFU_HEALTH_WAIT_MS / 1000);

    vTaskDelay(pdMS_TO_TICKS(DFU_HEALTH_WAIT_MS));

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image marked valid — rollback cancelled");
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s",
                 esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void dfu_start_health_monitor(void)
{
    xTaskCreatePinnedToCore(dfu_health_task, "dfu_health",
                            3072, NULL, 2, NULL, 1);
}
