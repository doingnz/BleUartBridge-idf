/**
 * boards/esp32s3_devkit.h — GPIO assignments for ESP32-S3 DevKit (S3-N16R8)
 *
 * LED: WS2812 RGB LED (single pixel) on GPIO48, driven via RMT peripheral.
 *      led_set(r, g, b) sets the pixel colour; use dim values (≤ 16) to
 *      avoid drawing too much current from the 3.3 V rail.
 */
#pragma once

#define TX_PIN   17   /* UART1 TX  → device RX  */
#define RX_PIN    8   /* UART1 RX  ← device TX  */
#define RTS_PIN  21   /* RTS       → device CTS (hardware flow control) */
#define CTS_PIN  47   /* CTS       ← device RTS (hardware flow control) */
#define LED_PIN  48   /* WS2812 RGB LED — blink=advertising, steady=connected */
