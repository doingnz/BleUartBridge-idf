# host/BpDfuCli — command-line fallback

C# .NET 8 console app that talks to the BP+ Mgmt GATT service via the
Windows.Devices.Bluetooth WinRT stack. Windows-only; the Web Bluetooth UI
at `host/bpconnect/` is the cross-platform path.

Use when:
- scripting firmware rollouts from CI or PowerShell
- working on machines without a usable Chrome/Edge install
- debugging the wire protocol from a non-browser vantage point

## Build

Requires [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).

```powershell
cd D:\BPplus\Espressive\BleUartBridge\host\BpDfuCli
dotnet build                       # debug run via `dotnet run -- <args>`
dotnet publish -c Release          # produces a single .exe in bin\Release\net8.0-windows*\win-x64\publish\
```

## Configure

Create `%USERPROFILE%\.bpdfu\config.json` with the same 32-byte master key
that's compiled into `main/secrets.h`:

```json
{
  "masterKey": "0101010101010101010101010101010101010101010101010101010101010101"
}
```

Restrict the file's permissions — it is functionally equivalent to the
fleet's auth credential.

## Subcommands

```
bpdfu list [--name <filter>] [--timeout 5]
bpdfu info  <device>
bpdfu cfg   enum   <device>
bpdfu cfg   get    <device> <id-hex>
bpdfu cfg   set    <device> <id-hex> <value>
bpdfu cfg   commit <device>
bpdfu flash <device> <firmware.bin>        (expects .bin.sig alongside)
```

`<device>` is either:
- a **name substring** (e.g. `96EE`, `BP+ Bridge 96EE`); matched case-insensitively
- a **BD_ADDR** (`AA:BB:CC:DD:EE:FF`); skips the scan

Exit codes: 0 OK, 1 usage, 2 timeout, 3 IO / GATT error, 4 bad arguments.

## Examples

```powershell
# find nearby devices
bpdfu list

# inspect one
bpdfu info 96EE

# enumerate config
bpdfu cfg enum 96EE

# raise UART baud to 230400 and commit (reboots the device on reboot-apply TLVs)
bpdfu cfg set    96EE 01 230400
bpdfu cfg commit 96EE

# push a signed firmware image
python ..\..\tools\sign_fw.py                  # produce build\BleUartBridge.bin.sig
bpdfu flash 96EE ..\..\build\BleUartBridge.bin --version 2
```

## TLV ids (mirror of `main/cfg.h`)

| ID   | Name                 | Type  |
|------|----------------------|-------|
| 0x01 | uart_baud            | u32   |
| 0x02 | uart_flowctrl        | u8    |
| 0x03 | uart_databits        | u8    |
| 0x04 | uart_parity          | u8    |
| 0x05 | uart_stopbits        | u8    |
| 0x06 | name_suffix          | str8  |
| 0x07 | ble_tx_power_dbm     | i8    |
| 0x08 | hexdump_default      | u8    |
| 0x09 | dfu_enabled          | u8    |
| 0x0A | auth_required        | u8    |
| 0x0B | factory_reset        | trig  |
| 0x0C | ble_adv_interval_ms  | u16   |
| 0x0D | ble_preferred_mtu    | u16   |
