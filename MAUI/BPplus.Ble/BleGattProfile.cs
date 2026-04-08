namespace BPplus.Ble;

/// <summary>
/// GATT service and characteristic UUIDs for a BLE UART adapter profile.
/// When <see cref="Bidirectional"/> is true the same characteristic handles TX and RX.
/// </summary>
public sealed record BleGattProfile(
    string Name,
    Guid   ServiceUuid,
    Guid   TxCharUuid,
    Guid   RxCharUuid,
    bool   Bidirectional,
    int    MaxPayloadBytes = 0);

/// <summary>Well-known GATT profiles for common BLE UART serial adapter chips.</summary>
public static class WellKnownBleProfiles
{
    public static readonly BleGattProfile S2B5232I = new(
        Name:            "S2B5232I",
        ServiceUuid:     Guid.Parse("0003ABCD-0000-1000-8000-00805F9B0131"),
        TxCharUuid:      Guid.Parse("00031202-0000-1000-8000-00805F9B0130"),
        RxCharUuid:      Guid.Parse("00031201-0000-1000-8000-00805F9B0130"),
        Bidirectional:   false,
        MaxPayloadBytes: 244);

    public static readonly BleGattProfile NordicNus = new(
        Name:          "Nordic NUS",
        ServiceUuid:   Guid.Parse("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"),
        TxCharUuid:    Guid.Parse("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"),
        RxCharUuid:    Guid.Parse("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"),
        Bidirectional: false);

    public static readonly BleGattProfile Rn4870 = new(
        Name:          "Microchip RN4870",
        ServiceUuid:   Guid.Parse("49535343-FE7D-4AE5-8FA9-9FAFD205E455"),
        TxCharUuid:    Guid.Parse("49535343-1E4D-4BD9-BA61-23C647249616"),
        RxCharUuid:    Guid.Parse("49535343-8841-43F4-A8D4-ECBE34729BB3"),
        Bidirectional: false);

    public static readonly BleGattProfile Hm10 = new(
        Name:          "HM-10 / JDY",
        ServiceUuid:   Guid.Parse("0000FFE0-0000-1000-8000-00805F9B34FB"),
        TxCharUuid:    Guid.Parse("0000FFE1-0000-1000-8000-00805F9B34FB"),
        RxCharUuid:    Guid.Parse("0000FFE1-0000-1000-8000-00805F9B34FB"),
        Bidirectional: true);

    public static readonly BleGattProfile Hm10Clone = new(
        Name:          "HM-10 clone (FFF0)",
        ServiceUuid:   Guid.Parse("0000FFF0-0000-1000-8000-00805F9B34FB"),
        TxCharUuid:    Guid.Parse("0000FFF1-0000-1000-8000-00805F9B34FB"),
        RxCharUuid:    Guid.Parse("0000FFF1-0000-1000-8000-00805F9B34FB"),
        Bidirectional: true);

    /// <summary>
    /// USConverters BLE232 (ESP32 version of CY8C4128 based BLE RS-232 adapter).
    /// Despite the different product name, this device uses the same GATT profile
    /// as the S2B5232I: service 0003ABCD-…-0131, characteristics 00031202 (Write)
    /// and 00031201 (Notify).
    /// </summary>
    public static readonly BleGattProfile BLE232 = new(
        Name:            "BLE232",
        ServiceUuid:     Guid.Parse("0003ABCD-0000-1000-8000-00805F9B0131"),
        TxCharUuid:      Guid.Parse("00031202-0000-1000-8000-00805F9B0130"),  // Write (app→device)
        RxCharUuid:      Guid.Parse("00031201-0000-1000-8000-00805F9B0130"),  // Notify (device→app)
        Bidirectional:   false,
        MaxPayloadBytes: 244);

    public static IReadOnlyList<BleGattProfile> KnownProfiles { get; } =
        [S2B5232I, NordicNus, Rn4870, Hm10, Hm10Clone, BLE232];
}
