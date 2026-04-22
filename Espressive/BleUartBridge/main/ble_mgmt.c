/**
 * ble_mgmt.c — BP+ Mgmt GATT service implementation.
 *
 * M3 scope: AUTH characteristic (live), INFO characteristic (live).
 * CFG_CONTROL / DFU_CONTROL / DFU_DATA present in the GATT table so clients
 * see a stable schema, but access handlers return a "not-ready" status.
 * They are wired up in M4 and M5.
 */
#include "ble_mgmt.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_mac.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "auth.h"
#include "cfg.h"
#include "dfu.h"
#include "signing_pubkey.h"

static const char *TAG = "BLE_MGMT";

/* ── UUIDs (little-endian wire order) ─────────────────────────────────────── */

/* 7E500001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t MGMT_SVC_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x50,0x7e);

/* 7E500002 AUTH */
static const ble_uuid128_t MGMT_AUTH_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x50,0x7e);

/* 7E500003 INFO */
static const ble_uuid128_t MGMT_INFO_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x50,0x7e);

/* 7E500004 CFG_CONTROL */
static const ble_uuid128_t MGMT_CFG_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x04,0x00,0x50,0x7e);

/* 7E500005 DFU_CONTROL */
static const ble_uuid128_t MGMT_DFU_CTL_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x05,0x00,0x50,0x7e);

/* 7E500006 DFU_DATA */
static const ble_uuid128_t MGMT_DFU_DATA_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x06,0x00,0x50,0x7e);

/* ── Per-connection state ─────────────────────────────────────────────────── */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu         = 23;

static uint16_t s_auth_val_handle;
static uint16_t s_info_val_handle;
static uint16_t s_cfg_val_handle;
static uint16_t s_dfu_ctl_val_handle;
static uint16_t s_dfu_data_val_handle;

/* ── Low-level notify helper ──────────────────────────────────────────────── */

static void notify_on(uint16_t val_handle, const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGW(TAG, "notify: mbuf alloc failed (len=%u)", (unsigned)len);
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify: rc=%d", rc);
    }
}

static void auth_notify(const uint8_t *data, size_t len)
{
    notify_on(s_auth_val_handle, data, len);
}

/* ── AUTH characteristic ──────────────────────────────────────────────────── */

static int auth_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t buf[64];
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, n, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

    bool ok = auth_handle_write(buf, n, auth_notify);
    return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/* ── INFO characteristic ──────────────────────────────────────────────────── */

/* Fixed-size wire struct, little-endian.  Schema versioned by magic+version. */
typedef struct __attribute__((packed)) {
    uint16_t magic;            /* 0xBB01                                      */
    uint8_t  version;          /* INFO schema version (1)                     */
    uint8_t  target;           /* 0 = esp32, 1 = esp32s3                      */
    char     fw_version[32];   /* NUL-padded; from esp_app_desc_t.version     */
    uint8_t  elf_sha8[8];      /* first 8 bytes of esp_app_get_elf_sha256     */
    uint32_t running_addr;
    uint32_t running_size;
    uint32_t next_addr;
    uint32_t next_size;
    uint16_t preferred_mtu;
    uint8_t  auth_required;
    uint8_t  dfu_enabled;
    uint8_t  pubkey_fp[16];    /* SHA-256(DER pubkey) truncated to 16 B       */
    uint8_t  mac[6];
    uint8_t  reserved[2];
} info_v1_t;

static uint8_t s_mac_cache[6] = {0};

static void build_info(info_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic   = 0xBB01;
    out->version = 1;
#if CONFIG_IDF_TARGET_ESP32S3
    out->target  = 1;
#else
    out->target  = 0;
#endif

    const esp_app_desc_t *desc = esp_app_get_description();
    strncpy(out->fw_version, desc->version, sizeof(out->fw_version) - 1);

    /* esp_app_desc_t.app_elf_sha256 is the raw 32-byte ELF SHA; take the
     * first 8 bytes as a human-readable fingerprint. */
    memcpy(out->elf_sha8, desc->app_elf_sha256, 8);

    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (run) {
        out->running_addr = run->address;
        out->running_size = run->size;
    }
    if (next) {
        out->next_addr = next->address;
        out->next_size = next->size;
    }

    const cfg_values_t *v = cfg_values();
    out->preferred_mtu = v->ble_preferred_mtu;
    out->auth_required = v->auth_required;
    out->dfu_enabled   = v->dfu_enabled;

    memcpy(out->pubkey_fp, FW_SIGNING_PUBKEY_FP, 16);
    memcpy(out->mac, s_mac_cache, 6);
}

static int info_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return 0;

    info_v1_t info;
    build_info(&info);

    int rc = os_mbuf_append(ctxt->om, &info, sizeof(info));
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/* ── CFG_CONTROL ──────────────────────────────────────────────────────────── */

/* Client → server opcodes */
#define CFG_OP_GET      0x01   /* u8 id                                       */
#define CFG_OP_SET      0x02   /* u8 id, u8 len, u8 val[len]                  */
#define CFG_OP_COMMIT   0x03   /* (no args)                                   */
#define CFG_OP_RESET    0x04   /* (no args) → factory reset + reboot          */
#define CFG_OP_ENUM     0x05   /* (no args) → server emits every known TLV    */

/* Server → client response opcodes */
#define CFG_RSP_GET      0x81   /* u8 id, u8 status, u8 len, u8 val[len]      */
#define CFG_RSP_SET      0x82   /* u8 id, u8 status                           */
#define CFG_RSP_COMMIT   0x83   /* u8 status                                  */
#define CFG_RSP_ENUM     0x85   /* u8 id, u8 type, u8 live, u8 len, u8 val[]  */
#define CFG_RSP_ENUM_END 0x86   /* (no args)                                  */

/* Every TLV id we understand; clients can walk this via the ENUM opcode
 * without needing a per-version hard-coded list. */
static const cfg_id_t k_all_ids[] = {
    CFG_UART_BAUD, CFG_UART_FLOWCTRL, CFG_UART_DATABITS, CFG_UART_PARITY,
    CFG_UART_STOPBITS, CFG_NAME_SUFFIX, CFG_BLE_TX_POWER, CFG_HEXDUMP_DEFAULT,
    CFG_DFU_ENABLED, CFG_AUTH_REQUIRED, CFG_BLE_ADV_INTERVAL, CFG_BLE_PREFERRED_MTU,
};
static const size_t k_all_ids_n = sizeof(k_all_ids) / sizeof(k_all_ids[0]);

/* Normalise cfg error codes into a compact status byte for the wire. */
static uint8_t cfg_status_byte(int rc)
{
    /* 0 OK, 1 bad id, 2 bad len, 3 range, 4 nvs, 255 other */
    switch (rc) {
    case CFG_OK:           return 0;
    case CFG_ERR_BAD_ID:   return 1;
    case CFG_ERR_BAD_LEN:  return 2;
    case CFG_ERR_RANGE:    return 3;
    case CFG_ERR_NVS:      return 4;
    default:               return 255;
    }
}

/* True while authenticated writes to CFG are required.  Reads of this flag
 * must match the INFO.auth_required surface. */
static bool cfg_auth_gate(void)
{
    if (!cfg_values()->auth_required) return false;
    return !auth_is_authenticated();
}

static int cfg_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n < 1 || n > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[64];
    if (os_mbuf_copydata(ctxt->om, 0, n, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (cfg_auth_gate()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t op = buf[0];

    switch (op) {

    case CFG_OP_GET: {
        if (n != 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t id = buf[1];
        uint8_t val[32];
        uint8_t vlen = sizeof(val);
        int rc = cfg_get_tlv((cfg_id_t)id, val, &vlen);

        uint8_t resp[4 + 32];
        resp[0] = CFG_RSP_GET;
        resp[1] = id;
        resp[2] = cfg_status_byte(rc);
        resp[3] = (rc == CFG_OK) ? vlen : 0;
        if (rc == CFG_OK) memcpy(&resp[4], val, vlen);
        notify_on(s_cfg_val_handle, resp, (size_t)(4 + resp[3]));
        return 0;
    }

    case CFG_OP_SET: {
        if (n < 3) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t id  = buf[1];
        uint8_t len = buf[2];
        if ((size_t)3 + len != n) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        /* Factory-reset trigger is handled here so the reboot happens at
         * commit time in a predictable place. */
        if (id == CFG_FACTORY_RESET) {
            uint8_t resp[3] = { CFG_RSP_SET, id, 0 };
            notify_on(s_cfg_val_handle, resp, sizeof(resp));
            /* Give the notify a moment to go out before we reboot. */
            vTaskDelay(pdMS_TO_TICKS(100));
            cfg_factory_reset();
            return 0;   /* not reached */
        }

        int rc = cfg_set_tlv((cfg_id_t)id, &buf[3], len);
        uint8_t resp[3] = { CFG_RSP_SET, id, cfg_status_byte(rc) };
        notify_on(s_cfg_val_handle, resp, sizeof(resp));
        return 0;
    }

    case CFG_OP_COMMIT: {
        if (n != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        int rc = cfg_commit();
        uint8_t resp[2] = { CFG_RSP_COMMIT, cfg_status_byte(rc) };
        notify_on(s_cfg_val_handle, resp, sizeof(resp));
        return 0;
    }

    case CFG_OP_RESET: {
        if (n != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t resp[2] = { CFG_RSP_COMMIT, 0 };
        notify_on(s_cfg_val_handle, resp, sizeof(resp));
        vTaskDelay(pdMS_TO_TICKS(100));
        cfg_factory_reset();
        return 0;   /* not reached */
    }

    case CFG_OP_ENUM: {
        if (n != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        for (size_t i = 0; i < k_all_ids_n; ++i) {
            cfg_id_t   id = k_all_ids[i];
            cfg_type_t type; uint8_t max_len; bool live;
            if (!cfg_describe(id, &type, &max_len, &live)) continue;

            uint8_t val[32];
            uint8_t vlen = sizeof(val);
            (void)cfg_get_tlv(id, val, &vlen);

            uint8_t resp[5 + 32];
            resp[0] = CFG_RSP_ENUM;
            resp[1] = (uint8_t)id;
            resp[2] = (uint8_t)type;
            resp[3] = live ? 1 : 0;
            resp[4] = vlen;
            memcpy(&resp[5], val, vlen);
            notify_on(s_cfg_val_handle, resp, 5 + vlen);
        }
        uint8_t end = CFG_RSP_ENUM_END;
        notify_on(s_cfg_val_handle, &end, 1);
        return 0;
    }

    default:
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
}

static void dfu_ctl_notify(const uint8_t *data, size_t len)
{
    notify_on(s_dfu_ctl_val_handle, data, len);
}

static int dfu_ctl_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n < 1 || n > 128) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[128];
    if (os_mbuf_copydata(ctxt->om, 0, n, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

    /* dfu.c applies the auth/dfu_enabled gates per-opcode (START rejects
     * with an explicit status so clients can tell why). */
    int rc = dfu_handle_control(buf, n);
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int dfu_data_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n < 3 || n > 2 + 510) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[2 + 510];
    if (os_mbuf_copydata(ctxt->om, 0, n, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

    /* write_no_rsp: clients never see our return value, so a non-zero here
     * has no effect beyond logging.  dfu.c will have emitted ABORT_EVT. */
    (void)dfu_handle_data(buf, n);
    return 0;
}

/* ── GATT service table ───────────────────────────────────────────────────── */

static const struct ble_gatt_svc_def s_mgmt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &MGMT_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &MGMT_AUTH_UUID.u,
                .access_cb  = auth_chr_cb,
                .val_handle = &s_auth_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid       = &MGMT_INFO_UUID.u,
                .access_cb  = info_chr_cb,
                .val_handle = &s_info_val_handle,
                .flags      = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid       = &MGMT_CFG_UUID.u,
                .access_cb  = cfg_chr_cb,
                .val_handle = &s_cfg_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid       = &MGMT_DFU_CTL_UUID.u,
                .access_cb  = dfu_ctl_chr_cb,
                .val_handle = &s_dfu_ctl_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid       = &MGMT_DFU_DATA_UUID.u,
                .access_cb  = dfu_data_chr_cb,
                .val_handle = &s_dfu_data_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ── Public API ───────────────────────────────────────────────────────────── */

void ble_mgmt_init(const uint8_t ble_mac[6])
{
    memcpy(s_mac_cache, ble_mac, 6);
    auth_init(ble_mac);
    /* dfu_init() must happen only once per boot; it spawns a persistent task
     * and creates a queue.  The auth-init path can fire from on_sync more
     * than once if the BLE stack resets, so guard via a static flag. */
    static bool s_dfu_started = false;
    if (!s_dfu_started) {
        dfu_init(dfu_ctl_notify);
        s_dfu_started = true;
    }
}

void ble_mgmt_gatt_register(void)
{
    int rc = ble_gatts_count_cfg(s_mgmt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(s_mgmt_svcs);
    assert(rc == 0);
    ESP_LOGI(TAG, "BP+ Mgmt service registered  auth=%d info=%d cfg=%d "
                  "dfu_ctl=%d dfu_data=%d",
             s_auth_val_handle, s_info_val_handle, s_cfg_val_handle,
             s_dfu_ctl_val_handle, s_dfu_data_val_handle);
}

void ble_mgmt_on_connect(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    s_mtu         = 23;
    auth_on_connect();
}

void ble_mgmt_on_disconnect(void)
{
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    auth_on_disconnect();
    dfu_on_disconnect();
}

void ble_mgmt_on_mtu(uint16_t mtu)
{
    s_mtu = mtu;
}

uint16_t ble_mgmt_get_mtu(void)
{
    return s_mtu;
}

bool ble_mgmt_is_authenticated(void)
{
    return auth_is_authenticated();
}

bool ble_mgmt_dfu_active(void)
{
    return dfu_is_active();
}
