# BleUartBridge

A BLE-to-UART bridge system: ESP32/ESP32-S3 firmware, a .NET MAUI test app, and a Web Bluetooth test page.

A BLE client connects via the Nordic UART Service (NUS) and gets a transparent serial pipe to whatever device is wired to the bridge's hardware UART. The same BLE connection also exposes a **BP+ Mgmt** service for authenticated runtime configuration (UART baud, flow control, advertising interval, etc.) and signed over-the-air firmware updates.

```
BLE client  ◄──NUS notify / write──►  ESP32 / ESP32-S3  ◄──UART1 RTS/CTS──►  Device
            ◄──BP+ Mgmt auth / cfg / DFU──►
```

## Projects

| Folder | Description |
|--------|-------------|
| [`Espressive/BleUartBridge`](Espressive/BleUartBridge/README.md) | ESP-IDF firmware for ESP32 and ESP32-S3 (NUS bridge + BP+ Mgmt service) |
| [`Espressive/BleUartBridge/host/bpconnect`](Espressive/BleUartBridge/host/bpconnect/README.md) | Web Bluetooth UI for auth / config / signed OTA |
| [`Espressive/BleUartBridge/host/BpDfuCli`](Espressive/BleUartBridge/host/BpDfuCli/README.md) | .NET 8 command-line alternative (Windows) |
| [`MAUI/BleUartBridgeTester`](MAUI/BleUartBridgeTester/README.md) | .NET MAUI test app exercising the NUS pipe (Windows / Android) |
| [`JavaScript/BleUartBridgeTester`](JavaScript/BleUartBridgeTester/README.md) | Web Bluetooth test page exercising the NUS pipe (Chrome / Edge) |

## Firmware features

- Transparent BLE → UART1 bridge with full hardware RTS/CTS flow control
- BP+ Mgmt GATT service: HMAC-SHA256 client auth + NVS-backed runtime config
- Signed OTA firmware update (ECDSA P-256) with automatic rollback on failure
- Single codebase targets both ESP32 (8 MB flash) and ESP32-S3 (16 MB flash)
- Interactive console on UART0 / USB-JTAG for status, info, hex dump, factory reset

See [`Espressive/BleUartBridge/README.md`](Espressive/BleUartBridge/README.md) for the full firmware reference.

## License

[MIT](LICENSE)
