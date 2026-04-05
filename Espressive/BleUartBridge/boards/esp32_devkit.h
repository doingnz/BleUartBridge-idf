/**
 * boards/esp32_devkit.h — GPIO assignments for ESP32 DevKit (8 MB flash)
 *
 * LED: single-colour blue LED driven by plain GPIO output.
 *      led_set(r, g, b) turns the LED on when any colour component is non-zero.
 */
#pragma once

#define TX_PIN   12   /* UART1 TX  → device RX  */
#define RX_PIN    4   /* UART1 RX  ← device TX  */
#define RTS_PIN  13   /* RTS       → device CTS (hardware flow control) */
#define CTS_PIN  15   /* CTS       ← device RTS (hardware flow control) */
#define LED_PIN  14   /* Blue LED  — blink=advertising, steady=connected */
