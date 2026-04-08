using BPplus.Ble;
using Plugin.BLE;
using Plugin.BLE.Abstractions.Contracts;

namespace BleUartBridgeTester.Services;

public sealed class RawBleClient : IRawBleClient
{
    private IDevice?         _device;
    private ICharacteristic? _txChar;
    private ICharacteristic? _rxChar;
    private const int        DefaultMtuPayload = 128; // matches ESP32 BLE_CHUNK
    private int              _chunkSize = DefaultMtuPayload;

    public bool IsConnected => _device?.State == Plugin.BLE.Abstractions.DeviceState.Connected;
    public BleGattProfile? ConnectedProfile { get; private set; }

    public event EventHandler<byte[]>? DataReceived;
    public event EventHandler?         Disconnected;

    public async Task ConnectAsync(BleDeviceInfo info, BleGattProfile? profile,
                                   CancellationToken ct = default)
    {
        // Resolve the adapter on a thread-pool thread so Plugin.BLE's blocking WinRT call
        // (GetDefaultAsync().AsTask().Result inside InitializeNative) can complete without
        // deadlocking the UI thread on .NET 10 Windows.
        var adapter = await Task.Run(() => CrossBluetoothLE.Current.Adapter, ct);

        adapter.DeviceDisconnected += OnDeviceDisconnected;

        _device = await adapter.ConnectToKnownDeviceAsync(info.Id, cancellationToken: ct);

        // Auto-detect profile if none was specified.
        profile ??= await BleProfileDetector.DetectAsync(_device, ct)
            ?? throw new InvalidOperationException(
                "No recognised BLE UART profile found on this device. " +
                "Ensure it is a supported BLE serial adapter.");

        ConnectedProfile = profile;
        _chunkSize = profile.MaxPayloadBytes > 0 ? profile.MaxPayloadBytes : DefaultMtuPayload;

        IService service;
        if (profile.ServiceUuid == Guid.Empty)
        {
            // Service UUID not published for this profile — scan all GATT services
            // and find the one that contains the expected characteristics.
            var services = await _device.GetServicesAsync(ct);
            IService? found = null;
            foreach (var svc in services)
            {
                var chars = await svc.GetCharacteristicsAsync(ct);
                bool hasTx = chars.Any(c => c.Id == profile.TxCharUuid);
                bool hasRx = profile.Bidirectional ? hasTx : chars.Any(c => c.Id == profile.RxCharUuid);
                if (hasTx && hasRx) { found = svc; break; }
            }
            service = found ?? throw new InvalidOperationException(
                $"No GATT service found containing {profile.Name} characteristics.");
        }
        else
        {
            service = await _device.GetServiceAsync(profile.ServiceUuid, ct)
                ?? throw new InvalidOperationException($"Service {profile.ServiceUuid} not found.");
        }

        _txChar = await service.GetCharacteristicAsync(profile.TxCharUuid)
            ?? throw new InvalidOperationException("TX characteristic not found.");

        _rxChar = profile.Bidirectional
            ? _txChar
            : await service.GetCharacteristicAsync(profile.RxCharUuid)
              ?? throw new InvalidOperationException("RX characteristic not found.");

        _rxChar.ValueUpdated += OnValueUpdated;
        await _rxChar.StartUpdatesAsync(ct);
    }

    public async Task SendAsync(byte[] data, CancellationToken ct = default)
    {
        if (_txChar is null) throw new InvalidOperationException("Not connected.");

        for (int offset = 0; offset < data.Length; offset += _chunkSize)
        {
            ct.ThrowIfCancellationRequested();
            int count = Math.Min(_chunkSize, data.Length - offset);
            byte[] chunk = data[offset..(offset + count)];
            await _txChar.WriteAsync(chunk, ct);
        }
    }

    public async Task DisconnectAsync()
    {
        if (_device is null) return;

        if (_rxChar is not null)
        {
            _rxChar.ValueUpdated -= OnValueUpdated;
            try { await _rxChar.StopUpdatesAsync(); } catch { }
        }

        try
        {
            CrossBluetoothLE.Current.Adapter.DeviceDisconnected -= OnDeviceDisconnected;
            await CrossBluetoothLE.Current.Adapter.DisconnectDeviceAsync(_device);
        }
        catch { }

        _device          = null;
        _txChar          = null;
        _rxChar          = null;
        _chunkSize       = DefaultMtuPayload;
        ConnectedProfile = null;
    }

    private void OnValueUpdated(object? sender, Plugin.BLE.Abstractions.EventArgs.CharacteristicUpdatedEventArgs e)
        => DataReceived?.Invoke(this, e.Characteristic.Value ?? []);

    private void OnDeviceDisconnected(object? sender, Plugin.BLE.Abstractions.EventArgs.DeviceEventArgs e)
    {
        if (e.Device.Id == _device?.Id)
            Disconnected?.Invoke(this, EventArgs.Empty);
    }
}
