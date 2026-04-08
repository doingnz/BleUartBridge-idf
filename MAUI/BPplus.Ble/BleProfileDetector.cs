using Plugin.BLE.Abstractions.Contracts;

namespace BPplus.Ble;

/// <summary>
/// Queries a connected BLE device's GATT services and matches against
/// <see cref="WellKnownBleProfiles.KnownProfiles"/> in priority order.
/// </summary>
public static class BleProfileDetector
{
    public static async Task<BleGattProfile?> DetectAsync(
        IDevice device, CancellationToken ct = default)
    {
        var services = await device.GetServicesAsync(ct);

        foreach (var profile in WellKnownBleProfiles.KnownProfiles)
        {
            if (profile.ServiceUuid == Guid.Empty)
            {
                // Service UUID not published — scan every service for the matching characteristics.
                foreach (var svc in services)
                {
                    var chars = await svc.GetCharacteristicsAsync(ct);
                    if (MatchesProfile(profile, chars)) return profile;
                }
                continue;
            }

            var service = services.FirstOrDefault(s => s.Id == profile.ServiceUuid);
            if (service == null) continue;

            var serviceChars = await service.GetCharacteristicsAsync(ct);
            if (MatchesProfile(profile, serviceChars)) return profile;
        }

        return null;
    }

    private static bool MatchesProfile(
        BleGattProfile profile,
        IReadOnlyList<Plugin.BLE.Abstractions.Contracts.ICharacteristic> chars)
    {
        bool hasTx = chars.Any(c => c.Id == profile.TxCharUuid &&
            (c.Properties.HasFlag(Plugin.BLE.Abstractions.CharacteristicPropertyType.Write) ||
             c.Properties.HasFlag(Plugin.BLE.Abstractions.CharacteristicPropertyType.WriteWithoutResponse)));

        if (!hasTx) return false;

        if (profile.Bidirectional) return true;

        return chars.Any(c => c.Id == profile.RxCharUuid &&
            (c.Properties.HasFlag(Plugin.BLE.Abstractions.CharacteristicPropertyType.Notify) ||
             c.Properties.HasFlag(Plugin.BLE.Abstractions.CharacteristicPropertyType.Indicate)));
    }
}
