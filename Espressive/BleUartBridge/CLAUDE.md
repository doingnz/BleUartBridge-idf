# BleUartBridge — ESP-IDF Native Implementation

## Purpose

Native ESP-IDF equivalent of `D:\BPplus\Ardunio\BleUartBridge\BleUartBridge.ino`.
Bridges a BLE Nordic UART Service (NUS) connection to UART1 with hardware
RTS/CTS flow control on ESP32-S3.

## Build & flash

```bash
# One-time: set up IDF environment (adjust path to your IDF install)
. $IDF_PATH/export.sh          # Linux/macOS
# or: .\export.ps1             # Windows PowerShell

cd D:\BPplus\Espressive\BleUartBridge

idf.py set-target esp32s3
idf.py build
idf.py -p COM<N> flash monitor
```

Console commands in the monitor:
| Key | Action |
|-----|--------|
| `h` | Toggle hex dump (logs both directions with offset, hex, ASCII) |
| `n` | Toggle NimBLE verbose logging (suppressed by default — fires on every notify) |
| `s` | Print status: BLE state, byte counts, UART HW buffer size |

## File structure

```
BleUartBridge/
├── CMakeLists.txt          Top-level IDF project file
├── sdkconfig.defaults      Build configuration (NimBLE, UART ISR in IRAM)
├── CLAUDE.md               This file
└── main/
    ├── CMakeLists.txt      Component registration
    ├── main.c              app_main, UART init, UART→BLE task, console
    ├── ble_nus.c           NimBLE NUS GATT server implementation
    └── ble_nus.h           Public API for the NUS module
```

## Architecture

### Task layout

```
Core 0:  ble_host_task     NimBLE host (created by nimble_port_freertos_init)
                            Handles all BLE events, GAP, GATT callbacks
Core 1:  uart_rx_task      Reads UART1 ring buffer → nus_notify()
Core 1:  app_main          Initialisation + interactive console (getchar loop)
```

### Data flow

```
BLE client writes to NUS RX char (6E400002)
    └─► on_ble_write() callback (NimBLE host task, core 0)
            └─► uart_write_bytes(UART_PORT, data, len)
                    ├─► TX FIFO → GPIO17 → device
                    └─► Blocks naturally when device deasserts CTS (hardware FC)

Device UART TX → GPIO8 → RX FIFO → UART driver ring buffer (4096 B)
    └─► uart_rx_task (core 1) polls uart_read_bytes() every 20 ms
            └─► nus_notify(buf, n)
                    └─► ble_gatts_notify_custom() → NUS TX char (6E400003) → client
```

### Hardware flow control

UART1 is configured with `UART_HW_FLOWCTRL_CTS_RTS` at threshold 122/128 FIFO bytes:

- **RTS (GPIO21)**: deasserted when the RX FIFO ≥ 122 bytes → tells device to pause TX
- **CTS (GPIO47)**: monitored before each TX byte → pauses sending if device is not ready

`uart_write_bytes()` blocks when the TX FIFO is full (TX buffer size = 0, i.e. no SW buffer).
This creates natural backpressure from CTS: if the device deasserts CTS, the FIFO stops
draining, the write call blocks in `on_ble_write()`, and no bytes are dropped.

## Design decisions

### NimBLE vs Bluedroid

**NimBLE** was chosen over Bluedroid because:
- ~50% smaller RAM/flash footprint
- Fully maintained by Espressif for IDF v5+
- Cleaner C API with no hidden heap allocations
- Recommended for new ESP32-S3 peripheral designs

### UART TX software buffer

The TX software buffer size is set to **0** (disabled). This means `uart_write_bytes()`
blocks in the calling task (the NimBLE host task) when the hardware TX FIFO is full due to
CTS being deasserted. This is intentional: it propagates device-side backpressure all the
way back to the BLE write handler without requiring an extra queue or task.

If non-blocking BLE writes are needed in future, add a TX software buffer and a separate
UART TX task with a FreeRTOS queue between the BLE callback and the UART write.

### UUID byte order

NimBLE's `BLE_UUID128_INIT()` expects bytes in **little-endian (wire) order**.
The NUS UUIDs `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` through `...03...` are stored as:
```
0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01-03,0x00,0x40,0x6e
```

### BLE chunk size

`BLE_CHUNK = 128` bytes per notification. The negotiated MTU is 512 − 3 = 509 bytes maximum
payload, but 128 bytes keeps notification latency low and avoids congestion if the client
is slow to consume. This can be increased if throughput needs to be maximised.

### Scan response advertising

The 128-bit NUS service UUID (18 bytes) is placed in the **scan response**, not the
advertising PDU, to leave room for flags and the device name in the 31-byte ADV PDU.
Active-scanning clients (including Windows BLE and Android) always request the scan
response, so discovery is unaffected.

## Differences from the Arduino version

| Feature | Arduino (`BleUartBridge.ino`) | This project |
|---------|-------------------------------|--------------|
| BLE stack | Bluedroid (via Arduino wrapper) | NimBLE (native IDF) |
| UART init | `HardwareSerial` + `uart_set_pin` | `uart_driver_install` directly |
| Hex dump output | `Serial.printf` | `ESP_LOGI` (tagged, timestamped) |
| Console input | `Serial.available()` / `Serial.read()` | `getchar()` via VFS |
| TX buffer | `availableForWrite()` retry loop | `uart_write_bytes` blocking (buffer size=0) |
| BLE notify | `txChar->setValue()` + `notify()` | `ble_gatts_notify_custom()` with mbuf |

## Pin assignments

| Signal | GPIO | Direction |
|--------|------|-----------|
| UART1 TX | 17 | ESP32-S3 → device |
| UART1 RX | 8  | device → ESP32-S3 |
| RTS | 21 | ESP32-S3 → device CTS |
| CTS | 47 | device RTS → ESP32-S3 |
