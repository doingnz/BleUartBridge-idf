# BleUartBridge — ESP-IDF Native Implementation

## Purpose

Native ESP-IDF equivalent of `D:\BPplus\Ardunio\BleUartBridge\BleUartBridge.ino`.
Bridges a BLE Nordic UART Service (NUS) connection to UART1 with hardware
RTS/CTS flow control. Supports two hardware targets from a single codebase:

| Target | Board | LED |
|--------|-------|-----|
| `esp32` | ESP32 DevKit (8 MB flash) | GPIO14, single-colour blue, plain GPIO |
| `esp32s3` | ESP32-S3 DevKit S3-N16R8 (16 MB flash, 8 MB PSRAM) | GPIO48, WS2812 RGB |

The firmware additionally exposes a **BP+ Mgmt GATT service** on the same
connection, providing:

- **HMAC-SHA256 challenge/response authentication** — per-device secret
  derived as `HMAC(MASTER_KEY, ble_mac)`; master key in `main/secrets.h`
  (gitignored).
- **TLV-based runtime configuration** — UART baud/flow control/parity/etc.,
  BLE advertising interval, MTU preference, hex-dump default, feature gates.
  Persisted in NVS, live-apply for runtime-tunable fields, reboot-apply for
  UART parameters.
- **OTA firmware update** — ECDSA P-256 signed images flashed through the
  BLE link; rolling SHA-256 integrity check during upload, signature verify
  at the VERIFY stage, `esp_ota_set_boot_partition` + rollback watchdog on
  APPLY. NUS bridge traffic is suspended during DFU.

Browser-side tooling is at `host/bpconnect/` — a self-contained Web Bluetooth
page (no framework) that handles the auth handshake, config edit/commit,
and signed OTA upload.

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
| `s` | Print status: BLE state, disconnect count, byte counters, TX queue depth, heap, DFU state |
| `c` | Clear stats: resets byte counters, dropped bytes, NOMEM retries, disconnect count |
| `i` | Print app info: firmware version, running/next OTA partitions, OTA state |
| `r` | Factory-reset NVS (requires confirming `Y` within 5 s); reboots |

## First-time provisioning

Three one-off steps before OTA works end-to-end:

1. **Master key** — replace the all-zero placeholder in `main/secrets.h` with
   32 random bytes. Every host tool (`host/bpconnect/` in-browser; future
   `BpDfuCli`) must hold the same bytes.
2. **Signing keypair** — `python tools/keygen.py` writes
   `keys/signing_private.pem` (gitignored) and `keys/signing_public.pem`.
3. **Embed the public key** — `python tools/embed_pubkey.py` rewrites
   `main/signing_pubkey.h` with the public key as a C array. Rebuild so the
   fleet ships with the trusted key baked in.

Until step 3 runs, the `FW_SIGNING_PUBKEY` placeholder is all zeros, so
`verify_ecdsa_p256()` fails closed and DFU at the VERIFY stage is rejected
with `DFU_ST_SIG_MISMATCH (9)`. That's the intended fail-safe.

Every firmware build delivered via OTA must be signed:

```
python tools/sign_fw.py        # produces build/BleUartBridge.bin.sig
```

## File structure

```
BleUartBridge/
├── CMakeLists.txt                  Top-level IDF project (no hardcoded target)
├── build.cmd                       Build/flash/monitor helper — see Build section
├── sdkconfig.defaults              Shared: NimBLE, mbuf pool, UART ISR, OTA rollback, mbedTLS ECP
├── sdkconfig.defaults.esp32        ESP32: target, 8 MB flash, UART0 console @460800, partition CSV
├── sdkconfig.defaults.esp32s3      ESP32-S3: target, 16 MB flash, USB-JTAG console, partition CSV
├── partitions_esp32.csv            2× 3.87 MB OTA slots + nvs/otadata/phy_init
├── partitions_esp32s3.csv          2× 7.87 MB OTA slots + nvs/otadata/phy_init
├── boards/
│   ├── esp32_devkit.h              GPIO assignments for ESP32 DevKit
│   └── esp32s3_devkit.h            GPIO assignments for ESP32-S3 DevKit
├── keys/
│   ├── README.md                   Key-rotation policy; produced by tools/keygen.py
│   └── signing_*.pem               ECDSA P-256 keypair (private PEM gitignored)
├── tools/
│   ├── keygen.py                   Generate signing keypair
│   ├── embed_pubkey.py             Public key → main/signing_pubkey.h
│   └── sign_fw.py                  build/BleUartBridge.bin → .bin.sig (64 B r||s)
├── host/
│   └── bpconnect/                  Web Bluetooth management UI (auth + config + DFU)
│       ├── index.html
│       ├── app.js
│       └── README.md
├── CLAUDE.md                       This file
├── README.md                       User-facing documentation
└── main/
    ├── CMakeLists.txt              Component registration
    ├── idf_component.yml           Managed component: espressif/led_strip
    ├── main.c                      app_main, tasks, LED abstraction, console
    ├── ble_nus.c/.h                NimBLE NUS GATT server + advertising
    ├── ble_mgmt.c/.h               BP+ Mgmt GATT service (AUTH, INFO, CFG, DFU chars)
    ├── auth.c/.h                   HMAC-SHA256 challenge/response + per-session state
    ├── cfg.c/.h                    NVS-backed TLV config store + live-apply callbacks
    ├── dfu.c/.h                    OTA state machine, chunk queue, signature verify
    ├── verify_ecdsa.c/.h           ECDSA P-256 verify on mbedTLS ECP primitives
    ├── sha256.c/.h                 Self-contained SHA-256 + HMAC-SHA256 (no mbedTLS dep)
    ├── secrets.h                   32-byte MASTER_KEY (gitignored; placeholder committed)
    ├── secrets.example.h           Committed template for secrets.h
    └── signing_pubkey.h            ECDSA P-256 public key (generated by embed_pubkey.py)
```

## Architecture

### Task layout

```
Core 0:  ble_host_task     NimBLE host (created by nimble_port_freertos_init)
                            Handles all BLE events, GAP, GATT callbacks
                            Dispatches AUTH / CFG / DFU char writes to ble_mgmt.c

Core 1:  uart_tx_task      Drains s_uart_tx_q → uart_write_bytes() on UART1
                            (blocking writes are safe here — not the host task)
                            Suspended implicitly while dfu_is_active()
Core 1:  uart_rx_task      Reads UART1 ring buffer → nus_notify()
                            While DFU active: drains + discards (bytes_dropped_dfu)
Core 1:  dfu_task          Drains s_chunk_q → esp_ota_write() off the host task
                            Updates rolling SHA-256, emits CHUNK_ACK notifies
Core 1:  dfu_health_task   One-shot: marks image valid after 30 s if PENDING_VERIFY
Core 1:  led_task          LED status: blink=advertising, steady=connected,
                            fast pulse = DFU in progress
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

If the queue fills (device held off by CTS long enough to exhaust all 32 slots),
`on_ble_write()` returns 1. `nus_rx_chr_cb` maps this to `BLE_ATT_ERR_INSUFFICIENT_RES`
and returns it to NimBLE, which sends an ATT Error Response. Clients using
`writeValueWithResponse` (ATT Write Request) receive the rejection and can retry
after a brief delay; clients using `writeValueWithoutResponse` (Write Command) cannot
be notified and their data is counted in `bytes_dropped_tx`.

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

The device name goes in the **scan response** because the ADV PDU is full
(3 bytes flags + 18 bytes UUID128 = 21 bytes, leaving only 10 bytes free).
The name is built at boot from the BLE MAC address: `"BP+ Bridge XXXX"` where
`XXXX` is the last 4 hex digits of the MAC (e.g. `"BP+ Bridge 96EE"`), making
each bridge uniquely identifiable in scan results.

### BLE chunk size

`BLE_CHUNK = 128` bytes per notification. The negotiated MTU is up to 509 bytes
(512 − 3), but 128 bytes keeps per-notification latency low and avoids congestion
when the client is slow to consume. Increase if throughput must be maximised.

### Hex dump and task watchdog (WDT) interaction

**Do not run hex dump during sustained high-speed stress tests.** When hex dump
is on, `on_ble_write()` (NimBLE host task, core 0) calls `ESP_LOGI` for every
incoming BLE write. At full throughput this floods the UART0 console faster than
460800 baud can drain it (128-byte BLE write → ~480 chars output → ~10 ms at
460800; BLE writes can arrive faster than this under Chrome's multi-PDU pipelining).

`uart_tx_char()` — the low-level console write primitive — **busy-waits** on the
hardware TX FIFO when it is full. It does not yield to the scheduler. If
`uart_rx_task` (core 1) calls any `ESP_LOG*` while the FIFO is saturated, it
sticks in that busy-wait, never reaching `vTaskDelay()`, starving IDLE1, and
triggering the task watchdog.

**Fix applied:** all `ESP_LOG*` calls were removed from the `NUS_ERR_NOMEM`
retry hot-loop in `uart_rx_task`. The loop only calls `vTaskDelay(2ms)`, which
always yields. Counters (`nomem_retries_total`, `nomem_last_ms`) accumulate
silently and are reported by the `s` status command. A single recovery log is
emitted when `nus_notify()` first succeeds again — at that point the console has
had time to drain during the preceding `vTaskDelay` calls.

### ESP32 console baud rate — IDF v6.0 quirk

`CONFIG_ESP_CONSOLE_UART_BAUDRATE` is only user-configurable in IDF v6.0 when
`CONFIG_ESP_CONSOLE_UART_CUSTOM=y`. When `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`
(the usual setting), the baud rate config has no prompt and is locked to 115200;
`sdkconfig.defaults` overrides are silently ignored.

`sdkconfig.defaults.esp32` therefore uses `CONFIG_ESP_CONSOLE_UART_CUSTOM=y` with
`CONFIG_ESP_CONSOLE_UART_NUM=0` (keeps console on UART0) and
`CONFIG_ESP_CONSOLE_UART_BAUDRATE=460800`.

460800 was chosen over 921600: the CH340G baud-rate divisor at 921600 has ~3%
error (at the UART tolerance limit), causing silent fallback to 115200 on some
boards. 460800 has <1% error and is reliable on all CH340 variants.

After changing this setting, delete both `sdkconfig` **and** `build/` before
rebuilding — `idf.py fullclean` alone is sometimes insufficient because
`build/project_description.json` (which `idf_monitor` reads for the baud rate)
is not always regenerated by an incremental build.

### NimBLE mbuf pool

`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=24` (up from the default 12).
Both RX writes (from client → ESP32) and TX notifications (ESP32 → client)
consume mbufs simultaneously during bidirectional stress tests. 24 blocks provides
headroom for sustained full-speed traffic in both directions.

### BP+ Mgmt GATT service

Service UUID `7E500001-B5A3-F393-E0A9-E50E24DCCA9E`. Discovered after connect;
NUS UUID stays in the ADV PDU, BP+ Mgmt goes in the scan response / is listed
in Web Bluetooth `optionalServices`.

| Char | UUID suffix | Properties | Purpose |
|------|-------------|------------|---------|
| AUTH | `…02` | Write + Notify | HMAC-SHA256 challenge/response |
| INFO | `…03` | Read | 88-byte packed struct (see `ble_mgmt.c info_v1_t`) |
| CFG_CONTROL | `…04` | Write + Notify | TLV GET / SET / COMMIT / RESET / ENUM |
| DFU_CONTROL | `…05` | Write + Notify | START / VERIFY / APPLY / ABORT / STATUS |
| DFU_DATA    | `…06` | Write NR | `u16 seq + payload` |

Wire-protocol details, opcode values, and status-byte meanings are documented
in the header comments of `main/dfu.h`, `main/auth.h`, and at the top of the
CFG section in `main/ble_mgmt.c`. The Web Bluetooth client mirrors the
constants verbatim at the top of `host/bpconnect/app.js`.

### Signing workflow and key custody

- ECDSA **P-256** (curve `SECP256R1`). Signature is 64 bytes raw `r || s`
  big-endian, exactly what `tools/sign_fw.py` writes next to the `.bin`.
- The device verifies using `main/verify_ecdsa.c`, which hand-implements
  FIPS 186-4 §6.4 on top of `mbedtls_ecp_muladd` + `mbedtls_mpi_*`. This
  bypasses the `mbedtls/ecdsa.h` header that was moved to private in
  mbedTLS 4.x (IDF v6.0).
- `MBEDTLS_ALLOW_PRIVATE_ACCESS` is defined at the top of `verify_ecdsa.c`
  to restore the pre-4.x `.X/.Y/.Z/.N/.G` struct-member names the code uses.
- `main/signing_pubkey.h` is **committed** (placeholder until provisioned,
  then overwritten by `tools/embed_pubkey.py`). The matching private key
  must NEVER be committed — see `.gitignore` (`keys/*private*.pem`).

### Security model

Current layers (v1 fleet-hardening targets deferred — see `keys/README.md`):

| Layer | Mechanism | Stops |
|-------|-----------|-------|
| L1 | BLE bonding (not yet wired — NimBLE cfg pending) | opportunistic eavesdropping |
| L2 | HMAC-SHA256 session auth, 3-fail lockout, 30 s cooldown | replay / guessing / bonded-but-revoked |
| L3 | SHA-256 over streamed image, verified at VERIFY | corrupted transfers |
| L4 | ECDSA P-256 signature over the image hash | malicious authenticated client |
| L5 | ESP-IDF Secure Boot v2 + Flash Encryption **(deferred — eFuse is one-way)** | JTAG extraction of master key |
| L6 | Anti-rollback (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`) **(off — downgrades permitted)** | forced downgrade to vulnerable image |

The master key + signing pubkey live in committed-or-committable source.
Without L5, a physical attacker with JTAG/flash-dump access to one unit can
extract the master key and impersonate the fleet to initiate authenticated
DFU — but they still cannot get past L4 without the ECDSA private key,
which lives only on the build host.

### NUS suspension during DFU

On receipt of DFU START (or while state ≠ IDLE for any reason), `dfu_is_active()`
returns true. Both the NUS RX callback (`on_ble_write` in main.c) and the
UART→BLE path (`uart_rx_task`) consult this flag:

- RX callback returns 1 → NimBLE sends `BLE_ATT_ERR_INSUFFICIENT_RES` so BLE
  writers see a clear rejection rather than silent reordering against DFU
  chunk traffic.
- `uart_rx_task` drains the UART ring buffer into a scratch buffer and
  discards, counting `bytes_dropped_dfu`. This prevents RTS from deasserting
  and stalling the peer MCU while the user waits out the update.

The `s` status command reports `DFU state: ACTIVE/idle` and `DFU drop: N bytes`.

### DFU chunk size and MTU

`DFU_MAX_CHUNK_PAYLOAD = 510` so the full `seq+payload` frame is 512 bytes,
the per-write ceiling enforced by Chrome's Web Bluetooth. `do_start` further
caps to `negotiated_mtu − 5` (ATT header 3 + seq header 2) because Chrome on
Windows rejects `writeValueWithoutResponse` frames larger than the negotiated
MTU − 3 with a generic "GATT operation failed" error. The device reports the
final chunk size back to the client in `START_RSP.max_chunk_size`; the client
must honour that value.

### OTA partition layout and rollback

Both targets use two equal-sized app slots covering nearly all remaining
flash (after 0x11000 header/boot/nvs/phy_init overhead):

- ESP32 (`partitions_esp32.csv`): 2 × 3.87 MB
- ESP32-S3 (`partitions_esp32s3.csv`): 2 × 7.87 MB

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a freshly-flashed image is in
`ESP_OTA_IMG_PENDING_VERIFY` on first boot. `dfu_start_health_monitor()`
spawns a one-shot task that waits `DFU_HEALTH_WAIT_MS` (30 s) and then calls
`esp_ota_mark_app_valid_cancel_rollback()`. If the image crashes before
that window elapses, the bootloader reverts to the previous slot on the
next reset.

Anti-rollback is **deliberately disabled** — the design admits downgrades
(per Q3 at plan time). If you want to switch to monotonic version
enforcement, set `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` and bump
`CONFIG_BOOTLOADER_APP_SEC_VER` for every release.

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
| Runtime config | Hardcoded | NVS-backed TLV store, live + reboot apply |
| OTA updates | None | BLE DFU with ECDSA P-256 signed images + rollback |
| Authentication | None | HMAC-SHA256 challenge/response, per-device derived secret |
