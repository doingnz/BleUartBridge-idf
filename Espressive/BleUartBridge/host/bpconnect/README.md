# host/bpconnect/ — BP+ Bridge management UI

Web Bluetooth page for authenticating to a **BP+ Bridge** device, editing its
runtime config (baud, flow control, advertising interval, etc.), and pushing
signed OTA firmware updates.

Self-contained: no build step, no dependencies. The same code can later be
ported into `D:\BPplus\JavaScript\bpconnect` as a Framework7 panel once the
flow is settled.

## Requirements

- Chrome or Edge on Windows/macOS/Linux/Android
- Not supported: Safari (iOS + macOS — no Web Bluetooth)
- The page must be served from `localhost`, `https://`, or `file://` with the
  appropriate flag enabled. Plain `http://` is blocked by Web Bluetooth.

## Running locally

```
cd D:\BPplus\Espressive\BleUartBridge\host\bpconnect
python -m http.server 8080
```

Open `http://localhost:8080` in Chrome and click **Connect**. Filter is
`namePrefix: 'BP+ Bridge'`.

## Flow

1. **Connect** — pick a `BP+ Bridge XXXX` device in the browser picker.
2. **Authenticate** — paste the 32-byte `MASTER_KEY` (matches `main/secrets.h`
   on the device) as **64 hex chars, no spaces**. The page derives
   `HMAC-SHA256(master_key, mac)` and runs the challenge/response.

   To turn the C byte array in `main/secrets.h` into the 64-char hex string
   the UI expects, run this in PowerShell from the repo root:

   ```powershell
   (Get-Content main/secrets.h -Raw) -match '(?s)\{([^}]*)\}' | Out-Null
   ($matches[1] -replace '\s|,|0x','').ToLower()
   ```

   It strips the braces, whitespace, commas and `0x` prefixes and lower-cases
   the result — e.g. a 32-byte key of all `0x01` becomes
   `0101010101010101010101010101010101010101010101010101010101010101`.
   Paste that string into the Master key field.
3. **Config** — Enumerate, edit, Commit.
4. **Update firmware** — pick `build/BleUartBridge.bin` and its
   `.sig` sidecar produced by `python tools/sign_fw.py`. The page computes
   the image's SHA-256 locally, sends `START`, streams chunks with
   window-based flow control, sends `VERIFY`, then `APPLY`.

## Security notes

- The master key is stored in browser `localStorage` by default (convenience).
  Click **Forget key** to clear it. Treat the browser as semi-trusted —
  don't paste the master key on a shared workstation.
- The private firmware-signing key is **never** used by this page. Only the
  signature produced offline by `tools/sign_fw.py` is sent.
