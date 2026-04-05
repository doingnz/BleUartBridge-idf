# BleUartBridge

ESP32 / ESP32-S3 firmware that bridges a BLE [Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html) connection to a hardware UART with full RTS/CTS flow control.

A BLE client (phone app, web app via Web Bluetooth) connects and gets a transparent serial pipe to whatever device is wired to UART1.

```
BLE client  ◄──NUS notify / write──►  ESP32 / ESP32-S3  ◄──UART1 RTS/CTS──►  Device
```

Both the ESP32 DevKit and ESP32-S3 DevKit (S3-N16R8) are supported from a single codebase. Select the target at build time — no source changes required.

## Hardware

### ESP32-S3 DevKit (S3-N16R8) — 16 MB flash, 8 MB PSRAM

| Signal | GPIO | Direction |
|--------|------|-----------|
| UART1 TX | 17 | ESP32-S3 → device RX |
| UART1 RX | 8  | device TX → ESP32-S3 |
| RTS | 21 | ESP32-S3 → device CTS |
| CTS | 47 | device RTS → ESP32-S3 |
| LED | 48 | WS2812 RGB (RMT) |

Console port: built-in USB Serial/JTAG (no external adapter needed).

### ESP32 DevKit — 8 MB flash

| Signal | GPIO | Direction |
|--------|------|-----------|
| UART1 TX | 12 | ESP32 → device RX |
| UART1 RX | 4  | device TX → ESP32 |
| RTS | 13 | ESP32 → device CTS |
| CTS | 15 | device RTS → ESP32 |
| LED | 14 | Blue LED (plain GPIO, **active-low**: GPIO=0 → on) |

Console port: UART0 via on-board CH340/CP2102 USB-UART chip.

UART settings for both: **115200 baud, 8N1, hardware RTS/CTS flow control**.

## Build and flash

### Prerequisites

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/latest/) installed
- ESP-IDF environment activated in the current shell:

```bat
C:\esp\.espressif\v6.0\esp-idf\export.bat
```

### Using build.cmd (Windows)

```bat
cd D:\BPplus\Espressive\BleUartBridge

build.cmd esp32s3              REM ESP32-S3 DevKit on default COM9
build.cmd esp32                REM ESP32 DevKit on default COM8
build.cmd esp32s3 COM5         REM ESP32-S3 on a specific COM port
build.cmd esp32 COM3           REM ESP32 on a specific COM port
```

`build.cmd` runs `idf.py build flash monitor` with the correct target and
configuration. If you switch targets, it automatically cleans the build
directory first so the CMake cache and `sdkconfig` are regenerated cleanly.

### Manual commands

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

> **Note:** when switching targets manually, delete `sdkconfig` and `build/`
> before rebuilding so the CMake cache and sdkconfig are regenerated.

## Console commands

The firmware exposes an interactive console on the monitor port.

| Key | Action |
|-----|--------|
| `h` | Toggle hex dump — logs every bridged byte in both directions with offset, hex, and ASCII columns |
| `n` | Toggle NimBLE verbose logging — suppressed by default because it logs on every notification |
| `s` | Print status: BLE state, disconnect count, byte counters, TX queue depth, heap free / minimum |

## LED status

| State | Behaviour |
|-------|-----------|
| Advertising (no client) | Blinks dim blue at 1 Hz |
| Client connected | Steady dim blue |

On the ESP32-S3 the LED is a WS2812 RGB pixel (GPIO48). On the ESP32 it is a single-colour blue LED on GPIO14. The ESP32 DevKit LED is **active-low** (anode to VCC, cathode to GPIO) — `gpio_set_level(0)` turns it on.

## BLE details

### Advertising

| PDU | Contents |
|-----|----------|
| ADV PDU | Flags + NUS 128-bit service UUID |
| Scan response | Complete local name `"BP+ Bridge"` |

The service UUID is in the ADV PDU (not the scan response) for reliable discovery via the Web Bluetooth API on Chrome/Windows, which may not process scan responses before the `requestDevice()` picker appears.

### NUS UUIDs

| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX char | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write NR |
| TX char | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- **RX** (client → ESP32): data forwarded to UART1 TX.
- **TX** (ESP32 → client): UART1 RX data sent as notifications in 128-byte chunks.

### MTU

Preferred ATT MTU is 512 bytes. The client negotiates the actual MTU; the ESP32 accepts whatever the client proposes up to 512.

## Flow control

### BLE → UART (CTS direction)

`on_ble_write()` runs on the NimBLE host task. It posts received data to a FreeRTOS queue and returns immediately. A dedicated `uart_tx_task` dequeues items and calls `uart_write_bytes()`. When the device deasserts CTS, `uart_write_bytes()` blocks inside `uart_tx_task` — this is harmless because `uart_tx_task` is not the BLE host task, so NimBLE continues processing events normally.

### UART → BLE (RTS direction)

`uart_rx_task` reads 128-byte chunks and sends them as NUS notifications. When the NimBLE mbuf pool is exhausted:

1. `nus_notify()` returns `NUS_ERR_NOMEM`.
2. The task holds the pending chunk and **stops calling `uart_read_bytes()`**.
3. The UART driver ring buffer (4096 bytes) fills up.
4. The hardware FIFO fills to the RTS threshold (122/128 bytes).
5. **Hardware RTS asserts**, pausing the device sender.
6. The BLE host task continues draining its queue and freeing mbufs.
7. `nus_notify()` succeeds on the next retry; RTS deasserts and normal flow resumes.

## Architecture

### Tasks

| Task | Core | Priority | Role |
|------|------|----------|------|
| `ble_host_task` | 0 | (NimBLE) | NimBLE host — all BLE events, GAP, GATT callbacks |
| `uart_tx_task`  | 1 | 5 | Drains BLE→UART queue to UART1 TX |
| `uart_rx_task`  | 1 | 5 | UART1 RX → BLE notifications |
| `led_task`      | 1 | 3 | LED status indicator |
| `app_main`      | 1 | 1 | Initialisation + interactive console |

### Data flow

```
BLE client writes to NUS RX char (6E400002)
    └─► on_ble_write() [NimBLE host task, core 0]
            └─► xQueueSend(s_uart_tx_q)  [non-blocking]
                    └─► uart_tx_task [core 1]
                            └─► uart_write_bytes(UART1)
                                    └─► UART1 TX pin → device
                                        (blocks here if device deasserts CTS)

Device UART TX → UART1 RX pin → FIFO → driver ring buffer (4096 B)
    └─► uart_rx_task [core 1]
            └─► nus_notify()
                    ├── 0             → ble_gatts_notify_custom() → NUS TX char → client
                    ├── NUS_ERR_NOMEM → hold chunk, yield 2 ms, retry → backpressure → RTS
                    └── NUS_ERR_CONN  → discard chunk, pause 50 ms
```

## Why NimBLE

NimBLE was chosen over Bluedroid:

- ~50% smaller RAM and flash footprint
- Fully maintained by Espressif for IDF v5+
- Cleaner C API with no hidden heap allocations
- Recommended for new ESP32 peripheral designs

## File structure

```
BleUartBridge/
├── CMakeLists.txt                  Top-level IDF project (target set at build time)
├── build.cmd                       Build/flash/monitor helper script
├── sdkconfig.defaults              Shared settings: NimBLE, mbuf pool, UART ISR
├── sdkconfig.defaults.esp32        ESP32: target, 8 MB flash, UART0 console
├── sdkconfig.defaults.esp32s3      ESP32-S3: target, 16 MB flash, USB-JTAG console
├── boards/
│   ├── esp32_devkit.h              Pin assignments — ESP32 DevKit
│   └── esp32s3_devkit.h            Pin assignments — ESP32-S3 DevKit (S3-N16R8)
├── README.md                       This file
├── CLAUDE.md                       Design notes and AI assistant context
└── main/
    ├── CMakeLists.txt              Component registration
    ├── idf_component.yml           Managed dependency: espressif/led_strip
    ├── main.c                      Tasks, LED abstraction, UART init, console
    ├── ble_nus.c                   NimBLE NUS GATT server + advertising
    └── ble_nus.h                   Public API
```
