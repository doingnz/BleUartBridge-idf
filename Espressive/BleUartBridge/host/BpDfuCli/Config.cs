using System.Text.Json;
using System.Text.Json.Serialization;

namespace BpDfuCli;

/// Minimal persistent config: just the master key (hex) that the device's
/// secrets.h matches.  Stored in %USERPROFILE%\.bpdfu\config.json so it is
/// per-user and stays off shared filesystems.
internal sealed class AppConfig
{
    [JsonPropertyName("masterKey")]
    public string MasterKeyHex { get; set; } = "";

    private static readonly JsonSerializerOptions s_json = new()
    {
        WriteIndented = true,
    };

    public static string DefaultPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                     ".bpdfu", "config.json");

    public static AppConfig Load(string? path = null)
    {
        path ??= DefaultPath;
        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                $"Config not found at {path}. Create it with the master key, e.g.:\n" +
                "{\n  \"masterKey\": \"0101010101010101010101010101010101010101010101010101010101010101\"\n}");
        }
        var json = File.ReadAllText(path);
        var cfg = JsonSerializer.Deserialize<AppConfig>(json, s_json)
                  ?? throw new InvalidDataException("config.json did not deserialise");
        if (cfg.MasterKeyHex.Length != 64)
            throw new InvalidDataException("masterKey must be 64 hex characters");
        return cfg;
    }

    public byte[] MasterKey => Convert.FromHexString(MasterKeyHex);
}
