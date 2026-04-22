using System.Runtime.InteropServices.WindowsRuntime;
using System.Security.Cryptography;
using System.Text;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

namespace BpDfuCli;

/// <summary>
/// Mirrors main/ble_mgmt.c / main/dfu.c / main/auth.c on the device side.
/// Scans for a BP+ Bridge, connects, authenticates, and performs CFG and
/// DFU operations.  WinRT-only (Windows 10.0.19041+).
/// </summary>
internal sealed class BleMgmtClient : IAsyncDisposable
{
    // ── UUIDs ───────────────────────────────────────────────────────────────
    public static readonly Guid NUS_SERVICE  = new("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid MGMT_SERVICE = new("7e500001-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid AUTH_UUID    = new("7e500002-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid INFO_UUID    = new("7e500003-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid CFG_UUID     = new("7e500004-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid DFU_CTL_UUID = new("7e500005-b5a3-f393-e0a9-e50e24dcca9e");
    public static readonly Guid DFU_DAT_UUID = new("7e500006-b5a3-f393-e0a9-e50e24dcca9e");

    // ── Opcodes (client → server) ──────────────────────────────────────────
    const byte CFG_GET = 0x01, CFG_SET = 0x02, CFG_COMMIT = 0x03, CFG_RESET = 0x04, CFG_ENUM = 0x05;
    const byte CFG_RSP_GET = 0x81, CFG_RSP_SET = 0x82, CFG_RSP_COMMIT = 0x83;
    const byte CFG_RSP_ENUM = 0x85, CFG_RSP_ENUM_END = 0x86;

    const byte DFU_START = 0x01, DFU_VERIFY = 0x03, DFU_APPLY = 0x04, DFU_ABORT = 0x05;
    const byte DFU_RSP_START = 0x81, DFU_RSP_CHUNK = 0x82, DFU_RSP_VERIFY = 0x83;
    const byte DFU_RSP_APPLY = 0x84, DFU_EVT_ABORT = 0x85;

    BluetoothLEDevice? _device;
    GattDeviceService? _mgmt;
    GattCharacteristic? _auth, _info, _cfg, _dfuCtl, _dfuDat;

    // Notification dispatch: every notify we receive is shoved into the
    // matching queue so callers can "await next notification on this char".
    readonly System.Collections.Concurrent.BlockingCollection<byte[]> _authNotifs = new();
    readonly System.Collections.Concurrent.BlockingCollection<byte[]> _cfgNotifs  = new();
    readonly System.Collections.Concurrent.BlockingCollection<byte[]> _dfuNotifs  = new();

    public InfoSnapshot Info { get; private set; } = new();

    // ── Scanning ────────────────────────────────────────────────────────────
    public sealed record ScanHit(string Name, ulong Address);

    public static async Task<List<ScanHit>> ScanAsync(TimeSpan duration, string? nameFilter)
    {
        var hits = new Dictionary<ulong, ScanHit>();
        var watcher = new BluetoothLEAdvertisementWatcher
        {
            ScanningMode = BluetoothLEScanningMode.Active,
        };
        watcher.Received += (_, e) =>
        {
            var name = string.IsNullOrEmpty(e.Advertisement.LocalName)
                ? $"[{e.BluetoothAddress:X12}]"
                : e.Advertisement.LocalName;
            if (nameFilter != null && !name.Contains(nameFilter, StringComparison.OrdinalIgnoreCase))
                return;
            if (!name.StartsWith("BP+ Bridge") && nameFilter == null) return;
            hits[e.BluetoothAddress] = new ScanHit(name, e.BluetoothAddress);
        };
        watcher.Start();
        await Task.Delay(duration);
        watcher.Stop();
        return hits.Values.OrderBy(h => h.Name).ToList();
    }

    public static async Task<BleMgmtClient> ConnectAsync(ulong address)
    {
        var dev = await BluetoothLEDevice.FromBluetoothAddressAsync(address)
                  ?? throw new IOException($"FromBluetoothAddressAsync returned null for {address:X12}");

        var svcRes = await dev.GetGattServicesForUuidAsync(MGMT_SERVICE, BluetoothCacheMode.Uncached);
        if (svcRes.Status != GattCommunicationStatus.Success || svcRes.Services.Count == 0)
            throw new IOException($"BP+ Mgmt service not found ({svcRes.Status})");

        var client = new BleMgmtClient { _device = dev, _mgmt = svcRes.Services[0] };

        client._auth   = await client.GetCharAsync(AUTH_UUID);
        client._info   = await client.GetCharAsync(INFO_UUID);
        client._cfg    = await client.GetCharAsync(CFG_UUID);
        client._dfuCtl = await client.GetCharAsync(DFU_CTL_UUID);
        client._dfuDat = await client.GetCharAsync(DFU_DAT_UUID);

        await client.SubscribeAsync(client._auth!,   client._authNotifs);
        await client.SubscribeAsync(client._cfg!,    client._cfgNotifs);
        await client.SubscribeAsync(client._dfuCtl!, client._dfuNotifs);

        await client.RefreshInfoAsync();
        return client;
    }

    async Task<GattCharacteristic> GetCharAsync(Guid uuid)
    {
        var cr = await _mgmt!.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode.Uncached);
        if (cr.Status != GattCommunicationStatus.Success || cr.Characteristics.Count == 0)
            throw new IOException($"characteristic {uuid} not found ({cr.Status})");
        return cr.Characteristics[0];
    }

    async Task SubscribeAsync(GattCharacteristic c,
                              System.Collections.Concurrent.BlockingCollection<byte[]> q)
    {
        var st = await c.WriteClientCharacteristicConfigurationDescriptorAsync(
                     GattClientCharacteristicConfigurationDescriptorValue.Notify);
        if (st != GattCommunicationStatus.Success)
            throw new IOException($"enable notifications on {c.Uuid}: {st}");
        c.ValueChanged += (_, e) =>
        {
            CryptographicBuffer.CopyToByteArray(e.CharacteristicValue, out var bytes);
            q.Add(bytes);
        };
    }

    // ── INFO ────────────────────────────────────────────────────────────────

    public async Task RefreshInfoAsync()
    {
        var r = await _info!.ReadValueAsync(BluetoothCacheMode.Uncached);
        if (r.Status != GattCommunicationStatus.Success)
            throw new IOException($"INFO read: {r.Status}");

        CryptographicBuffer.CopyToByteArray(r.Value, out var bytes);
        if (bytes.Length < 88) throw new IOException($"INFO too short: {bytes.Length}");
        Info = InfoSnapshot.Parse(bytes);
    }

    // ── Auth ────────────────────────────────────────────────────────────────

    public async Task AuthenticateAsync(byte[] masterKey)
    {
        // device_secret = HMAC-SHA256(master_key, mac[6])  — mac in radio order
        var deviceSecret = HMACSHA256.HashData(masterKey, Info.Mac);

        // BEGIN
        await WriteAsync(_auth!, new byte[] { 0x01 });
        var chalFrame = Take(_authNotifs, TimeSpan.FromSeconds(5));
        if (chalFrame.Length != 1 + 16 || chalFrame[0] != 0x81)
            throw new IOException("expected AUTH CHALLENGE");
        var challenge = chalFrame.AsSpan(1, 16).ToArray();

        // RESPONSE = HMAC-SHA256(device_secret, challenge)
        var response = HMACSHA256.HashData(deviceSecret, challenge);
        var frame = new byte[1 + 32];
        frame[0] = 0x02;
        Array.Copy(response, 0, frame, 1, 32);
        await WriteAsync(_auth!, frame);

        var stat = Take(_authNotifs, TimeSpan.FromSeconds(5));
        if (stat.Length != 2 || stat[0] != 0x82)
            throw new IOException("expected AUTH STATUS");
        if (stat[1] == 2) throw new IOException("auth locked out (3 failures, 30 s cooldown)");
        if (stat[1] != 0) throw new IOException($"auth failed: status {stat[1]}");
    }

    // ── CFG ─────────────────────────────────────────────────────────────────

    public sealed record CfgEntry(byte Id, byte Type, bool Live, byte[] Value);

    public async Task<List<CfgEntry>> CfgEnumAsync()
    {
        // Drain any stale notifications before issuing new request
        while (_cfgNotifs.TryTake(out _)) { }
        await WriteAsync(_cfg!, new byte[] { CFG_ENUM });

        var list = new List<CfgEntry>();
        while (true)
        {
            var f = Take(_cfgNotifs, TimeSpan.FromSeconds(5));
            if (f.Length == 0) continue;
            if (f[0] == CFG_RSP_ENUM)
            {
                if (f.Length < 5) throw new IOException("CFG_RSP_ENUM too short");
                var id   = f[1]; var type = f[2]; var live = f[3] != 0; var len = f[4];
                if (f.Length < 5 + len) throw new IOException("CFG_RSP_ENUM truncated");
                list.Add(new CfgEntry(id, type, live, f.AsSpan(5, len).ToArray()));
            }
            else if (f[0] == CFG_RSP_ENUM_END)
            {
                return list;
            }
            else throw new IOException($"unexpected CFG opcode 0x{f[0]:X2} during enum");
        }
    }

    public async Task<byte[]> CfgGetAsync(byte id)
    {
        while (_cfgNotifs.TryTake(out _)) { }
        await WriteAsync(_cfg!, new byte[] { CFG_GET, id });
        var f = Take(_cfgNotifs, TimeSpan.FromSeconds(5));
        if (f.Length < 4 || f[0] != CFG_RSP_GET) throw new IOException("bad CFG GET response");
        if (f[2] != 0) throw new IOException($"CFG GET status {f[2]}");
        return f.AsSpan(4, f[3]).ToArray();
    }

    public async Task CfgSetAsync(byte id, byte[] value)
    {
        while (_cfgNotifs.TryTake(out _)) { }
        var frame = new byte[3 + value.Length];
        frame[0] = CFG_SET; frame[1] = id; frame[2] = (byte)value.Length;
        Array.Copy(value, 0, frame, 3, value.Length);
        await WriteAsync(_cfg!, frame);
        var f = Take(_cfgNotifs, TimeSpan.FromSeconds(5));
        if (f.Length < 3 || f[0] != CFG_RSP_SET) throw new IOException("bad CFG SET response");
        if (f[2] != 0) throw new IOException($"CFG SET 0x{id:X2} status {f[2]}");
    }

    public async Task CfgCommitAsync()
    {
        while (_cfgNotifs.TryTake(out _)) { }
        await WriteAsync(_cfg!, new byte[] { CFG_COMMIT });
        var f = Take(_cfgNotifs, TimeSpan.FromSeconds(10));
        if (f.Length < 2 || f[0] != CFG_RSP_COMMIT) throw new IOException("bad CFG COMMIT response");
        if (f[1] != 0) throw new IOException($"CFG COMMIT status {f[1]}");
    }

    // ── DFU ─────────────────────────────────────────────────────────────────

    public async Task FlashAsync(byte[] image, byte[] sig, uint version, IProgress<(int sent, int total)>? progress)
    {
        if (sig.Length != 64) throw new ArgumentException("signature must be 64 bytes raw r||s");

        var sha = SHA256.HashData(image);

        while (_dfuNotifs.TryTake(out _)) { }

        // START = opcode + size_u32 + sha[32] + version_u32 + sig[64]
        var start = new byte[1 + 4 + 32 + 4 + 64];
        start[0] = DFU_START;
        WriteU32Le(start, 1, (uint)image.Length);
        Array.Copy(sha, 0, start, 5, 32);
        WriteU32Le(start, 37, version);
        Array.Copy(sig, 0, start, 41, 64);
        await WriteAsync(_dfuCtl!, start);

        var sr = Take(_dfuNotifs, TimeSpan.FromSeconds(10));
        if (sr.Length < 1 || sr[0] != DFU_RSP_START) throw new IOException("bad DFU START response");
        if (sr.Length < 2 || sr[1] != 0) throw new IOException($"DFU START status {sr[1]} — see dfu.h");
        if (sr.Length < 6) throw new IOException("DFU START response truncated");
        int maxChunk = sr[2] | (sr[3] << 8);
        int window   = sr[4] | (sr[5] << 8);

        Console.WriteLine($"DFU: maxChunk={maxChunk}, window={window}, bytes={image.Length}");

        // Shared across the sender (main flow) and drain task — use Interlocked
        // to keep the counts coherent without a lock.
        int sent = 0, seq = 0;
        int outstanding = 0;
        int received = 0;
        var lastAckMs = Environment.TickCount64;
        int abortReason = -1;

        using var cts = new CancellationTokenSource();
        var drain = Task.Run(async () =>
        {
            try
            {
                while (!cts.IsCancellationRequested)
                {
                    if (!_dfuNotifs.TryTake(out var f, 200, cts.Token)) continue;
                    if (f[0] == DFU_RSP_CHUNK)
                    {
                        Interlocked.Decrement(ref outstanding);
                        if (f.Length >= 7)
                            Volatile.Write(ref received, (int)ReadU32Le(f, 3));
                        Interlocked.Exchange(ref lastAckMs, Environment.TickCount64);
                        progress?.Report((Volatile.Read(ref received), image.Length));
                    }
                    else if (f[0] == DFU_EVT_ABORT)
                    {
                        Volatile.Write(ref abortReason, f.Length >= 2 ? f[1] : 0);
                        return;
                    }
                    else
                    {
                        // VERIFY/APPLY response arrived early — hand it back.
                        _dfuNotifs.Add(f, cts.Token);
                        await Task.Delay(5, cts.Token);
                    }
                }
            }
            catch (OperationCanceledException) { /* drain shutting down */ }
        });

        while (sent < image.Length && Volatile.Read(ref abortReason) < 0)
        {
            while (Volatile.Read(ref outstanding) >= window &&
                   Volatile.Read(ref abortReason) < 0)
                await Task.Delay(5);
            int len = Math.Min(maxChunk, image.Length - sent);
            var frame = new byte[2 + len];
            frame[0] = (byte)(seq & 0xFF);
            frame[1] = (byte)((seq >> 8) & 0xFF);
            Array.Copy(image, sent, frame, 2, len);
            Interlocked.Increment(ref outstanding);
            await WriteAsync(_dfuDat!, frame, GattWriteOption.WriteWithoutResponse);
            sent += len;
            seq++;
        }

        while (Volatile.Read(ref outstanding) > 0 && Volatile.Read(ref abortReason) < 0)
        {
            if (Environment.TickCount64 - Volatile.Read(ref lastAckMs) > 15000)
                throw new IOException("ACK drain timeout");
            await Task.Delay(20);
        }

        cts.Cancel();
        try { await drain; } catch (OperationCanceledException) { /* normal */ }
        int ar = Volatile.Read(ref abortReason);
        if (ar >= 0) throw new IOException($"DFU aborted by device, reason {ar} — see dfu.h");

        while (_dfuNotifs.TryTake(out _)) { }
        await WriteAsync(_dfuCtl!, new byte[] { DFU_VERIFY });
        var vr = Take(_dfuNotifs, TimeSpan.FromSeconds(15));
        if (vr.Length < 2 || vr[0] != DFU_RSP_VERIFY) throw new IOException("bad VERIFY response");
        if (vr[1] != 0) throw new IOException($"VERIFY status {vr[1]} — see dfu.h (hash=8, sig=9)");

        await WriteAsync(_dfuCtl!, new byte[] { DFU_APPLY });
        var ar = Take(_dfuNotifs, TimeSpan.FromSeconds(5));
        if (ar.Length < 2 || ar[0] != DFU_RSP_APPLY) throw new IOException("bad APPLY response");
        if (ar[1] != 0) throw new IOException($"APPLY status {ar[1]} — see dfu.h");
    }

    // ── Low-level plumbing ─────────────────────────────────────────────────

    static async Task WriteAsync(GattCharacteristic c, byte[] data,
                                 GattWriteOption opt = GattWriteOption.WriteWithResponse)
    {
        var buf = CryptographicBuffer.CreateFromByteArray(data);
        var st  = await c.WriteValueAsync(buf, opt);
        if (st != GattCommunicationStatus.Success)
            throw new IOException($"write {c.Uuid}: {st}");
    }

    static byte[] Take(System.Collections.Concurrent.BlockingCollection<byte[]> q, TimeSpan timeout)
    {
        if (q.TryTake(out var v, (int)timeout.TotalMilliseconds)) return v;
        throw new TimeoutException("no notification within timeout");
    }

    static void WriteU32Le(byte[] b, int off, uint v)
    {
        b[off] = (byte)v; b[off+1] = (byte)(v>>8); b[off+2] = (byte)(v>>16); b[off+3] = (byte)(v>>24);
    }
    static uint ReadU32Le(byte[] b, int off)
        => (uint)(b[off] | (b[off+1]<<8) | (b[off+2]<<16) | (b[off+3]<<24));

    public async ValueTask DisposeAsync()
    {
        _mgmt?.Dispose();
        _device?.Dispose();
        await Task.CompletedTask;
    }
}

/// <summary>Parsed INFO characteristic (88-byte packed struct from ble_mgmt.c).</summary>
internal sealed record InfoSnapshot(
    ushort Magic, byte Version, byte Target, string Firmware,
    byte[] ElfSha8, uint RunningAddr, uint RunningSize,
    uint NextAddr, uint NextSize,
    ushort PreferredMtu, bool AuthRequired, bool DfuEnabled,
    byte[] PubkeyFp, byte[] Mac)
{
    public InfoSnapshot() : this(0, 0, 0, "", new byte[8], 0, 0, 0, 0, 0, false, false, new byte[16], new byte[6]) { }

    public static InfoSnapshot Parse(byte[] b)
    {
        if (b.Length < 88) throw new ArgumentException($"INFO must be ≥ 88 bytes, got {b.Length}");
        var magic   = (ushort)(b[0] | (b[1] << 8));
        var version = b[2];
        var target  = b[3];
        int nul = Array.IndexOf(b, (byte)0, 4, 32);
        var fw = Encoding.UTF8.GetString(b, 4, (nul < 0 ? 36 : nul) - 4);
        var sha8 = b.AsSpan(36, 8).ToArray();
        var runAddr = ReadU32(b, 44);
        var runSize = ReadU32(b, 48);
        var nxtAddr = ReadU32(b, 52);
        var nxtSize = ReadU32(b, 56);
        var mtu     = (ushort)(b[60] | (b[61] << 8));
        var authReq = b[62] != 0;
        var dfuEn   = b[63] != 0;
        var fp      = b.AsSpan(64, 16).ToArray();
        var mac     = b.AsSpan(80, 6).ToArray();
        return new InfoSnapshot(magic, version, target, fw, sha8, runAddr, runSize,
                                nxtAddr, nxtSize, mtu, authReq, dfuEn, fp, mac);
    }

    static uint ReadU32(byte[] b, int off)
        => (uint)(b[off] | (b[off+1]<<8) | (b[off+2]<<16) | (b[off+3]<<24));

    public string MacDisplay => string.Join(':',
        Mac.Reverse().Select(x => x.ToString("x2")));
}
