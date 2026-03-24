/**
 * ble_nus.c  —  Nordic UART Service (NUS) peripheral using ESP-IDF NimBLE
 *
 * Architecture:
 *   - NimBLE host task runs on core 0 (created by nimble_port_freertos_init)
 *   - GAP events (connect/disconnect) are handled in the host task context
 *   - GATT write callbacks are called from the host task context
 *   - nus_notify() may be called from any task; it allocates an mbuf from the
 *     NimBLE pool and calls ble_gatts_notify_custom (thread-safe in NimBLE)
 *
 * UUID byte order:
 *   NimBLE BLE_UUID128_INIT() expects bytes in little-endian (wire) order,
 *   i.e. the UUID string "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" becomes
 *   { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e }
 */

#include "ble_nus.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_NUS";

// ── NUS UUIDs (128-bit, little-endian wire order) ────────────────────────────

// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);

// 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (client writes → UART TX)
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

// 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (ESP32 notifies client of UART RX)
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

// ── State ─────────────────────────────────────────────────────────────────────

static volatile bool     s_connected    = false;
static volatile uint16_t s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_tx_val_handle;   // ATT handle for TX char value
static nus_write_cb_t    s_write_cb     = NULL;
static const char       *s_device_name  = "NUS Bridge";
static uint8_t           s_own_addr_type;

// ── Forward declarations ──────────────────────────────────────────────────────

static void start_advertising(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

// ── GATT callbacks ────────────────────────────────────────────────────────────

/**
 * NUS RX characteristic: called when the BLE client writes data.
 * Flattens the mbuf chain into a stack buffer and invokes s_write_cb.
 */
static int nus_rx_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (!s_write_cb) return 0;

    uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    if (pkt_len == 0) return 0;

    // Flatten mbuf chain — maximum is negotiated MTU minus 3 bytes overhead
    uint8_t buf[512];
    if (pkt_len > sizeof(buf)) pkt_len = sizeof(buf);

    int rc = os_mbuf_copydata(ctxt->om, 0, pkt_len, buf);
    if (rc != 0) {
        ESP_LOGW(TAG, "os_mbuf_copydata failed: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_write_cb(buf, pkt_len);
    return 0;
}

/**
 * NUS TX characteristic access callback.
 * Clients subscribe via CCCD; reads of the value itself return an empty buffer.
 */
static int nus_tx_chr_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    // Only notify is used; reads just return success with no data
    return 0;
}

// ── GATT service table ────────────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* NUS RX — client writes data that we forward to the UART */
                .uuid      = &NUS_RX_UUID.u,
                .access_cb = nus_rx_chr_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* NUS TX — ESP32 notifies client of UART received data.
                 * val_handle is populated by ble_gatts_add_svcs() and used
                 * later in ble_gatts_notify_custom().                        */
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = nus_tx_chr_cb,
                .val_handle = &s_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, // terminator
        },
    },
    { 0 }, // terminator
};

// ── Advertising ───────────────────────────────────────────────────────────────

static void start_advertising(void)
{
    // Advertising PDU: flags + NUS 128-bit service UUID
    // Placing the UUID here (not in scan response) ensures it is visible to
    // passive scanners and to Web Bluetooth on Windows, which uses the WinRT
    // BluetoothLEAdvertisementWatcher and may not process scan responses in
    // time for the requestDevice() picker.
    // 3 bytes flags + 18 bytes UUID128 AD = 21 bytes — fits within 31-byte limit.
    struct ble_hs_adv_fields adv = {0};
    adv.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128              = &NUS_SVC_UUID;
    adv.num_uuids128          = 1;
    adv.uuids128_is_complete  = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    // Scan response: device name (too long to share ADV PDU with 128-bit UUID)
    struct ble_hs_adv_fields rsp = {0};
    rsp.name                     = (const uint8_t *)s_device_name;
    rsp.name_len                 = (uint8_t)strlen(s_device_name);
    rsp.name_is_complete         = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        // Scan response failure is non-fatal — device is still discoverable by UUID
        ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields: %d (non-fatal)", rc);
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;  // connectable undirected
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // general discoverable

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising as '%s'", s_device_name);
}

// ── GAP event handler ─────────────────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected   = true;
            ESP_LOGI(TAG, "Client connected  handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed  status=%d — restarting advertising",
                     event->connect.status);
            s_connected = false;
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected  reason=0x%02x — restarting advertising",
                 event->disconnect.reason);
        s_connected   = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU exchanged: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGD(TAG, "Connection parameters updated");
        break;

    default:
        break;
    }

    return 0;
}

// ── NimBLE host sync / reset callbacks ───────────────────────────────────────

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: %d", rc);
        return;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE address: %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);

    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset  reason=%d", reason);
    s_connected = false;
}

// ── NimBLE host task ──────────────────────────────────────────────────────────

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();          // blocks until nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ── Public API ────────────────────────────────────────────────────────────────

void nus_init(const char *device_name, nus_write_cb_t write_cb)
{
    s_device_name = device_name;
    s_write_cb    = write_cb;

    // NVS is required by the BLE stack for bonding storage
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());

    // Register host sync and reset callbacks
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Set the GAP device name (used by BLE stack for Generic Access service)
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(device_name));

    // Initialise standard GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Register the NUS service
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    assert(rc == 0);

    // Start the NimBLE host task on core 0 (BLE stack runs there)
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "NUS service registered  tx_handle=%d", s_tx_val_handle);
}

bool nus_is_connected(void)
{
    return s_connected;
}

int nus_notify(const uint8_t *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return -1;
    if (len == 0) return 0;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGW(TAG, "nus_notify: mbuf alloc failed (len=%u)", (unsigned)len);
        return -1;
    }

    // ble_gatts_notify_custom takes ownership of om and frees it
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "ble_gatts_notify_custom: %d", rc);
    }
    return rc;
}
