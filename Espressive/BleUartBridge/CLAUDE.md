# BleUartBridge — ESP-IDF Native Implementation

## Purpose

Native ESP-IDF equivalent of `D:\BPplus\Ardunio\BleUartBridge\BleUartBridge.ino`.
Bridges a BLE Nordic UART Service (NUS) connection to UART1 with hardware
RTS/CTS flow control. Supports two hardware targets from a single codebase:

| Target | Board | LED |
|--------|-------|-----|
| `esp32` | ESP32 DevKit (8 MB flash) | GPIO14, single-colour blue, plain GPIO |
| `esp32s3` | ESP32-S3 DevKit S3-N16R8 (16 MB flash, 8 MB PSRAM) | GPIO48, WS2812 RGB |

## Build & flash

```bat
REM One-time per shell session: activate ESP-IDF environment
C:\esp\.espressif\v6.0\esp-idf\export.bat

cd D:\BPplus\Espressive\BleUartBridge

build.cmd esp32s3          REM ESP32-S3 on default COM9
build.cmd esp32            REM ESP32   on default COM8
build.cmd esp32s3 COM5     REM ESP32-S3 on a different COM port
```

`build.cmd` automatically detects a target change, runs `idf.py fullclean`,
and re-builds from scratch when switching between `esp32` and `esp32s3`.

The equivalent manual commands are:

```bash
# ESP32-S3
idf.py -DIDF_TARGET=esp32s3 \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" \
       -p COM9 build flash monitor

# ESP32
idf.py -DIDF_TARGET=esp32 \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" \
       -p COM8 build flash monitor
```

Console commands in the monitor:
| Key | Action |
|-----|--------|
| `h` | Toggle hex dump (logs both directions with offset, hex, ASCII) |
| `n` | Toggle NimBLE verbose logging (suppressed by default) |
| `s` | Print status: BLE state, disconnect count, byte counters, TX queue depth, heap |

## File structure

```
BleUartBridge/
├── CMakeLists.txt                  Top-level IDF project (no hardcoded target)
├── build.cmd                       Build/flash/monitor helper — see Build section
├── sdkconfig.defaults              Shared: NimBLE, mbuf pool, UART ISR in IRAM
├── sdkconfig.defaults.esp32        ESP32-specific: target, flash size, UART0 console
├── sdkconfig.defaults.esp32s3      ESP32-S3-specific: target, flash size, USB-JTAG console
├── boards/
│   ├── esp32_devkit.h              GPIO assignments for ESP32 DevKit
│   └── esp32s3_devkit.h            GPIO assignments for ESP32-S3 DevKit
├── CLAUDE.md                       This file
├── README.md                       User-facing documentation
└── main/
    ├── CMakeLists.txt              Component registration (includes led_strip)
    ├── idf_component.yml           Managed component: espressif/led_strip
    ├── main.c                      app_main, tasks, LED abstraction, console
    ├── ble_nus.c                   NimBLE NUS GATT server implementation
    └── ble_nus.h                   Public API for the NUS module
```

## Architecture

### Task layout

```
Core 0:  ble_host_task     NimBLE host (created by nimble_port_freertos_init)
                            Handles all BLE events, GAP, GATT callbacks

Core 1:  uart_tx_task      Drains s_uart_tx_q → uart_write_bytes() on UART1
                            (blocking writes are safe here — not the host task)
Core 1:  uart_rx_task      Reads UART1 ring buffer → nus_notify()
Core 1:  led_task          LED status: blink=advertising, steady=connected
Core 1:  app_main          Initialisation + interactive console (getchar loop)
```

### Data flow

```
BLE client writes to NUS RX char (6E400002)
    └─► on_ble_write() callback (NimBLE host task, core 0)
            └─► xQueueSend(s_uart_tx_q)   ← non-blocking, returns immediately
                    └─► uart_tx_task (core 1)
                            └─► uart_write_bytes(UART_PORT, ...)
                                    └─► TX FIFO → UART1 TX pin → device
                                        (blocks here if device deasserts CTS — correct,
                                         this task is not the BLE host task)

Device UART TX → UART1 RX pin → RX FIFO → UART driver ring buffer (4096 B)
    └─► uart_rx_task (core 1) polls uart_read_bytes() every 20 ms
            └─► nus_notify(buf, n)
                    ├── 0         → ble_gatts_notify_custom() → NUS TX (6E400003) → client
                    ├── NUS_ERR_NOMEM → hold buf, yield 2 ms, retry (backpressure)
                    └── NUS_ERR_CONN  → discard buf, pause 50 ms (avoid host-task starvation)
```

### Hardware flow control

UART1 is configured with `UART_HW_FLOWCTRL_CTS_RTS` at threshold 122/128 FIFO bytes:

- **RTS**: deasserted when the RX FIFO ≥ 122 bytes → tells device to pause TX
- **CTS**: monitored by the UART hardware before each TX byte → pauses sending if
  device is not ready

See pin assignments in `boards/esp32_devkit.h` and `boards/esp32s3_devkit.h`.

## Design decisions

### uart_tx_task — decoupling BLE host from UART writes

`on_ble_write()` is called on the NimBLE host task (core 0). Calling
`uart_write_bytes()` directly there would block the host task when CTS is
deasserted, preventing NimBLE from processing HCI events and recycling mbufs —
eventually causing `nus_notify()` to fail with `NUS_ERR_NOMEM` in a death spiral.

The fix: `on_ble_write()` posts to a 32-slot FreeRTOS queue
(`TX_Q_DEPTH × TX_MAX_LEN ≈ 16 KB`) and returns immediately.
`uart_tx_task` on core 1 dequeues and calls `uart_write_bytes()`, where blocking
on CTS is harmless.

If the queue fills (device held off by CTS for a long time), writes are dropped
and counted in `bytes_dropped_tx`. This is acceptable — the BLE client will be
flow-controlled by BLE notification acknowledgements before the queue exhausts.

### nus_notify error codes

`nus_notify()` returns one of three values:

| Return | Meaning | Action |
|--------|---------|--------|
| `0` | Success | Advance, read next chunk |
| `NUS_ERR_NOMEM (-1)` | NimBLE mbuf pool temporarily exhausted | Hold chunk, yield 2 ms, retry |
| `NUS_ERR_CONN (-2)` | Not connected or fatal GAP error | Discard chunk, pause 50 ms |

Retrying on `NUS_ERR_CONN` (the old behaviour) starved the NimBLE host task
by hammering `ble_gatts_notify_custom()` while the host task was trying to
process the disconnect event, which prevented clean advertising restart.

### LED abstraction

`led_init()` and `led_set(r, g, b)` hide the hardware difference:

- **ESP32**: plain `gpio_set_level()` — the LED is **active-low** (anode to VCC,
  cathode to GPIO14), so `gpio_set_level(0)` = on, `gpio_set_level(1)` = off.
  `led_set()` inverts the level accordingly: any non-zero colour component drives
  the pin low (on); all-zero drives it high (off).
- **ESP32-S3**: `led_strip_set_pixel()` + `led_strip_refresh()` via RMT peripheral
  (`led_strip_new_rmt_device`, 10 MHz resolution).
  Keep colour values ≤ 16 to limit current draw from the 3.3 V rail.

The same `led_task()` body drives both: dim blue (0, 0, 8) steady when connected,
blinking at 1 Hz when advertising.

### NimBLE vs Bluedroid

**NimBLE** was chosen over Bluedroid because:
- ~50% smaller RAM/flash footprint
- Fully maintained by Espressif for IDF v5+
- Cleaner C API with no hidden heap allocations
- Recommended for new ESP32 peripheral designs

### BLE advertising layout

The 128-bit NUS service UUID is placed in the **ADV PDU** (not the scan response).
This is required for Chrome/Windows Web Bluetooth: the WinRT BLE stack may not
process scan responses before the `requestDevice()` picker opens, so the UUID
must be in the primary advertisement to appear in the filter results.

The device name `"BP+ Bridge"` goes in the **scan response** because the ADV PDU
is full (3 bytes flags + 18 bytes UUID128 = 21 bytes, leaving only 10 bytes free).

### BLE chunk size

`BLE_CHUNK = 128` bytes per notification. The negotiated MTU is up to 509 bytes
(512 − 3), but 128 bytes keeps per-notification latency low and avoids congestion
when the client is slow to consume. Increase if throughput must be maximised.

### NimBLE mbuf pool

`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=24` (up from the default 12).
Both RX writes (from client → ESP32) and TX notifications (ESP32 → client)
consume mbufs simultaneously during bidirectional stress tests. 24 blocks provides
headroom for sustained full-speed traffic in both directions.

## Pin assignments

### ESP32 DevKit (`boards/esp32_devkit.h`)

| Signal | GPIO | Direction |
|--------|------|-----------|
| UART1 TX | 12 | ESP32 → device |
| UART1 RX | 4  | device → ESP32 |
| RTS | 13 | ESP32 → device CTS |
| CTS | 15 | device RTS → ESP32 |
| Blue LED | 14 | Output (plain GPIO, **active-low**) |

### ESP32-S3 DevKit S3-N16R8 (`boards/esp32s3_devkit.h`)

| Signal | GPIO | Direction |
|--------|------|-----------|
| UART1 TX | 17 | ESP32-S3 → device |
| UART1 RX | 8  | device → ESP32-S3 |
| RTS | 21 | ESP32-S3 → device CTS |
| CTS | 47 | device RTS → ESP32-S3 |
| WS2812 LED | 48 | Output (RMT peripheral) |

## Differences from the Arduino version

| Feature | Arduino (`BleUartBridge.ino`) | This project |
|---------|-------------------------------|--------------|
| BLE stack | Bluedroid (via Arduino wrapper) | NimBLE (native IDF) |
| UART init | `HardwareSerial` + `uart_set_pin` | `uart_driver_install` directly |
| BLE→UART | Direct write in callback | FreeRTOS queue + uart_tx_task |
| Hex dump output | `Serial.printf` | `ESP_LOGI` (tagged, timestamped) |
| Console input | `Serial.available()` / `Serial.read()` | `getchar()` via VFS |
| BLE notify | `txChar->setValue()` + `notify()` | `ble_gatts_notify_custom()` with mbuf |
| LED | None | Target-specific (GPIO / WS2812) |
| Multi-target | No | Yes (ESP32 + ESP32-S3 from one codebase) |
