using System.Collections.Concurrent;
using Plugin.BLE;
using Plugin.BLE.Abstractions.Contracts;
using Plugin.BLE.Abstractions.EventArgs;
using IAdapter = Plugin.BLE.Abstractions.Contracts.IAdapter;

namespace BPplus.Ble;

/// <summary>
/// Cross-platform BLE scanner using Plugin.BLE.
/// De-duplicates discoveries by device ID.
///
/// Plugin.BLE's <see cref="CrossBluetoothLE.Current"/> must NOT be accessed
/// during DI construction — on Windows the underlying WinRT COM objects are
/// not ready until the UI thread is running. All access is deferred to the
/// <see cref="StartAsync"/>/<see cref="StopAsync"/> call sites.
/// </summary>
public sealed class BleScanner : IBleScanner
{
    private IAdapter? _adapter;
    private IAdapter Adapter => _adapter ??= CrossBluetoothLE.Current.Adapter;

    private readonly ConcurrentDictionary<Guid, BleDeviceInfo> _seen = new();

    public bool IsScanning => _adapter?.IsScanning == true;

    public event EventHandler<BleDeviceInfo>? DeviceDiscovered;

    public async Task StartAsync(CancellationToken ct = default)
    {
        _seen.Clear();

        // Plugin.BLE's Windows implementation calls BluetoothAdapter.GetDefaultAsync().AsTask().Result
        // inside its constructor/InitializeNative.  On .NET 10 the WinRT completion callback
        // cannot be dispatched back to the blocked UI (STA) thread, causing a deadlock / NRE.
        // Force the first access — and therefore the blocking WinRT call — onto a thread-pool
        // (MTA) thread where the async completion can complete freely.
        var adapter = await Task.Run(() => Adapter, ct);

        adapter.DeviceDiscovered += OnDeviceDiscovered;
        await adapter.StartScanningForDevicesAsync(cancellationToken: ct);
    }

    public async Task StopAsync()
    {
        if (_adapter != null)
        {
            _adapter.DeviceDiscovered -= OnDeviceDiscovered;
            if (_adapter.IsScanning)
                await _adapter.StopScanningForDevicesAsync();
        }
    }

    private void OnDeviceDiscovered(object? sender, DeviceEventArgs e)
    {
        var device = e.Device;
        var info = new BleDeviceInfo(
            device.Id,
            device.Name ?? string.Empty,
            device.Rssi,
            DateTimeOffset.UtcNow);

        if (_seen.TryAdd(device.Id, info))
            DeviceDiscovered?.Invoke(this, info);
        else
            _seen[device.Id] = info;
    }
}
