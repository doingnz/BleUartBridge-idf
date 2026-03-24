/**
 * main.c  —  BLE ↔ UART bridge  (ESP-IDF / NimBLE)
 *
 * Bridges a BLE Nordic UART Service (NUS) connection to UART1 with
 * hardware RTS/CTS flow control on ESP32-S3.
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  BLE client  ◄──NUS notify──►  ESP32-S3  ◄──UART1 RTS/CTS──►  Device │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * Pin assignments:
 *   GPIO17  UART1 TX  →  device RX
 *   GPIO8   UART1 RX  ←  device TX
 *   GPIO21  RTS       →  device CTS  (hardware flow control)
 *   GPIO47  CTS       ←  device RTS  (hardware flow control)
 *
 * Task layout:
 *   ble_host_task  (core 0, NimBLE)   — BLE stack, GAP/GATT events
 *   uart_rx_task   (core 1)           — UART1 RX → BLE notifications
 *   app_main       (core 1)           — init + interactive console on UART0
 *
 * Console commands (via idf.py monitor or any 115200-baud terminal):
 *   h   toggle hex dump of bridged data
 *   s   print status (connection, byte counts, UART HW buffer)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "ble_nus.h"

static const char *TAG = "BRIDGE";

// ── Pin and parameter constants ───────────────────────────────────────────────

#define UART_PORT       UART_NUM_1
#define TX_PIN          17
#define RX_PIN          8
#define RTS_PIN         21
#define CTS_PIN         47
#define UART_BAUD       115200
#define UART_RX_BUF     4096    // UART driver RX ring buffer

#define BLE_DEVICE_NAME "BP+ Bridge"
#define BLE_MTU         512
#define BLE_CHUNK       128     // max bytes per BLE notification

#define FC_THRESH       122     // RTS deasserts when UART RX FIFO ≥ this many bytes

// ── Bridge state ──────────────────────────────────────────────────────────────

static volatile bool          hex_dump_on       = false;
static volatile bool          nimble_log_verbose = false;  // NimBLE INFO logs off by default
static volatile unsigned long bytes_to_uart  = 0;
static volatile unsigned long bytes_from_uart = 0;

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
// Called from the NimBLE host task (core 0).  uart_write_bytes is thread-safe.

static void on_ble_write(const uint8_t *data, size_t len)
{
    hex_dump("BLE→UART", data, len);

    // uart_write_bytes blocks when the TX software buffer is full.
    // This provides natural backpressure if the device deasserts CTS:
    // the TX FIFO pauses, the software TX buffer fills, and the call
    // blocks until space is available.  No data is dropped.
    int written = uart_write_bytes(UART_PORT, (const char *)data, len);
    if (written > 0) {
        bytes_to_uart += written;
    }
}

// ── UART RX task: UART1 → BLE notifications ───────────────────────────────────

static void uart_rx_task(void *arg)
{
    uint8_t buf[BLE_CHUNK];

    ESP_LOGI(TAG, "UART→BLE task started");

    for (;;) {
        // Block up to 20 ms waiting for data; non-zero timeout keeps CPU low
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0) continue;

        if (!nus_is_connected()) {
            // Drain received bytes silently if no BLE client is connected
            continue;
        }

        hex_dump("UART→BLE", buf, n);

        int rc = nus_notify(buf, n);
        if (rc == 0) {
            bytes_from_uart += n;
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
    ESP_LOGI(TAG, " BLE       : %s", nus_is_connected() ? "connected" : "advertising");
    ESP_LOGI(TAG, " Hex dump  : %s", hex_dump_on ? "ON" : "OFF");
    ESP_LOGI(TAG, " NimBLE log: %s", nimble_log_verbose ? "VERBOSE" : "quiet");
    ESP_LOGI(TAG, " →UART     : %lu bytes", bytes_to_uart);
    ESP_LOGI(TAG, " ←UART     : %lu bytes", bytes_from_uart);
    ESP_LOGI(TAG, " UART hwBuf: %u bytes", (unsigned)hw_buf);
    ESP_LOGI(TAG, "────────────────────────────────────────");
}

// ── Entry point ───────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "  BLE UART Bridge  —  ESP32-S3");
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, " UART : TX=%d  RX=%d  RTS=%d  CTS=%d  %d baud",
             TX_PIN, RX_PIN, RTS_PIN, CTS_PIN, UART_BAUD);
    ESP_LOGI(TAG, " BLE  : '%s'  MTU=%d  chunk=%d",
             BLE_DEVICE_NAME, BLE_MTU, BLE_CHUNK);
    ESP_LOGI(TAG, " Dump : OFF  (send 'h' to toggle)");
    ESP_LOGI(TAG, " Commands: h=hex dump  n=NimBLE log  s=status");

    // Suppress NimBLE host INFO logs by default — they fire on every notify()
    // call and flood the console during active data transfer.
    // Send 'n' in the monitor to toggle them back on for debugging.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    uart_init();
    nus_init(BLE_DEVICE_NAME, on_ble_write);

    // UART→BLE forwarding task pinned to core 1
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            4096, NULL, 5, NULL, 1);

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
