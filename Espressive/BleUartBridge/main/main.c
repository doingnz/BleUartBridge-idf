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

static const char *TAG = "BRIDGE";

// ── Bridge parameters ─────────────────────────────────────────────────────────

#define UART_PORT       UART_NUM_1
#define UART_BAUD       115200
#define UART_RX_BUF     4096    // UART driver RX ring buffer

#define BLE_DEVICE_NAME "BP+ Bridge"
#define BLE_MTU         512
#define BLE_CHUNK       128     // max bytes per BLE notification

#define FC_THRESH       122     // RTS deasserts when UART RX FIFO ≥ this many bytes

// ── Bridge state ──────────────────────────────────────────────────────────────

static volatile bool          hex_dump_on        = false;
static volatile bool          nimble_log_verbose  = false;
static volatile unsigned long bytes_to_uart       = 0;
static volatile unsigned long bytes_from_uart     = 0;
static volatile unsigned long bytes_dropped_tx    = 0;  // BLE→UART queue-full drops

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
        if (nus_is_connected()) {
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

static void on_ble_write(const uint8_t *data, size_t len)
{
    hex_dump("BLE→UART", data, len);

    if (len == 0 || len > TX_MAX_LEN) return;

    tx_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);

    // Non-blocking post — if the queue is full the UART TX task is backed up
    // (CTS held off).  Drop and warn; RTS on the UART side will already be
    // asserting to slow the sender.
    if (xQueueSend(s_uart_tx_q, &item, 0) != pdTRUE) {
        bytes_dropped_tx += len;
        ESP_LOGW(TAG, "BLE→UART: TX queue full — dropped %u bytes (total dropped: %lu)",
                 (unsigned)len, bytes_dropped_tx);
    }
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
            bytes_from_uart += pending_n;
            pending_n   = 0;
            retry_count = 0;
        } else if (rc == NUS_ERR_NOMEM) {
            // mbuf pool temporarily exhausted — hold buf, yield so the BLE host
            // task (core 0) can drain its TX queue and free mbufs.
            retry_count++;
            if (retry_count % 250 == 0) {   // warn every ~500 ms
                ESP_LOGW(TAG, "UART→BLE: mbuf pool exhausted for %d ms, holding %d B",
                         retry_count * 2, pending_n);
            }
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
    uart_config_t cfg = {
        .baud_rate           = UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = FC_THRESH,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TX_PIN, RX_PIN, RTS_PIN, CTS_PIN));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,
                                        UART_RX_BUF, /* rx_buffer_size */
                                        0,           /* tx_buffer_size: 0 = no SW buffer,
                                                        writes block when HW TX FIFO full */
                                        0, NULL, 0));

    ESP_LOGI(TAG, "UART1 ready  TX=%d RX=%d RTS=%d CTS=%d  %d baud  RTS threshold=%d",
             TX_PIN, RX_PIN, RTS_PIN, CTS_PIN, UART_BAUD, FC_THRESH);
}

// ── Interactive console ────────────────────────────────────────────────────────

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
    ESP_LOGI(TAG, " TX dropped : %lu bytes  (BLE→UART queue-full)", bytes_dropped_tx);
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

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "  BLE UART Bridge  —  %s", board);
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, " UART : TX=%d  RX=%d  RTS=%d  CTS=%d  %d baud",
             TX_PIN, RX_PIN, RTS_PIN, CTS_PIN, UART_BAUD);
    ESP_LOGI(TAG, " BLE  : '%s'  MTU=%d  chunk=%d",
             BLE_DEVICE_NAME, BLE_MTU, BLE_CHUNK);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, " LED  : GPIO%d (WS2812) — blink=advertising  steady=connected", LED_PIN);
#else
    ESP_LOGI(TAG, " LED  : GPIO%d — blink=advertising  steady=connected", LED_PIN);
#endif
    ESP_LOGI(TAG, " Dump : OFF  (send 'h' to toggle)");
    ESP_LOGI(TAG, " Commands: h=hex dump  n=NimBLE log  s=status");

    // Suppress NimBLE host INFO logs by default — they fire on every notify()
    // call and flood the console during active data transfer.
    // Send 'n' in the monitor to toggle them back on for debugging.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // BLE→UART queue — must be created before nus_init registers on_ble_write
    s_uart_tx_q = xQueueCreate(TX_Q_DEPTH, sizeof(tx_item_t));
    assert(s_uart_tx_q != NULL);

    uart_init();
    nus_init(BLE_DEVICE_NAME, on_ble_write);

    // BLE→UART task: drains TX queue to UART1 (blocking writes stay off host task)
    xTaskCreatePinnedToCore(uart_tx_task, "uart_tx",
                            4096, NULL, 5, NULL, 1);

    // UART→BLE forwarding task pinned to core 1
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            4096, NULL, 5, NULL, 1);

    // LED status task pinned to core 1
    xTaskCreatePinnedToCore(led_task, "led",
                            2048, NULL, 3, NULL, 1);

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
        default:
            break;
        }
    }
}
