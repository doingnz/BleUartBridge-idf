using BPplus.Ble;

namespace BleUartBridgeTester.Services;

public interface IRawBleClient
{
    bool IsConnected { get; }

    /// <summary>The profile negotiated during the last successful <see cref="ConnectAsync"/>. Null when not connected.</summary>
    BleGattProfile? ConnectedProfile { get; }

    /// <summary>Fired on any thread when a BLE notification arrives.</summary>
    event EventHandler<byte[]> DataReceived;

    /// <summary>Fired when the remote device disconnects.</summary>
    event EventHandler Disconnected;

    /// <summary>
    /// Connect and set up UART characteristics.
    /// Pass <c>null</c> for <paramref name="profile"/> to auto-detect via GATT service scan.
    /// </summary>
    Task ConnectAsync(BleDeviceInfo device, BleGattProfile? profile,
                      CancellationToken ct = default);

    /// <summary>Chunk-writes data respecting negotiated MTU.</summary>
    Task SendAsync(byte[] data, CancellationToken ct = default);

    Task DisconnectAsync();
}
