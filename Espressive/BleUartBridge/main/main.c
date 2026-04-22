/**
 * main.c  —  BLE ↔ UART bridge  (ESP-IDF / NimBLE)
 *
 * Bridges a BLE Nordic UART Service (NUS) connection to UART1 with
 * hardware RTS/CTS flow control.  Supports ESP32 and ESP32-S3 via
 * compile-time board selection; pin assignments live in boards/.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  BLE client  ◄──NUS notify──►  ESP32/S3  ◄──UART1 RTS/CTS──►  Device   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Task layout:
 *   ble_host_task  (core 0, NimBLE)   — BLE stack, GAP/GATT events
 *   uart_tx_task   (core 1)           — drains BLE→UART queue to UART1
 *   uart_rx_task   (core 1)           — UART1 RX → BLE notifications
 *   led_task       (core 1)           — LED status indicator
 *   app_main       (core 1)           — init + interactive console
 *
 * Console commands (via idf.py monitor or any terminal at UART0 baud):
 *   h   toggle hex dump of bridged data
 *   n   toggle NimBLE verbose logging
 *   s   print status (connection, byte counts, queue depth, heap)
 *
 * Build commands:
 *   ESP32:   idf.py -DIDF_TARGET=esp32   -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32"   build
 *   ESP32-S3:idf.py -DIDF_TARGET=esp32s3 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

// ── Board pin assignments ─────────────────────────────────────────────────────
// TX_PIN, RX_PIN, RTS_PIN, CTS_PIN, LED_PIN are defined in the board header.
// The board is selected automatically based on the IDF_TARGET.

#if CONFIG_IDF_TARGET_ESP32S3
  #include "esp32s3_devkit.h"
#elif CONFIG_IDF_TARGET_ESP32
  #include "esp32_devkit.h"
#else
  #error "Unsupported IDF_TARGET — add a boards/ header for this chip"
#endif

// ── WS2812 RGB LED (ESP32-S3 only) ───────────────────────────────────────────
#if CONFIG_IDF_TARGET_ESP32S3
  #include "led_strip.h"
  static led_strip_handle_t s_led_strip;
#endif

#include "ble_nus.h"
#include "ble_mgmt.h"
#include "cfg.h"
#include "dfu.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

static const char *TAG = "BRIDGE";

// ── Bridge parameters ─────────────────────────────────────────────────────────

#define UART_PORT       UART_NUM_1
#define UART_RX_BUF     4096    // UART driver RX ring buffer

#define BLE_DEVICE_NAME_PREFIX "BP+ Bridge"
#define BLE_MTU         512
#define BLE_CHUNK       128     // max bytes per BLE notification

#define FC_THRESH       122     // RTS deasserts when UART RX FIFO ≥ this many bytes

// ── Bridge state ──────────────────────────────────────────────────────────────

static volatile bool          hex_dump_on        = false;
static volatile bool          nimble_log_verbose  = false;
static volatile unsigned long bytes_to_uart       = 0;
static volatile unsigned long bytes_from_uart     = 0;
static volatile unsigned long bytes_dropped_tx    = 0;  // BLE→UART queue-full drops
static volatile unsigned long nomem_retries_total = 0;  // cumulative NUS_ERR_NOMEM retries
static volatile unsigned long nomem_last_ms       = 0;  // duration of last NOMEM episode
static volatile unsigned long bytes_dropped_dfu   = 0;  // UART bytes discarded during DFU

// ── UART TX queue (BLE → UART direction) ─────────────────────────────────────
// on_ble_write() runs on the NimBLE host task (core 0).  Calling uart_write_bytes()
// directly from there blocks the host task when the UART TX FIFO is full (CTS
// deasserted), preventing NimBLE from processing HCI events and recycling mbufs.
// Posting to a queue returns immediately; uart_tx_task (core 1) performs the
// blocking write, keeping the host task free to drain its event queue.

#define TX_Q_DEPTH   32         // max queued BLE writes before dropping (~4 KB at 128B/write)
#define TX_MAX_LEN   512        // max BLE ATT write payload (MTU - 3)

typedef struct {
    uint8_t  data[TX_MAX_LEN];
    uint16_t len;
} tx_item_t;

static QueueHandle_t s_uart_tx_q;

// ── LED helpers ───────────────────────────────────────────────────────────────
// led_init() / led_set(r, g, b) abstract over a plain GPIO LED (ESP32) and a
// single-pixel WS2812 (ESP32-S3).  The same led_task() body drives both.

static void led_init(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    led_strip_config_t strip_cfg = {
        .strip_gpio_num            = LED_PIN,
        .max_leds                  = 1,
        .led_model                 = LED_MODEL_WS2812,
        .color_component_format    = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out          = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz — standard for WS2812
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip));
    led_strip_clear(s_led_strip);
#else
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_PIN, 0);
#endif
}

// Set LED colour.  On the single-colour GPIO LED any non-zero component = on.
// Keep r/g/b values small (≤ 16) on the WS2812 to limit current draw.
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_IDF_TARGET_ESP32S3
    led_strip_set_pixel(s_led_strip, 0, r, g, b);
    led_strip_refresh(s_led_strip);
#else
    gpio_set_level(LED_PIN, (r || g || b) ? 0 : 1);  // active-low: 0 = on
#endif
}

// ── LED task ──────────────────────────────────────────────────────────────────
// Blinks at 1 Hz while advertising; holds steady (dim blue) when connected.

static void led_task(void *arg)
{
    led_init();
    for (;;) {
        if (ble_mgmt_dfu_active()) {
            // DFU in progress: rapid pulse.  Cyan on S3 (WS2812 sees G+B),
            // fast GPIO blink on classic ESP32.
            led_set(0, 8, 8);
            vTaskDelay(pdMS_TO_TICKS(125));
            led_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(125));
        } else if (nus_is_connected()) {
            led_set(0, 0, 8);           // dim blue: connected
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            led_set(0, 0, 0);           // off
            vTaskDelay(pdMS_TO_TICKS(500));
            led_set(0, 0, 8);           // dim blue: blink
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// ── Hex dump ──────────────────────────────────────────────────────────────────

static void hex_dump(const char *dir, const uint8_t *data, size_t len)
{
    if (!hex_dump_on || len == 0) return;

    const int ROW = 16;
    char hex[ROW * 3 + 1];
    char asc[ROW + 1];

    for (size_t row = 0; row * ROW < len; row++) {
        size_t off   = row * ROW;
        size_t count = (len - off < ROW) ? (len - off) : ROW;

        for (int col = 0; col < ROW; col++) {
            if ((size_t)col < count) {
                uint8_t b = data[off + col];
                hex[col*3]   = "0123456789ABCDEF"[b >> 4];
                hex[col*3+1] = "0123456789ABCDEF"[b & 0xF];
                hex[col*3+2] = ' ';
                asc[col]     = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
            } else {
                hex[col*3] = hex[col*3+1] = hex[col*3+2] = ' ';
                asc[col] = ' ';
            }
        }
        hex[ROW*3] = asc[ROW] = '\0';

        ESP_LOGI(TAG, "%s  %04X | %s | %s", dir, (unsigned)off, hex, asc);
    }
}

// ── BLE RX callback: data written by client → UART TX ─────────────────────────
// Called from the NimBLE host task (core 0).
// Posts to s_uart_tx_q and returns immediately so the host task stays free to
// process HCI events (including mbuf recycling for sent notifications).
// uart_tx_task performs the blocking uart_write_bytes on core 1.

// Returns 0 if data was queued, 1 if the queue is full.
// A non-zero return causes nus_rx_chr_cb to send an ATT Error Response so that
// clients using writeValueWithResponse (ATT Write Request) receive the rejection
// and can retry.  The caller (uart_tx_task) will drain the queue as fast as
// hardware CTS flow control allows; the client should retry after a short delay.
static int on_ble_write(const uint8_t *data, size_t len)
{
    // DFU owns the airtime during an update — reject bridge traffic so the
    // client sees an ATT error rather than silently having bytes reordered
    // against the DFU chunks going the other way.
    if (ble_mgmt_dfu_active()) return 1;

    hex_dump("BLE→UART", data, len);

    if (len == 0 || len > TX_MAX_LEN) return 0;  // ignore empty / oversized

    tx_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);

    if (xQueueSend(s_uart_tx_q, &item, 0) != pdTRUE) {
        // Queue full — signal the ATT layer to reject this write so the client
        // can retry once the queue has drained (CTS-driven backpressure).
        bytes_dropped_tx += len;   // track bytes rejected (may be retried by client)
        return 1;
    }
    return 0;
}

// ── UART TX task: drains s_uart_tx_q → UART1 ─────────────────────────────────
// Runs on core 1.  uart_write_bytes may block here when CTS is deasserted —
// this is intentional and correct because this task is NOT the NimBLE host task.

static void uart_tx_task(void *arg)
{
    tx_item_t item;
    ESP_LOGI(TAG, "UART TX task started");

    for (;;) {
        if (xQueueReceive(s_uart_tx_q, &item, portMAX_DELAY) == pdTRUE) {
            int written = uart_write_bytes(UART_PORT, (const char *)item.data, item.len);
            if (written > 0) bytes_to_uart += written;
        }
    }
}

// ── UART RX task: UART1 → BLE notifications ───────────────────────────────────

static void uart_rx_task(void *arg)
{
    uint8_t buf[BLE_CHUNK];
    int     pending_n   = 0;   // bytes in buf waiting for a successful nus_notify
    int     retry_count = 0;   // consecutive NUS_ERR_NOMEM retries

    ESP_LOGI(TAG, "UART→BLE task started");

    for (;;) {
        // Suspend the bridge entirely while DFU is in progress.  We still
        // drain the UART ring buffer so the peer's TX is not wedged by our
        // RTS threshold, but the bytes are discarded — the client is updating
        // firmware, not talking to the downstream device.
        if (ble_mgmt_dfu_active()) {
            uint8_t drop[128];
            int n = uart_read_bytes(UART_PORT, drop, sizeof(drop),
                                    pdMS_TO_TICKS(20));
            if (n > 0) bytes_dropped_dfu += n;
            pending_n   = 0;
            retry_count = 0;
            continue;
        }

        // Only read new data once the previous chunk has been sent.
        //
        // Backpressure mechanism: when nus_notify() returns NUS_ERR_NOMEM we
        // keep pending_n > 0 and do NOT call uart_read_bytes again.  Leaving
        // data in the UART driver ring buffer causes it to fill; once full the
        // FIFO reaches the RTS threshold (FC_THRESH) and hardware RTS asserts,
        // pausing the sender.  Once an mbuf is freed the next retry succeeds.
        if (pending_n == 0) {
            retry_count = 0;
            int n = uart_read_bytes(UART_PORT, buf, sizeof(buf),
                                    pdMS_TO_TICKS(20));
            if (n <= 0) continue;
            pending_n = n;
            hex_dump("UART→BLE", buf, pending_n);
        }

        if (!nus_is_connected()) {
            pending_n   = 0;
            retry_count = 0;
            continue;
        }

        int rc = nus_notify(buf, pending_n);
        if (rc == 0) {
            if (retry_count > 0) {
                // Log recovery once here — console has had time to drain during
                // the vTaskDelay calls, so this single write is safe.
                nomem_last_ms = (unsigned long)retry_count * 2;
                ESP_LOGW(TAG, "UART→BLE: mbuf pool recovered after %lu ms", nomem_last_ms);
            }
            bytes_from_uart += pending_n;
            pending_n   = 0;
            retry_count = 0;
        } else if (rc == NUS_ERR_NOMEM) {
            // mbuf pool temporarily exhausted — hold buf, yield so the BLE host
            // task (core 0) can drain its TX queue and free mbufs.
            //
            // NO ESP_LOG* calls in this hot loop. When hex dump is active,
            // on_ble_write() floods the UART0 console faster than 115200 baud
            // can drain it. uart_tx_char() then busy-waits on the full FIFO
            // without yielding, which starves IDLE and triggers the task WDT.
            // Counters are reported safely by the 's' status command instead.
            retry_count++;
            nomem_retries_total++;
            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            // NUS_ERR_CONN: connection gone or fatal send error.
            // Do NOT retry — hammering ble_gatts_notify_custom while the host
            // task is trying to process the disconnect event starves it and
            // delays the GAP callback, preventing clean reconnect.
            ESP_LOGW(TAG, "UART→BLE: connection error — discarding %d B pending",
                     pending_n);
            pending_n   = 0;
            retry_count = 0;
            // Brief pause; let the host task fire the GAP disconnect callback
            // and update s_connected before we loop back to the connected check.
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ── UART1 initialisation ──────────────────────────────────────────────────────

static void uart_init(void)
{
    const cfg_values_t *v = cfg_values();

    uart_word_length_t db;
    switch (v->uart_databits) {
        case 5: db = UART_DATA_5_BITS; break;
        case 6: db = UART_DATA_6_BITS; break;
        case 7: db = UART_DATA_7_BITS; break;
        default: db = UART_DATA_8_BITS; break;
    }
    uart_parity_t par;
    switch (v->uart_parity) {
        case 1:  par = UART_PARITY_EVEN;    break;
        case 2:  par = UART_PARITY_ODD;     break;
        default: par = UART_PARITY_DISABLE; break;
    }
    uart_stop_bits_t sb = (v->uart_stopbits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    uart_hw_flowcontrol_t fc = v->uart_flowctrl ? UART_HW_FLOWCTRL_CTS_RTS
                                                : UART_HW_FLOWCTRL_DISABLE;

    uart_config_t cfg = {
        .baud_rate           = (int)v->uart_baud,
        .data_bits           = db,
        .parity              = par,
        .stop_bits           = sb,
        .flow_ctrl           = fc,
        .rx_flow_ctrl_thresh = FC_THRESH,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TX_PIN, RX_PIN,
                                 v->uart_flowctrl ? RTS_PIN : UART_PIN_NO_CHANGE,
                                 v->uart_flowctrl ? CTS_PIN : UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,
                                        UART_RX_BUF, /* rx_buffer_size */
                                        0,           /* tx_buffer_size: 0 = no SW buffer,
                                                        writes block when HW TX FIFO full */
                                        0, NULL, 0));

    ESP_LOGI(TAG, "UART1 ready  TX=%d RX=%d RTS=%d CTS=%d  %lu baud  %u%c%u  flow=%s",
             TX_PIN, RX_PIN,
             v->uart_flowctrl ? RTS_PIN : -1,
             v->uart_flowctrl ? CTS_PIN : -1,
             (unsigned long)v->uart_baud,
             v->uart_databits,
             v->uart_parity == 1 ? 'E' : v->uart_parity == 2 ? 'O' : 'N',
             v->uart_stopbits,
             v->uart_flowctrl ? "RTS/CTS" : "none");
}

// ── Interactive console ────────────────────────────────────────────────────────

// ── Live-apply callback for runtime-tunable settings ─────────────────────────
// Invoked from cfg_commit() whenever any TLV tagged live_apply changed.
// Compares against the previous applied values so we only act on the fields
// that moved.  Reboot-apply TLVs (baud, flow control, etc.) are NOT handled
// here — they are read once at boot by uart_init() and nus_init().
static cfg_values_t s_prev_applied;        // seeded in app_main after cfg_init

static void cfg_live_apply(const cfg_values_t *v)
{
    if (v->hexdump_default != s_prev_applied.hexdump_default) {
        hex_dump_on = (v->hexdump_default != 0);
        ESP_LOGI(TAG, "[cfg] hex dump %s", hex_dump_on ? "ON" : "OFF");
    }
    if (v->ble_adv_interval_ms != s_prev_applied.ble_adv_interval_ms) {
        ESP_LOGI(TAG, "[cfg] adv interval → %u ms", v->ble_adv_interval_ms);
        nus_restart_advertising();
    }
    /* TX power and auth/dfu flags are read on-demand by the code that uses
     * them (ble_mgmt for auth_required, dfu.c for dfu_enabled).  TX power
     * live-apply is deferred: NimBLE on IDF sets TX power through
     * esp_ble_tx_power_set(), which requires a per-target dBm→level mapping;
     * reboot-apply is acceptable for now. */

    s_prev_applied = *v;
}

static void print_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &st);
    const char *st_str;
    switch (st) {
        case ESP_OTA_IMG_NEW:             st_str = "new";             break;
        case ESP_OTA_IMG_PENDING_VERIFY:  st_str = "pending-verify";  break;
        case ESP_OTA_IMG_VALID:           st_str = "valid";           break;
        case ESP_OTA_IMG_INVALID:         st_str = "invalid";         break;
        case ESP_OTA_IMG_ABORTED:         st_str = "aborted";         break;
        default:                          st_str = "undefined";       break;
    }

    ESP_LOGI(TAG, "── Info ────────────────────────────────");
    ESP_LOGI(TAG, " App        : %s %s", app->project_name, app->version);
    ESP_LOGI(TAG, " Built      : %s %s", app->date, app->time);
    ESP_LOGI(TAG, " IDF        : %s", app->idf_ver);
    ESP_LOGI(TAG, " Running    : %s  @0x%06lx (%lu KB)  state=%s",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0UL,
             running ? (unsigned long)(running->size / 1024) : 0UL,
             st_str);
    ESP_LOGI(TAG, " Next OTA   : %s  @0x%06lx (%lu KB)",
             next ? next->label : "?",
             next ? (unsigned long)next->address : 0UL,
             next ? (unsigned long)(next->size / 1024) : 0UL);
    ESP_LOGI(TAG, "────────────────────────────────────────");
}

static void print_status(void)
{
    size_t hw_buf = 0;
    uart_get_buffered_data_len(UART_PORT, &hw_buf);

    ESP_LOGI(TAG, "── Status ──────────────────────────────");
    ESP_LOGI(TAG, " BLE        : %s", nus_is_connected() ? "connected" : "advertising");
    ESP_LOGI(TAG, " Disconnects: %lu  (since boot)", nus_disconnect_count());
    ESP_LOGI(TAG, " Hex dump   : %s", hex_dump_on ? "ON" : "OFF");
    ESP_LOGI(TAG, " NimBLE log : %s", nimble_log_verbose ? "VERBOSE" : "quiet");
    ESP_LOGI(TAG, " →UART      : %lu bytes", bytes_to_uart);
    ESP_LOGI(TAG, " ←UART      : %lu bytes", bytes_from_uart);
    ESP_LOGI(TAG, " TX rejected: %lu bytes  (BLE→UART queue-full; ATT error sent, client retries)", bytes_dropped_tx);
    ESP_LOGI(TAG, " NOMEM retry: %lu total  (last episode: %lu ms)", nomem_retries_total, nomem_last_ms);
    ESP_LOGI(TAG, " DFU drop   : %lu bytes  (UART→BLE discarded while DFU in progress)", bytes_dropped_dfu);
    ESP_LOGI(TAG, " DFU state  : %s", ble_mgmt_dfu_active() ? "ACTIVE" : "idle");
    ESP_LOGI(TAG, " UART hwBuf : %u bytes", (unsigned)hw_buf);
    ESP_LOGI(TAG, " TX q depth : %u / %d",
             (unsigned)(TX_Q_DEPTH - uxQueueSpacesAvailable(s_uart_tx_q)), TX_Q_DEPTH);
    ESP_LOGI(TAG, " Heap free  : %lu bytes  (min ever: %lu)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "────────────────────────────────────────");
}

// ── Entry point ───────────────────────────────────────────────────────────────

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    const char *board = "ESP32-S3 DevKit (S3-N16R8)";
#else
    const char *board = "ESP32 DevKit";
#endif

    // Load NVS-backed configuration before anything that depends on it (UART
    // baud/bits, hex dump default, device-name suffix override).
    cfg_init();
    hex_dump_on = (cfg_values()->hexdump_default != 0);

    // Seed the live-apply baseline with the current cached values so the
    // first commit doesn't spuriously "reapply" values that were loaded from
    // NVS at boot.
    s_prev_applied = *cfg_values();
    cfg_register_live_cb(cfg_live_apply);

    // Build a unique device name.  If CFG_NAME_SUFFIX is set use it; otherwise
    // derive the suffix from the last 4 hex digits of the BLE MAC so multiple
    // bridges are distinguishable during scanning.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char ble_device_name[32];
    const char *ns = cfg_values()->name_suffix;
    if (ns[0] != '\0') {
        snprintf(ble_device_name, sizeof(ble_device_name),
                 "%s %s", BLE_DEVICE_NAME_PREFIX, ns);
    } else {
        snprintf(ble_device_name, sizeof(ble_device_name),
                 "%s %02X%02X", BLE_DEVICE_NAME_PREFIX, mac[4], mac[5]);
    }

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "  BLE UART Bridge  —  %s", board);
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, " UART : TX=%d  RX=%d  RTS=%d  CTS=%d  %lu baud",
             TX_PIN, RX_PIN, RTS_PIN, CTS_PIN,
             (unsigned long)cfg_values()->uart_baud);
    ESP_LOGI(TAG, " BLE  : '%s'  MTU=%d  chunk=%d",
             ble_device_name, BLE_MTU, BLE_CHUNK);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, " LED  : GPIO%d (WS2812) — blink=advertising  steady=connected", LED_PIN);
#else
    ESP_LOGI(TAG, " LED  : GPIO%d — blink=advertising  steady=connected", LED_PIN);
#endif
    ESP_LOGI(TAG, " Dump : %s  (send 'h' to toggle)", hex_dump_on ? "ON" : "OFF");
    ESP_LOGI(TAG, " Commands: h=hex  n=NimBLE log  s=status  c=clear stats");
    ESP_LOGI(TAG, "           i=info  r=factory reset");

    // Suppress NimBLE host INFO logs by default — they fire on every notify()
    // call and flood the console during active data transfer.
    // Send 'n' in the monitor to toggle them back on for debugging.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // BLE→UART queue — must be created before nus_init registers on_ble_write
    s_uart_tx_q = xQueueCreate(TX_Q_DEPTH, sizeof(tx_item_t));
    assert(s_uart_tx_q != NULL);

    uart_init();
    nus_init(ble_device_name, on_ble_write);

    // BLE→UART task: drains TX queue to UART1 (blocking writes stay off host task)
    xTaskCreatePinnedToCore(uart_tx_task, "uart_tx",
                            4096, NULL, 5, NULL, 1);

    // UART→BLE forwarding task pinned to core 1
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            4096, NULL, 5, NULL, 1);

    // LED status task pinned to core 1
    xTaskCreatePinnedToCore(led_task, "led",
                            2048, NULL, 3, NULL, 1);

    // If this boot is running a freshly-flashed image, start the health
    // watchdog that will call esp_ota_mark_app_valid_cancel_rollback() once
    // the system has been stable for DFU_HEALTH_WAIT_MS.  No-op if the
    // running image is not in the PENDING_VERIFY state.
    dfu_start_health_monitor();

    // app_main becomes the interactive console loop
    ESP_LOGI(TAG, "Ready. Waiting for BLE connection…");

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        switch ((char)c) {
        case 'h': case 'H':
            hex_dump_on = !hex_dump_on;
            ESP_LOGI(TAG, "[CMD] Hex dump %s", hex_dump_on ? "ON" : "OFF");
            break;
        case 'n': case 'N':
            nimble_log_verbose = !nimble_log_verbose;
            esp_log_level_set("NimBLE",
                nimble_log_verbose ? ESP_LOG_INFO : ESP_LOG_WARN);
            ESP_LOGI(TAG, "[CMD] NimBLE log %s",
                nimble_log_verbose ? "VERBOSE" : "quiet");
            break;
        case 's': case 'S':
            print_status();
            break;
        case 'c': case 'C':
            bytes_to_uart        = 0;
            bytes_from_uart      = 0;
            bytes_dropped_tx     = 0;
            nomem_retries_total  = 0;
            nomem_last_ms        = 0;
            bytes_dropped_dfu    = 0;
            nus_reset_disconnect_count();
            ESP_LOGI(TAG, "[CMD] Stats cleared");
            break;
        case 'i': case 'I':
            print_info();
            break;
        case 'r': case 'R':
            ESP_LOGW(TAG, "[CMD] Factory reset? Press 'Y' to confirm within 5 s...");
            // Simple confirmation: next getchar within timeout must be Y.
            {
                TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                bool confirmed = false;
                while (xTaskGetTickCount() < deadline) {
                    int ch = getchar();
                    if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
                    if (ch == 'Y' || ch == 'y') { confirmed = true; break; }
                    break;
                }
                if (confirmed) cfg_factory_reset();   // does not return
                ESP_LOGI(TAG, "[CMD] Factory reset aborted");
            }
            break;
        default:
            break;
        }
    }
}
