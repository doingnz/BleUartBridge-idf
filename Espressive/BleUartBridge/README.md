# BleUartBridge

ESP32 / ESP32-S3 firmware that bridges a BLE [Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html) connection to a hardware UART with full RTS/CTS flow control.

A BLE client (phone app, web app via Web Bluetooth) connects and gets a transparent serial pipe to whatever device is wired to UART1.

```
BLE client  ◄──NUS notify / write──►  ESP32 / ESP32-S3  ◄──UART1 RTS/CTS──►  Device
```

Both the ESP32 DevKit and ESP32-S3 DevKit (S3-N16R8) are supported from a single codebase. Select the target at build time — no source changes required.

The same connection also exposes a **BP+ Mgmt service** for authenticated
runtime configuration (UART baud, flow control, BLE advertising interval,
etc.) and signed **over-the-air firmware updates**. A Web Bluetooth UI at
`host/bpconnect/` drives both.

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
| `s` | Print status: BLE state, disconnect count, byte counters, TX queue depth, heap, DFU state |
| `c` | Clear statistics — resets byte counters, dropped-byte count, NOMEM retry total, and disconnect count |
| `i` | Print app info — firmware version, running + next OTA partitions, OTA state |
| `r` | Factory-reset NVS (press `Y` within 5 s to confirm); reboots |

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
| Scan response | Complete local name `"BP+ Bridge XXXX"` |

The device name includes the last 4 hex digits of the BLE MAC address (e.g. `"BP+ Bridge 96EE"`) so multiple bridges can be distinguished during scanning. The MAC suffix matches the address shown in BLE scanner apps.

The service UUID is in the ADV PDU (not the scan response) for reliable discovery via the Web Bluetooth API on Chrome/Windows, which may not process scan responses before the `requestDevice()` picker appears.

### NUS UUIDs

| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX char | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write NR |
| TX char | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- **RX** (client → ESP32): data forwarded to UART1 TX.
- **TX** (ESP32 → client): UART1 RX data sent as notifications in 128-byte chunks.

### BP+ Mgmt service UUIDs

Discovered after connect (not advertised). Web Bluetooth clients must list
this service UUID in `optionalServices` when calling `requestDevice()`.

| Role | UUID | Properties |
|------|------|------------|
| Service    | `7E500001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| AUTH       | `7E500002-…` | Write + Notify |
| INFO       | `7E500003-…` | Read (88-byte packed struct) |
| CFG_CONTROL | `7E500004-…` | Write + Notify (TLV protocol) |
| DFU_CONTROL | `7E500005-…` | Write + Notify |
| DFU_DATA   | `7E500006-…` | Write No Response (`u16 seq + payload`) |

Wire-protocol opcodes and status codes are documented in the headers of
`main/dfu.h`, `main/auth.h`, and in the comment block near `CFG_OP_GET` at
the top of `main/ble_mgmt.c`.

### MTU

Preferred ATT MTU is 512 bytes. The client negotiates the actual MTU; the
ESP32 accepts whatever the client proposes up to 512. DFU caps chunk size to
`negotiated_mtu − 5` (Chrome on Windows typically negotiates ~247, yielding
242-byte payload chunks) — see the `START_RSP.max_chunk_size` field.

## Runtime configuration

Writable TLVs (via CFG_CONTROL or the `host/bpconnect/` UI):

| ID | Name | Type | Default | Apply |
|----|------|------|---------|-------|
| 0x01 | `uart_baud`          | u32  | 115200 | reboot |
| 0x02 | `uart_flowctrl`      | u8   | 1 (RTS/CTS) | reboot |
| 0x03 | `uart_databits`      | u8   | 8 | reboot |
| 0x04 | `uart_parity`        | u8   | 0 (None) | reboot |
| 0x05 | `uart_stopbits`      | u8   | 1 | reboot |
| 0x06 | `name_suffix`        | str[8] | MAC-derived | reboot |
| 0x07 | `ble_tx_power_dbm`   | i8   | 3 dBm | reboot |
| 0x08 | `hexdump_default`    | u8   | 0 | live |
| 0x09 | `dfu_enabled`        | u8   | 1 | live |
| 0x0A | `auth_required`      | u8   | 1 | live |
| 0x0B | `factory_reset`      | trigger | — | reboot |
| 0x0C | `ble_adv_interval_ms`| u16  | 100 | live (re-kicks advertising) |
| 0x0D | `ble_preferred_mtu`  | u16  | 509 | reboot |

Values live in the NVS namespace `bpcfg`. `r` on the console (or writing
`CFG_FACTORY_RESET`) erases the namespace and reboots.

## OTA firmware updates

### Key provisioning (one-off)

```
python tools/keygen.py          # generate ECDSA P-256 keypair → keys/*.pem
python tools/embed_pubkey.py    # overwrite main/signing_pubkey.h with the public key
# edit main/secrets.h — replace the all-zero MASTER_KEY with 32 random bytes
build.cmd esp32s3               # rebuild + flash so the device trusts the new pubkey
```

Until these run, `FW_SIGNING_PUBKEY[]` is all zeros and every DFU attempt
fails at VERIFY with `DFU_ST_SIG_MISMATCH` — which is the intended fail-safe
behaviour for un-provisioned units.

### Signing a firmware release

```
python tools/sign_fw.py
# produces build/BleUartBridge.bin.sig (64 B raw r||s)
#          build/BleUartBridge.bin.sha (32 B sidecar)
```

### Pushing an update

```
cd host/bpconnect
python -m http.server 8080
# open http://localhost:8080 in Chrome
```

Flow: **Connect** → **Authenticate** (paste the 32-byte MASTER_KEY as 64 hex
chars) → **Firmware update**: choose `.bin` and `.bin.sig` → **Flash**.

The device:

1. Allocates the inactive OTA slot
2. Streams chunks through `esp_ota_write` (on a dedicated core-1 task)
3. Verifies rolling SHA-256 matches what START claimed
4. Verifies ECDSA-P256 signature against the embedded public key
5. Sets boot partition and reboots
6. On first boot of the new image, runs for 30 s, then calls
   `esp_ota_mark_app_valid_cancel_rollback()` — until that point the
   bootloader will revert on any reset

Downgrades are permitted (anti-rollback not enabled).

### Security model

| Layer | Mechanism | Stops |
|-------|-----------|-------|
| L2 | HMAC-SHA256 session auth, 3-fail/30 s lockout | replay, guessing, revoked bonds |
| L3 | SHA-256 over image, verified at VERIFY | corrupted transfers |
| L4 | ECDSA P-256 signature (64 B) | forged images from authed clients |
| L5 | Secure Boot v2 + Flash Encryption | **not enabled** — eFuse is one-way |

Without L5 a local attacker with JTAG can extract the master key from a
unit. The ECDSA private key is the remaining barrier — keep
`keys/signing_private.pem` out of CI logs, shared drives, and untrusted
hosts. See `keys/README.md` for rotation procedure.

## Flow control

### BLE → UART (CTS direction)

`on_ble_write()` runs on the NimBLE host task. It posts received data to a FreeRTOS queue and returns immediately. A dedicated `uart_tx_task` dequeues items and calls `uart_write_bytes()`. When the device deasserts CTS, `uart_write_bytes()` blocks inside `uart_tx_task` — this is harmless because `uart_tx_task` is not the BLE host task, so NimBLE continues processing events normally.

If the queue is full (CTS held off long enough to exhaust all 32 slots), `on_ble_write()` returns a non-zero value. `nus_rx_chr_cb` translates this into an ATT Error Response (`ATT_ERR_INSUFFICIENT_RES`), which causes clients using `writeValueWithResponse` to receive a rejection and retry. This provides true end-to-end backpressure rather than silent data loss.

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
├── CMakeLists.txt              Top-level IDF project (target set at build time)
├── build.cmd                   Build / flash / monitor helper
├── sdkconfig.defaults          Shared settings: NimBLE, mbuf pool, mbedTLS ECP, OTA rollback
├── sdkconfig.defaults.esp32    ESP32: target, 8 MB flash, UART0 console, partitions CSV
├── sdkconfig.defaults.esp32s3  ESP32-S3: target, 16 MB flash, USB-JTAG console, partitions CSV
├── partitions_esp32.csv        2× 3.87 MB OTA slots for 8 MB flash
├── partitions_esp32s3.csv      2× 7.87 MB OTA slots for 16 MB flash
├── boards/                     Per-board pin headers
├── keys/                       ECDSA signing keypair + rotation notes
├── tools/                      keygen.py, embed_pubkey.py, sign_fw.py
├── host/
│   └── bpconnect/              Web Bluetooth management UI (auth / config / DFU)
├── README.md                   This file
├── CLAUDE.md                   Design notes + AI assistant context
└── main/
    ├── main.c                  Tasks, LED abstraction, UART init, console
    ├── ble_nus.c/.h            NimBLE NUS GATT server + advertising
    ├── ble_mgmt.c/.h           BP+ Mgmt GATT service (auth / info / cfg / dfu)
    ├── auth.c/.h               HMAC-SHA256 challenge/response
    ├── cfg.c/.h                NVS-backed TLV config store
    ├── dfu.c/.h                OTA state machine + chunk queue
    ├── verify_ecdsa.c/.h       ECDSA P-256 verify (FIPS 186-4 §6.4)
    ├── sha256.c/.h             Self-contained SHA-256 + HMAC-SHA256
    ├── signing_pubkey.h        Embedded public key (generated)
    ├── secrets.h               MASTER_KEY (gitignored)
    └── secrets.example.h       Template
```
