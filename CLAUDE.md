# BleUartBridge — Monorepo

## Overview

Three-component system for testing and using the BLE-to-UART bridge firmware.

| Project | Path | Language / Platform |
|---------|------|---------------------|
| ESP-IDF firmware | `Espressive/BleUartBridge/` | C, ESP-IDF v6.0, ESP32 / ESP32-S3 |
| MAUI tester | `MAUI/BleUartBridgeTester/` | C#, .NET MAUI 10, Windows / Android |
| Web tester | `JavaScript/BleUartBridgeTester/` | Vanilla JS, Web Bluetooth API |

The firmware additionally ships its own management UI and CLI under
`Espressive/BleUartBridge/host/` — see the *Host tools* section below.

Each subdirectory has its own `CLAUDE.md` with project-specific context.

## Repository layout

```
BleUartBridge/
├── Espressive/BleUartBridge/   ESP-IDF firmware
├── MAUI/BleUartBridgeTester/   .NET MAUI test app
└── JavaScript/BleUartBridgeTester/  Web Bluetooth test page
```

## BLE protocol (shared across all three)

The ESP32 firmware exposes two GATT services: the transparent NUS pipe and a
device-management service ("BP+ Mgmt") for authenticated runtime
configuration and signed OTA firmware updates.

### NUS — transparent UART bridge

| Role | UUID |
|------|------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (client → ESP32) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (ESP32 → client) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Device advertises as `"BP+ Bridge XXXX"` where `XXXX` is the last 4 hex digits of the BLE MAC address. The NUS service UUID is in the ADV PDU for reliable Web Bluetooth discovery on Chrome/Windows.

### BP+ Mgmt — auth + config + DFU

Discovered after connect (not advertised). Web Bluetooth clients must list
this service UUID in `optionalServices` when calling `requestDevice()`.

| Role | UUID | Properties |
|------|------|------------|
| Service     | `7E500001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| AUTH        | `7E500002-…` | Write + Notify — HMAC-SHA256 challenge/response |
| INFO        | `7E500003-…` | Read — 88-byte packed struct |
| CFG_CONTROL | `7E500004-…` | Write + Notify — TLV GET / SET / COMMIT / RESET / ENUM |
| DFU_CONTROL | `7E500005-…` | Write + Notify — START / VERIFY / APPLY / ABORT |
| DFU_DATA    | `7E500006-…` | Write No Response — `u16 seq + payload` |

Authoritative wire-protocol documentation lives in the firmware headers
(`Espressive/BleUartBridge/main/dfu.h`, `auth.h`, and the opcode block at
the top of `ble_mgmt.c`). The Web Bluetooth client mirrors the constants
verbatim at the top of
`Espressive/BleUartBridge/host/bpconnect/app.js`.

### Security model

| Layer | Mechanism | Status |
|-------|-----------|--------|
| L2 | HMAC-SHA256 session auth, 3-fail/30 s lockout | enabled |
| L3 | SHA-256 over the streamed image | enabled |
| L4 | ECDSA P-256 signature over the image hash | enabled |
| L5 | Secure Boot v2 + Flash Encryption | not enabled (eFuse is one-way) |
| L6 | Anti-rollback | not enabled (downgrades permitted) |

Per-device auth secret = `HMAC-SHA256(MASTER_KEY, ble_mac)`. `MASTER_KEY`
lives in `Espressive/BleUartBridge/main/secrets.h` (gitignored) and must
match the one held by every host tool that talks to the fleet.

## Host tools (firmware-adjacent)

Both tools live under `Espressive/BleUartBridge/host/` and speak the BP+
Mgmt protocol directly (independent of the MAUI / JavaScript testers above,
which exercise NUS only):

| Tool | Path | Platform | Use |
|------|------|----------|-----|
| Web Bluetooth page | `host/bpconnect/` | Chrome / Edge | GUI for auth, config edit, signed OTA flash |
| C# CLI | `host/BpDfuCli/` | Windows (.NET 8) | Scripted upgrades, headless CI, Windows IT deployments |

Firmware signing scripts live under
`Espressive/BleUartBridge/tools/` (`keygen.py`, `embed_pubkey.py`,
`sign_fw.py`). The ECDSA keypair is stored under
`Espressive/BleUartBridge/keys/` — `signing_private.pem` is gitignored,
`signing_public.pem` is committed alongside the embedded C header
`main/signing_pubkey.h`.
