# BleUartBridge

ESP32-S3 firmware that bridges a BLE [Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html) connection to a hardware UART with full RTS/CTS flow control.

A BLE client (phone app, web app via Web Bluetooth) connects and gets a transparent serial pipe to whatever device is wired to UART1.

```
BLE client  ◄──NUS notify / write──►  ESP32-S3  ◄──UART1 RTS/CTS──►  Device
```

## Hardware

Tested on ESP32-S3 DevKit. Pin assignments:

| Signal    | GPIO | Direction              |
|-----------|------|------------------------|
| UART1 TX  | 17   | ESP32-S3 → device RX   |
| UART1 RX  | 8    | device TX → ESP32-S3   |
| RTS       | 21   | ESP32-S3 → device CTS  |
| CTS       | 47   | device RTS → ESP32-S3  |

UART settings: **115200 baud, 8N1, hardware RTS/CTS flow control**.

## Build and flash

```bash
# One-time: activate the ESP-IDF environment
. $IDF_PATH/export.sh          # Linux/macOS
# .\export.ps1                 # Windows PowerShell

cd BleUartBridge

# First build: generate sdkconfig from defaults
idf.py set-target esp32s3
idf.py build
idf.py -p COM<N> flash monitor
```

> **Note:** if you change `sdkconfig.defaults`, delete `sdkconfig` before rebuilding so the new defaults take effect.

## Console commands

The firmware exposes an interactive console on the USB Serial/JTAG port (the same port used by `idf.py monitor`).

| Key | Action |
|-----|--------|
| `h` | Toggle hex dump — logs every bridged byte in both directions with offset, hex, and ASCII columns |
| `n` | Toggle NimBLE verbose logging — suppressed by default because it logs on every notification |
| `s` | Print status: BLE connection state, byte counters, UART hardware buffer depth |

## BLE details

### Advertising

| PDU            | Contents |
|----------------|----------|
| ADV PDU        | Flags + NUS 128-bit service UUID |
| Scan response  | Complete local name `"BP+ Bridge"` |

Placing the service UUID in the ADV PDU (not the scan response) is required for reliable discovery on Chrome/Windows via the Web Bluetooth API, which uses the WinRT BLE stack and may not process scan responses in time for the `requestDevice()` picker.

### NUS UUIDs

| Role      | UUID                                   | Properties      |
|-----------|----------------------------------------|-----------------|
| Service   | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | —               |
| RX char   | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write NR |
| TX char   | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify          |

- **RX** (client → ESP32): data is forwarded to UART1 TX.
- **TX** (ESP32 → client): UART1 RX data is sent as notifications in 128-byte chunks.

### MTU

Preferred ATT MTU is 512 bytes (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`). The connected client negotiates the actual MTU; the ESP32 accepts whatever the client proposes up to 512.

## Flow control

### CTS (incoming BLE writes → UART TX)

`uart_write_bytes()` is called directly from the NimBLE GATT write callback with no software TX buffer (`tx_buffer_size = 0`). If the device deasserts CTS, the hardware TX FIFO stops draining and `uart_write_bytes()` blocks — naturally throttling the BLE write handler without dropping bytes.

### RTS (UART RX → outgoing BLE notifications)

`uart_rx_task` reads 128-byte chunks from the UART driver ring buffer and sends them as NUS notifications. When the NimBLE mbuf pool is exhausted (the BLE client is consuming data more slowly than UART is producing it):

1. `nus_notify()` returns failure.
2. The task holds the pending chunk and **stops calling `uart_read_bytes()`**.
3. The UART driver ring buffer (4096 bytes) fills.
4. The hardware FIFO can no longer be drained by the driver interrupt and fills to the RTS threshold (122/128 bytes).
5. **Hardware RTS asserts**, pausing the device sender.
6. The BLE host task (core 0) continues draining its transmit queue and freeing mbufs.
7. `nus_notify()` succeeds on the next retry; normal flow resumes and RTS deasserts.

## Architecture

### Tasks

| Task            | Core | Priority | Role |
|-----------------|------|----------|------|
| `ble_host_task` | 0    | (NimBLE) | NimBLE host — all BLE events, GAP, GATT callbacks |
| `uart_rx_task`  | 1    | 5        | UART1 RX ring buffer → BLE notifications |
| `app_main`      | 1    | 1        | Initialisation + interactive console |

### Data flow

```
BLE client writes to NUS RX char (6E400002)
    └─► on_ble_write() [NimBLE host task, core 0]
            └─► uart_write_bytes(UART1)
                    └─► GPIO17 → device (blocks if device deasserts CTS)

Device UART TX → GPIO8 → UART1 FIFO → driver ring buffer (4096 B)
    └─► uart_rx_task [core 1] reads up to 128 B
            └─► nus_notify()
                    ├── success → ble_gatts_notify_custom() → NUS TX char (6E400003) → client
                    └── failure → hold chunk, stop reading → ring buffer fills → RTS asserts
```

## Why NimBLE

NimBLE was chosen over Bluedroid:

- ~50% smaller RAM and flash footprint
- Fully maintained by Espressif for IDF v5+
- Cleaner C API with no hidden heap allocations
- Recommended for new ESP32-S3 peripheral designs

## File structure

```
BleUartBridge/
├── CMakeLists.txt        Top-level IDF project file
├── sdkconfig.defaults    Build configuration (NimBLE, USB console, UART ISR in IRAM)
├── README.md             This file
├── CLAUDE.md             AI assistant context and design notes
└── main/
    ├── CMakeLists.txt    Component registration
    ├── main.c            app_main, UART init, UART→BLE task, console
    ├── ble_nus.c         NimBLE NUS GATT server + advertising
    └── ble_nus.h         Public API
```
