# BleUartBridge

A BLE-to-UART bridge system: ESP32/ESP32-S3 firmware, a .NET MAUI test app, and a Web Bluetooth test page.

A BLE client connects via the Nordic UART Service (NUS) and gets a transparent serial pipe to whatever device is wired to the bridge's hardware UART.

```
BLE client  ◄──NUS notify / write──►  ESP32 / ESP32-S3  ◄──UART1 RTS/CTS──►  Device
```

## Projects

| Folder | Description |
|--------|-------------|
| [`Espressive/BleUartBridge`](Espressive/BleUartBridge/README.md) | ESP-IDF firmware for ESP32 and ESP32-S3 |
| [`MAUI/BleUartBridgeTester`](MAUI/BleUartBridgeTester/README.md) | .NET MAUI test app (Windows / Android) |
| [`JavaScript/BleUartBridgeTester`](JavaScript/BleUartBridgeTester/README.md) | Web Bluetooth test page (Chrome / Edge) |

## License

[MIT](LICENSE)
