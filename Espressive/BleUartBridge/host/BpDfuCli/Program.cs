// bpdfu — command-line fallback to the host/bpconnect Web Bluetooth UI.
//
// Subcommands:
//   bpdfu list [--name <filter>] [--timeout <sec>]
//   bpdfu info <device-name-or-addr>
//   bpdfu cfg  enum   <device>
//   bpdfu cfg  get    <device> <id-hex>
//   bpdfu cfg  set    <device> <id-hex> <value>
//   bpdfu cfg  commit <device>
//   bpdfu flash <device> <path-to-firmware.bin>    (expects .bin.sig alongside)
//
// <device> may be either a name substring (matched case-insensitively against
// the advertised name) or a colon-separated BD_ADDR (AA:BB:CC:DD:EE:FF).
//
// Config file: %USERPROFILE%\.bpdfu\config.json with {"masterKey": "<64 hex>"}.

using BpDfuCli;

return await Main(args);

static async Task<int> Main(string[] args)
{
    if (args.Length == 0) { PrintUsage(); return 1; }

    try
    {
        return args[0] switch
        {
            "list"  => await DoList(args),
            "info"  => await DoInfo(args),
            "cfg"   => await DoCfg(args),
            "flash" => await DoFlash(args),
            _       => Usage($"unknown subcommand: {args[0]}"),
        };
    }
    catch (TimeoutException ex) { Console.Error.WriteLine($"timeout: {ex.Message}"); return 2; }
    catch (IOException     ex) { Console.Error.WriteLine($"error: {ex.Message}");    return 3; }
    catch (ArgumentException ex) { Console.Error.WriteLine($"arg: {ex.Message}");     return 4; }
}

static int Usage(string msg) { Console.Error.WriteLine(msg); PrintUsage(); return 1; }

static void PrintUsage() => Console.Error.WriteLine(
@"bpdfu — command-line manager for BP+ Bridge devices

Usage:
  bpdfu list [--name <filter>] [--timeout 5]
  bpdfu info  <device>
  bpdfu cfg   enum   <device>
  bpdfu cfg   get    <device> <id-hex>
  bpdfu cfg   set    <device> <id-hex> <value>
  bpdfu cfg   commit <device>
  bpdfu flash <device> <firmware.bin>        (expects firmware.bin.sig alongside)

<device>: either a name substring (e.g. ""96EE"") or BD_ADDR (AA:BB:CC:DD:EE:FF)
Config:   %USERPROFILE%\.bpdfu\config.json with { ""masterKey"": ""<64 hex>"" }");

// ── list ────────────────────────────────────────────────────────────────────

static async Task<int> DoList(string[] args)
{
    string? filter = null;
    double  timeoutSec = 5;
    for (int i = 1; i < args.Length; i++)
    {
        if (args[i] == "--name"    && i+1 < args.Length) filter     = args[++i];
        else if (args[i] == "--timeout" && i+1 < args.Length) timeoutSec = double.Parse(args[++i]);
        else return Usage($"unexpected arg: {args[i]}");
    }

    Console.Error.WriteLine($"scanning for {timeoutSec} s...");
    var hits = await BleMgmtClient.ScanAsync(TimeSpan.FromSeconds(timeoutSec), filter);
    if (hits.Count == 0) { Console.Error.WriteLine("no devices found"); return 2; }
    foreach (var h in hits)
        Console.WriteLine($"{FormatAddr(h.Address)}  {h.Name}");
    return 0;
}

// ── info ────────────────────────────────────────────────────────────────────

static async Task<int> DoInfo(string[] args)
{
    if (args.Length < 2) return Usage("info needs a <device>");
    await using var c = await Resolve(args[1]);

    var i = c.Info;
    Console.WriteLine($"  MAC        : {i.MacDisplay}");
    Console.WriteLine($"  target     : {(i.Target == 1 ? "esp32s3" : "esp32")}");
    Console.WriteLine($"  firmware   : {i.Firmware}");
    Console.WriteLine($"  ELF SHA8   : {Convert.ToHexString(i.ElfSha8).ToLowerInvariant()}");
    Console.WriteLine($"  running    : 0x{i.RunningAddr:X6} / {i.RunningSize/1024} KB");
    Console.WriteLine($"  next OTA   : 0x{i.NextAddr:X6} / {i.NextSize/1024} KB");
    Console.WriteLine($"  pref MTU   : {i.PreferredMtu}");
    Console.WriteLine($"  auth req   : {i.AuthRequired}");
    Console.WriteLine($"  dfu enabled: {i.DfuEnabled}");
    Console.WriteLine($"  pubkey fp  : {Convert.ToHexString(i.PubkeyFp).ToLowerInvariant()}");
    return 0;
}

// ── cfg ─────────────────────────────────────────────────────────────────────

static async Task<int> DoCfg(string[] args)
{
    if (args.Length < 2) return Usage("cfg needs a subcommand: enum|get|set|commit");
    var sub = args[1];
    if (args.Length < 3) return Usage($"cfg {sub} needs a <device>");

    await using var c = await Resolve(args[2]);
    await c.AuthenticateAsync(AppConfig.Load().MasterKey);

    switch (sub)
    {
        case "enum":
        {
            var list = await c.CfgEnumAsync();
            Console.WriteLine("  ID   type  live  value (hex / decoded)");
            foreach (var e in list)
            {
                var hex = Convert.ToHexString(e.Value).ToLowerInvariant();
                var dec = DecodeTlv(e.Id, e.Type, e.Value);
                Console.WriteLine($"  0x{e.Id:X2} {e.Type,4}  {(e.Live?"yes":" no")}  {hex,-16}  {dec}");
            }
            return 0;
        }
        case "get":
        {
            if (args.Length < 4) return Usage("cfg get needs <id-hex>");
            byte id = Convert.ToByte(args[3], 16);
            var val = await c.CfgGetAsync(id);
            Console.WriteLine(Convert.ToHexString(val).ToLowerInvariant());
            return 0;
        }
        case "set":
        {
            if (args.Length < 5) return Usage("cfg set needs <id-hex> <value>");
            byte id = Convert.ToByte(args[3], 16);
            var payload = EncodeTlv(id, args[4]);
            await c.CfgSetAsync(id, payload);
            Console.WriteLine($"set 0x{id:X2} = {args[4]}  (staged; run 'cfg commit' to persist)");
            return 0;
        }
        case "commit":
        {
            await c.CfgCommitAsync();
            Console.WriteLine("committed");
            return 0;
        }
        default:
            return Usage($"cfg: unknown subcommand '{sub}'");
    }
}

// ── flash ───────────────────────────────────────────────────────────────────

static async Task<int> DoFlash(string[] args)
{
    if (args.Length < 3) return Usage("flash needs <device> <firmware.bin>");
    var binPath = args[2];
    var sigPath = binPath + ".sig";
    if (!File.Exists(binPath)) { Console.Error.WriteLine($"missing {binPath}"); return 4; }
    if (!File.Exists(sigPath)) { Console.Error.WriteLine($"missing {sigPath} — run tools/sign_fw.py"); return 4; }

    uint version = 1;
    for (int i = 3; i < args.Length; i++)
        if (args[i] == "--version" && i+1 < args.Length) version = uint.Parse(args[++i]);

    var image = await File.ReadAllBytesAsync(binPath);
    var sig   = await File.ReadAllBytesAsync(sigPath);

    await using var c = await Resolve(args[1]);
    await c.AuthenticateAsync(AppConfig.Load().MasterKey);

    var progress = new Progress<(int sent, int total)>(p =>
        Console.Error.Write($"\r{p.sent,10:N0} / {p.total:N0} B  ({100.0*p.sent/p.total,5:0.0}%)   "));

    await c.FlashAsync(image, sig, version, progress);
    Console.Error.WriteLine();
    Console.WriteLine("APPLY ok — device rebooting with the new image");
    return 0;
}

// ── helpers ─────────────────────────────────────────────────────────────────

static async Task<BleMgmtClient> Resolve(string target)
{
    // BD_ADDR form?  (AA:BB:CC:DD:EE:FF)
    if (TryParseAddr(target, out var addr))
        return await BleMgmtClient.ConnectAsync(addr);

    // Otherwise scan and match by name.
    var hits = await BleMgmtClient.ScanAsync(TimeSpan.FromSeconds(5), target);
    if (hits.Count == 0) throw new IOException($"no device matched '{target}'");
    if (hits.Count > 1)
    {
        var names = string.Join(", ", hits.Select(h => h.Name));
        throw new IOException($"'{target}' matched {hits.Count} devices: {names}. Be more specific or use BD_ADDR.");
    }
    return await BleMgmtClient.ConnectAsync(hits[0].Address);
}

static bool TryParseAddr(string s, out ulong addr)
{
    addr = 0;
    var parts = s.Split(':');
    if (parts.Length != 6) return false;
    ulong v = 0;
    foreach (var p in parts)
    {
        if (!byte.TryParse(p, System.Globalization.NumberStyles.HexNumber, null, out var b))
            return false;
        v = (v << 8) | b;
    }
    addr = v;
    return true;
}

static string FormatAddr(ulong a) =>
    a == 0 ? "" :
    string.Join(':', Enumerable.Range(0, 6).Reverse().Select(i => $"{(byte)(a >> (i*8)):X2}"));

// ── TLV codec (must match cfg.c / cfg.h TLV IDs and types) ─────────────────

static byte[] EncodeTlv(byte id, string value)
{
    // Heuristic decode based on well-known TLV ids.
    return id switch
    {
        0x01 => U32Le(uint.Parse(value)),                                    // uart_baud
        0x02 or 0x03 or 0x04 or 0x05 or 0x08 or 0x09 or 0x0A or 0x0B
             => new[] { (byte)int.Parse(value) },                            // u8 / bool / trigger
        0x06 => System.Text.Encoding.UTF8.GetBytes(value),                   // name_suffix
        0x07 => new[] { unchecked((byte)sbyte.Parse(value)) },               // i8
        0x0C => U16Le(ushort.Parse(value)),                                  // adv_interval_ms
        0x0D => U16Le(ushort.Parse(value)),                                  // preferred_mtu
        _    => throw new ArgumentException($"unknown TLV id 0x{id:X2}"),
    };
    static byte[] U32Le(uint v) => new[] { (byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24) };
    static byte[] U16Le(ushort v) => new[] { (byte)v, (byte)(v>>8) };
}

static string DecodeTlv(byte id, byte type, byte[] v) => type switch
{
    0 => $"{v[0]}",                                                           // u8
    1 => $"{(sbyte)v[0]}",                                                    // i8
    2 => $"{(ushort)(v[0] | (v[1]<<8))}",                                     // u16
    3 => $"{(uint)(v[0] | (v[1]<<8) | (v[2]<<16) | (v[3]<<24))}",             // u32
    4 => $"\"{System.Text.Encoding.UTF8.GetString(v)}\"",                     // str
    _ => "(trigger)",
};
