using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text;
using BleUartBridgeTester.Models;
using BleUartBridgeTester.Services;
using BPplus.Ble;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace BleUartBridgeTester.ViewModels;

public partial class MainViewModel : ObservableObject
{
    // ── Dependencies ──────────────────────────────────────────────────────────
    private readonly IBleScanner        _scanner;
    private readonly IRawBleClient      _ble;
    private readonly ISerialPortService _com;

    // ── BLE Scan ──────────────────────────────────────────────────────────────
    [ObservableProperty] private ObservableCollection<BleDeviceInfo> _bleDevices = [];
    [ObservableProperty] private BleDeviceInfo? _selectedBleDevice;
    [ObservableProperty] private bool   _isScanning;

    // ── Connection ────────────────────────────────────────────────────────────
    [ObservableProperty] [NotifyPropertyChangedFor(nameof(IsConnected))]
    private bool _isBleConnected;

    [ObservableProperty] [NotifyPropertyChangedFor(nameof(IsConnected))]
    private bool _isComConnected;

    [ObservableProperty] private string _bleStatus  = "Not connected";
    [ObservableProperty] private string _comStatus  = "Not connected";
    [ObservableProperty] private bool   _isComAvailable;

    public bool IsConnected => IsBleConnected && (IsComConnected || !IsComAvailable);

    // ── COM Config ────────────────────────────────────────────────────────────
    [ObservableProperty] private ObservableCollection<string> _availablePorts = [];
    [ObservableProperty] private string? _selectedPort;
    [ObservableProperty] private int    _selectedBaudRate  = 115200;
    [ObservableProperty] private string _selectedDataBits  = "8";
    [ObservableProperty] private string _selectedParity    = "None";
    [ObservableProperty] private string _selectedStopBits  = "1";
    [ObservableProperty] private bool   _flowControlEnabled = true;

    public int[]    BaudRates       { get; } = [9600, 19200, 38400, 57600, 115200, 230400, 460800];
    public string[] DataBitsOptions { get; } = ["7", "8"];
    public string[] ParityOptions   { get; } = ["None", "Even", "Odd", "Mark", "Space"];
    public string[] StopBitsOptions { get; } = ["1", "1.5", "2"];

    // ── Manual Echo ───────────────────────────────────────────────────────────
    [ObservableProperty] private string _comSendText = "";
    [ObservableProperty] private string _bleSendText = "";

    [ObservableProperty] private string _comToBleSentHex     = "";
    [ObservableProperty] private string _comToBleReceivedHex = "";
    [ObservableProperty] private string _bleToComSentHex     = "";
    [ObservableProperty] private string _bleToComReceivedHex = "";
    [ObservableProperty] private string _comToBleMatchStatus = "";
    [ObservableProperty] private string _bleToComMatchStatus = "";
    [ObservableProperty] private Color  _comToBleMatchColor  = Colors.Gray;
    [ObservableProperty] private Color  _bleToComMatchColor  = Colors.Gray;

    private byte[]?      _pendingComSent;
    private byte[]?      _pendingBleSent;
    private readonly List<byte> _bleEchoBuffer = [];
    private readonly List<byte> _comEchoBuffer = [];
    private CancellationTokenSource? _comEchoTimeoutCts;
    private CancellationTokenSource? _bleEchoTimeoutCts;

    // ── Tests ─────────────────────────────────────────────────────────────────
    [ObservableProperty] [NotifyCanExecuteChangedFor(nameof(StartComTestCommand))]
    private bool _isComTestRunning;

    [ObservableProperty] [NotifyCanExecuteChangedFor(nameof(StartBleTestCommand))]
    private bool _isBleTestRunning;

    [ObservableProperty] private bool      _stopOnError = true;
    [ObservableProperty] private TestStats _comTestStats = new();
    [ObservableProperty] private TestStats _bleTestStats = new();

    // Simulated receiver delays — applied independently per direction so that
    // each path's backpressure can be stressed separately.
    // COM→BLE: delay in OnBleDataReceived backs up the NimBLE mbuf pool.
    // BLE→COM: delay in OnComDataReceived fills the OS serial buffer → RTS asserts.
    [ObservableProperty] private bool _comToBleSimDelayEnabled;
    [ObservableProperty] private int  _comToBleSimDelayMaxMs = 150;
    [ObservableProperty] private bool _bleToComSimDelayEnabled;
    [ObservableProperty] private int  _bleToComSimDelayMaxMs = 150;

    private readonly Random _rng = new();

    // Error capture — flat ViewModel properties so WinUI bindings update reliably
    [ObservableProperty] private bool   _comTestHasError;
    [ObservableProperty] private string _comTestErrorType     = "";
    [ObservableProperty] private string _comTestErrorSent     = "";
    [ObservableProperty] private string _comTestErrorReceived = "";
    [ObservableProperty] private bool   _bleTestHasError;
    [ObservableProperty] private string _bleTestErrorType     = "";
    [ObservableProperty] private string _bleTestErrorSent     = "";
    [ObservableProperty] private string _bleTestErrorReceived = "";

    private CancellationTokenSource? _comTestCts;
    private CancellationTokenSource? _bleTestCts;

    // Last successfully received seq — used for gap detection and for building
    // the correct "Expected" packet in sync-error diagnostics.
    // Null until the first packet arrives in a test run.
    // Only accessed on the main thread (via BeginInvokeOnMainThread).
    private uint? _comToBleLastSeq;
    private uint? _bleToComLastSeq;

    // Packet parsers for stress test
    private readonly StreamPacketParser _comToBleParser = new();
    private readonly StreamPacketParser _bleToComParser = new();

    // Latency tracking: seq → send timestamp
    private readonly ConcurrentDictionary<uint, long> _comToBlePending = new();
    private readonly ConcurrentDictionary<uint, long> _bleToComPending = new();

    // Bytes received per 250 ms interval (for B/s calc)
    private long _comToBleRecvBytes;
    private long _bleToComRecvBytes;
    private long _comTestStartTick;
    private long _bleTestStartTick;

    private IDispatcherTimer? _statsTimer;

    // ── Constructor ───────────────────────────────────────────────────────────

    public MainViewModel(IBleScanner scanner, IRawBleClient ble, ISerialPortService com)
    {
        _scanner = scanner;
        _ble     = ble;
        _com     = com;

        _scanner.DeviceDiscovered += OnDeviceDiscovered;
        _ble.DataReceived         += OnBleDataReceived;
        _ble.Disconnected         += OnBleDisconnected;
        _com.DataReceived         += OnComDataReceived;

        _comToBleParser.PacketReceived += OnComToBlePacketReceived;
        _comToBleParser.SyncError      += (_, snap) => OnComToBleSyncError(snap);
        _bleToComParser.PacketReceived += OnBleToComPacketReceived;
        _bleToComParser.SyncError      += (_, snap) => OnBleToComSyncError(snap);

#if ANDROID
        IsComAvailable = false;
#else
        IsComAvailable = true;
        RefreshPorts();
#endif
    }

    // ── BLE Scanning ──────────────────────────────────────────────────────────

    [RelayCommand(AllowConcurrentExecutions = true)]
    private async Task ToggleScan()
    {
        if (IsScanning)
        {
            await _scanner.StopAsync();
            IsScanning = false;
            return;
        }

#if ANDROID
        var status = await Permissions.RequestAsync<Permissions.Bluetooth>();
        if (status != PermissionStatus.Granted)
        {
            BleStatus = "Bluetooth permission denied";
            return;
        }
#endif

        BleDevices.Clear();
        IsScanning = true;
        BleStatus  = "Scanning…";

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        try   { await _scanner.StartAsync(cts.Token); }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            BleStatus = $"BLE error: {ex.Message}";
            IsScanning = false;
            return;
        }
        finally
        {
            IsScanning = false;
            BleStatus  = $"{BleDevices.Count} device(s) found";
        }
    }

    private void OnDeviceDiscovered(object? sender, BleDeviceInfo info)
        => MainThread.BeginInvokeOnMainThread(() => BleDevices.Add(info));

    // ── Connect / Disconnect ──────────────────────────────────────────────────

    [RelayCommand(AllowConcurrentExecutions = true)]
    private async Task Connect()
    {
        if (SelectedBleDevice is null)
        {
            BleStatus = "Select a BLE device first";
            return;
        }

        if (IsComAvailable && SelectedPort is null)
        {
            ComStatus = "Select a COM port first";
            return;
        }

        // BLE
        try
        {
            BleStatus = "Connecting…";
            if (_scanner.IsScanning) await _scanner.StopAsync();

            await _ble.ConnectAsync(SelectedBleDevice, profile: null);
            IsBleConnected = true;
            BleStatus = $"Connected ({_ble.ConnectedProfile?.Name ?? "unknown profile"})";
        }
        catch (Exception ex)
        {
            BleStatus = $"BLE error: {ex.Message}";
            return;
        }

        // COM
        if (IsComAvailable && SelectedPort is not null)
        {
            try
            {
                ComStatus = "Opening…";
                _com.Open(SelectedPort, SelectedBaudRate,
                          int.Parse(SelectedDataBits),
                          SelectedParity, SelectedStopBits,
                          FlowControlEnabled);
                IsComConnected = true;
                ComStatus = $"Open ({SelectedPort} {SelectedBaudRate},{SelectedParity[0]},{SelectedDataBits},{SelectedStopBits})";
            }
            catch (Exception ex)
            {
                ComStatus = $"COM error: {ex.Message}";
                await _ble.DisconnectAsync();
                IsBleConnected = false;
                BleStatus = "Disconnected";
            }
        }

        OnPropertyChanged(nameof(IsConnected));
        StartStatsTimer();
    }

    [RelayCommand]
    private async Task Disconnect()
    {
        StopAllTests();
        StopStatsTimer();

        if (_ble.IsConnected) await _ble.DisconnectAsync();
        IsBleConnected = false;
        BleStatus = "Disconnected";

        _com.Close();
        IsComConnected = false;
        ComStatus = "Closed";

        OnPropertyChanged(nameof(IsConnected));
    }

    private void OnBleDisconnected(object? sender, EventArgs e)
        => MainThread.BeginInvokeOnMainThread(async () =>
        {
            IsBleConnected = false;
            BleStatus = "Disconnected (remote)";
            StopAllTests();
            _com.Close();
            IsComConnected = false;
            ComStatus = "Closed";
            OnPropertyChanged(nameof(IsConnected));
            await Task.CompletedTask;
        });

    // ── COM Port ──────────────────────────────────────────────────────────────

    [RelayCommand]
    private void RefreshPorts()
    {
        try
        {
            AvailablePorts.Clear();
            foreach (var p in _com.GetAvailablePorts())
                AvailablePorts.Add(p);
            if (SelectedPort is not null && !AvailablePorts.Contains(SelectedPort))
                SelectedPort = null;
            ComStatus = AvailablePorts.Count > 0
                ? $"{AvailablePorts.Count} port(s) found"
                : "No COM ports found";
        }
        catch (Exception ex)
        {
            ComStatus = $"Port scan error: {ex.Message}";
        }
    }

    // ── Manual Send ───────────────────────────────────────────────────────────

    [RelayCommand]
    private void SendCom()
    {
        if (!IsConnected || string.IsNullOrEmpty(ComSendText)) return;

        byte[] data = Encoding.UTF8.GetBytes(ComSendText);
        ComToBleSentHex     = FormatHex(data);
        ComToBleReceivedHex = "";
        ComToBleMatchStatus = "Waiting…";
        ComToBleMatchColor  = Colors.Gray;

        lock (_bleEchoBuffer) _bleEchoBuffer.Clear();
        _pendingComSent = data;

        _comEchoTimeoutCts?.Cancel();
        _comEchoTimeoutCts = new CancellationTokenSource();
        var ct = _comEchoTimeoutCts.Token;

        try { _com.Send(data); }
        catch (Exception ex)
        {
            ComToBleMatchStatus = $"Send error: {ex.Message}";
            ComToBleMatchColor  = Colors.Red;
            return;
        }

        // Wait 3 s for echo then compare
        Task.Delay(3000, ct).ContinueWith(_ =>
        {
            if (ct.IsCancellationRequested) return;
            MainThread.BeginInvokeOnMainThread(CompareComToBlEcho);
        }, TaskScheduler.Default);
    }

    [RelayCommand(AllowConcurrentExecutions = true)]
    private async Task SendBle()
    {
        if (!IsConnected || string.IsNullOrEmpty(BleSendText)) return;

        byte[] data = Encoding.UTF8.GetBytes(BleSendText);
        BleToComSentHex     = FormatHex(data);
        BleToComReceivedHex = "";
        BleToComMatchStatus = "Waiting…";
        BleToComMatchColor  = Colors.Gray;

        lock (_comEchoBuffer) _comEchoBuffer.Clear();
        _pendingBleSent = data;

        _bleEchoTimeoutCts?.Cancel();
        _bleEchoTimeoutCts = new CancellationTokenSource();
        var ct = _bleEchoTimeoutCts.Token;

        try { await _ble.SendAsync(data); }
        catch (Exception ex)
        {
            BleToComMatchStatus = $"Send error: {ex.Message}";
            BleToComMatchColor  = Colors.Red;
            return;
        }

        await Task.Delay(3000, ct).ContinueWith(_ =>
        {
            if (ct.IsCancellationRequested) return;
            MainThread.BeginInvokeOnMainThread(CompareBleToComEcho);
        }, TaskScheduler.Default);
    }

    [RelayCommand]
    private void ClearHexDumps()
    {
        ComToBleSentHex     = "";
        ComToBleReceivedHex = "";
        BleToComSentHex     = "";
        BleToComReceivedHex = "";
        ComToBleMatchStatus = "";
        BleToComMatchStatus = "";
        ComToBleMatchColor  = Colors.Gray;
        BleToComMatchColor  = Colors.Gray;
        lock (_bleEchoBuffer) _bleEchoBuffer.Clear();
        lock (_comEchoBuffer) _comEchoBuffer.Clear();
        _pendingComSent = null;
        _pendingBleSent = null;
    }

    private void CompareComToBlEcho()
    {
        byte[] sent;
        byte[] received;
        lock (_bleEchoBuffer)
        {
            sent     = _pendingComSent ?? [];
            received = [.. _bleEchoBuffer];
        }
        ComToBleReceivedHex = FormatHex(received);
        bool match = sent.SequenceEqual(received);
        ComToBleMatchStatus = match ? "MATCH ✓" : $"MISMATCH ✗  (sent {sent.Length} B, received {received.Length} B)";
        ComToBleMatchColor  = match ? Colors.Green : Colors.Red;

        if (!match && StopOnError) StopAllTests();
    }

    private void CompareBleToComEcho()
    {
        byte[] sent;
        byte[] received;
        lock (_comEchoBuffer)
        {
            sent     = _pendingBleSent ?? [];
            received = [.. _comEchoBuffer];
        }
        BleToComReceivedHex = FormatHex(received);
        bool match = sent.SequenceEqual(received);
        BleToComMatchStatus = match ? "MATCH ✓" : $"MISMATCH ✗  (sent {sent.Length} B, received {received.Length} B)";
        BleToComMatchColor  = match ? Colors.Green : Colors.Red;

        if (!match && StopOnError) StopAllTests();
    }

    // ── Data receive handlers ─────────────────────────────────────────────────

    private void OnBleDataReceived(object? sender, byte[] data)
    {
        // Simulate a busy receiver: block this thread before consuming data.
        // Slowing BLE notification consumption backs up the NimBLE mbuf pool on
        // the ESP32, exercising the nus_notify retry / backpressure path.
        ApplySimulatedRxDelay(ComToBleSimDelayEnabled, ComToBleSimDelayMaxMs);

        // Manual echo buffer (COM→BLE direction)
        if (_pendingComSent is not null)
            lock (_bleEchoBuffer) _bleEchoBuffer.AddRange(data);

        // Stress test parser
        if (IsComTestRunning)
            _comToBleParser.Feed(data);

        Interlocked.Add(ref _comToBleRecvBytes, data.Length);
    }

    private void OnComDataReceived(object? sender, byte[] data)
    {
        // Simulate a busy receiver: block this thread before consuming data.
        // The serial driver's OS buffer fills while we sleep; once the kernel RTS
        // threshold is reached, RTS asserts and the ESP32's CTS pauses its UART TX.
        ApplySimulatedRxDelay(BleToComSimDelayEnabled, BleToComSimDelayMaxMs);

        // Manual echo buffer (BLE→COM direction)
        if (_pendingBleSent is not null)
            lock (_comEchoBuffer) _comEchoBuffer.AddRange(data);

        // Stress test parser
        if (IsBleTestRunning)
            _bleToComParser.Feed(data);

        Interlocked.Add(ref _bleToComRecvBytes, data.Length);
    }

    private void ApplySimulatedRxDelay(bool enabled, int maxMs)
    {
        if (!enabled || maxMs <= 0) return;
        Thread.Sleep(_rng.Next(0, maxMs + 1));
    }

    // ── Stress Test: packet builder ───────────────────────────────────────────

    private static byte[] BuildPacket(uint seq)
    {
        int payloadLen = (int)(seq % 100) + 8; // 8-107 bytes
        Span<byte> header = stackalloc byte[6];
        BinaryPrimitives.WriteUInt32LittleEndian(header,      seq);
        BinaryPrimitives.WriteUInt16LittleEndian(header[4..], (ushort)payloadLen);

        byte[] payload = new byte[payloadLen];
        for (int i = 0; i < payloadLen; i++)
            payload[i] = (byte)((seq + i) & 0xFF);

        byte crc = Crc8.Compute(header);
        crc = Crc8.Compute(payload, crc);

        byte[] packet = new byte[6 + payloadLen + 2];
        header.CopyTo(packet);
        payload.CopyTo(packet, 6);
        packet[6 + payloadLen]     = crc;
        packet[6 + payloadLen + 1] = 0x00;
        return packet;
    }

    private static bool ValidatePayload(uint seq, byte[] payload)
    {
        int expectedLen = (int)(seq % 100) + 8;
        if (payload.Length != expectedLen) return false;
        for (int i = 0; i < payload.Length; i++)
            if (payload[i] != (byte)((seq + i) & 0xFF)) return false;
        return true;
    }

    // ── COM→BLE Stress Test ───────────────────────────────────────────────────

    [RelayCommand(CanExecute = nameof(CanStartComTest))]
    private async Task StartComTest()
    {
        _comTestCts = new CancellationTokenSource();
        IsComTestRunning = true;
        ComTestStats.Reset();
        ComTestHasError = false; ComTestErrorType = ""; ComTestErrorSent = ""; ComTestErrorReceived = "";
        _comToBlePending.Clear();
        _comToBleParser.Reset();
        _comToBleLastSeq = null;
        _comTestStartTick = Stopwatch.GetTimestamp();

        var ct = _comTestCts.Token;
        try
        {
            await Task.Run(async () =>
            {
                uint seq = 0;
                while (!ct.IsCancellationRequested)
                {
                    byte[] packet = BuildPacket(seq);
                    _comToBlePending[seq] = Stopwatch.GetTimestamp();

                    try { _com.Send(packet); }
                    catch (Exception ex)
                    {
                        MainThread.BeginInvokeOnMainThread(() =>
                        {
                            if (!ComTestHasError)
                            {
                                ComTestErrorType     = $"COM send error: {ex.Message}";
                                ComTestErrorSent     = "";
                                ComTestErrorReceived = "";
                                ComTestHasError      = true;
                            }
                            ComTestStats.Errors++;
                            if (StopOnError) StopAllTests();
                        });
                        break;
                    }

                    MainThread.BeginInvokeOnMainThread(() => ComTestStats.PacketsSent++);
                    PurgeOldPending(_comToBlePending);

                    seq++;
                    await Task.Yield();
                }
            }, ct);
        }
        catch (OperationCanceledException) { }
        finally
        {
            IsComTestRunning = false;
            _comTestCts?.Dispose();
            _comTestCts = null;
        }
    }

    private bool CanStartComTest() => IsConnected && !IsComTestRunning;

    [RelayCommand]
    private void StopComTest()
    {
        _comTestCts?.Cancel();
        IsComTestRunning = false;
    }

    // ── BLE→COM Stress Test ───────────────────────────────────────────────────

    [RelayCommand(CanExecute = nameof(CanStartBleTest))]
    private async Task StartBleTest()
    {
        _bleTestCts = new CancellationTokenSource();
        IsBleTestRunning = true;
        BleTestStats.Reset();
        BleTestHasError = false; BleTestErrorType = ""; BleTestErrorSent = ""; BleTestErrorReceived = "";
        _bleToComPending.Clear();
        _bleToComParser.Reset();
        _bleToComLastSeq = null;
        _bleTestStartTick = Stopwatch.GetTimestamp();

        var ct = _bleTestCts.Token;
        try
        {
            await Task.Run(async () =>
            {
                uint seq = 0;
                while (!ct.IsCancellationRequested)
                {
                    byte[] packet = BuildPacket(seq);
                    _bleToComPending[seq] = Stopwatch.GetTimestamp();

                    try { await _ble.SendAsync(packet, ct); }
                    catch (OperationCanceledException) { break; }
                    catch (Exception ex)
                    {
                        MainThread.BeginInvokeOnMainThread(() =>
                        {
                            if (!BleTestHasError)
                            {
                                BleTestErrorType     = $"BLE send error: {ex.Message}";
                                BleTestErrorSent     = "";
                                BleTestErrorReceived = "";
                                BleTestHasError      = true;
                            }
                            BleTestStats.Errors++;
                            if (StopOnError) StopAllTests();
                        });
                        break;
                    }

                    MainThread.BeginInvokeOnMainThread(() => BleTestStats.PacketsSent++);
                    PurgeOldPending(_bleToComPending);
                    seq++;
                }
            }, ct);
        }
        catch (OperationCanceledException) { }
        finally
        {
            IsBleTestRunning = false;
            _bleTestCts?.Dispose();
            _bleTestCts = null;
        }
    }

    private bool CanStartBleTest() => IsConnected && !IsBleTestRunning;

    [RelayCommand]
    private void StopBleTest()
    {
        _bleTestCts?.Cancel();
        IsBleTestRunning = false;
    }

    [RelayCommand]
    private void StopAllTests()
    {
        StopComTest();
        StopBleTest();
    }

    // ── Packet received callbacks ─────────────────────────────────────────────

    private void OnComToBlePacketReceived(object? sender, ParsedPacket pkt)
    {
        bool payloadOk = ValidatePayload(pkt.Seq, pkt.Payload);
        double latencyMs = 0;

        if (_comToBlePending.TryRemove(pkt.Seq, out long sent))
            latencyMs = (Stopwatch.GetTimestamp() - sent) * 1000.0 / Stopwatch.Frequency;

        byte[]? expectedPayload = payloadOk ? null : BuildExpectedPayload(pkt.Seq);

        MainThread.BeginInvokeOnMainThread(() =>
        {
            ComTestStats.PacketsReceived++;
            if (latencyMs > 0) ComTestStats.UpdateLatency(latencyMs);

            // Sequence-gap check: did we skip one or more packets?
            if (_comToBleLastSeq.HasValue && pkt.Seq != _comToBleLastSeq.Value + 1)
            {
                uint expectedSeq = _comToBleLastSeq.Value + 1;
                uint gap         = pkt.Seq - expectedSeq;   // wraps naturally for uint
                if (!ComTestHasError)
                {
                    ComTestErrorType     = $"Sequence gap — expected seq={expectedSeq}, got seq={pkt.Seq}  ({gap} packet(s) lost)";
                    ComTestErrorSent     = FormatHex(BuildPacket(expectedSeq));
                    ComTestErrorReceived = $"Next packet received had seq={pkt.Seq}\n\n" + FormatHex(pkt.Payload);
                    ComTestHasError      = true;
                }
                ComTestStats.Errors++;
                if (StopOnError) { StopAllTests(); return; }
            }

            if (!payloadOk)
            {
                if (!ComTestHasError)
                {
                    ComTestErrorType     = $"Payload mismatch  seq={pkt.Seq}  expected {expectedPayload!.Length}B  got {pkt.Payload.Length}B";
                    ComTestErrorSent     = FormatHex(expectedPayload!);
                    ComTestErrorReceived = FormatHex(pkt.Payload);
                    ComTestHasError      = true;
                }
                ComTestStats.Errors++;
                if (StopOnError) { StopAllTests(); return; }
            }

            _comToBleLastSeq = pkt.Seq;
        });
    }

    private void OnComToBleSyncError(byte[] snapshot)
        => MainThread.BeginInvokeOnMainThread(() =>
        {
            if (!IsComTestRunning) return;
            if (!ComTestHasError)
            {
                // Prefer last-known-good seq+1 for Expected — it's reliable.
                // Fall back to decoding seq from the received bytes only if no
                // prior packet has been successfully received this run.
                string expectedHex = _comToBleLastSeq.HasValue
                    ? FormatHex(BuildPacket(_comToBleLastSeq.Value + 1))
                    : (snapshot.Length >= 4
                        ? FormatHex(BuildPacket(BinaryPrimitives.ReadUInt32LittleEndian(snapshot)))
                        : "— (no prior packet received)");

                string label = _comToBleLastSeq.HasValue
                    ? $"CRC / sync error  —  last good seq={_comToBleLastSeq.Value}  ({snapshot.Length} bytes in parser buffer)"
                    : $"CRC / sync error  —  no prior seq  ({snapshot.Length} bytes in parser buffer)";

                ComTestErrorType     = label;
                ComTestErrorSent     = expectedHex;
                ComTestErrorReceived = FormatHex(snapshot);
                ComTestHasError      = true;
            }
            ComTestStats.Errors++;
            if (StopOnError) StopAllTests();
        });

    private void OnBleToComPacketReceived(object? sender, ParsedPacket pkt)
    {
        bool payloadOk = ValidatePayload(pkt.Seq, pkt.Payload);
        double latencyMs = 0;

        if (_bleToComPending.TryRemove(pkt.Seq, out long sent))
            latencyMs = (Stopwatch.GetTimestamp() - sent) * 1000.0 / Stopwatch.Frequency;

        byte[]? expectedPayload = payloadOk ? null : BuildExpectedPayload(pkt.Seq);

        MainThread.BeginInvokeOnMainThread(() =>
        {
            BleTestStats.PacketsReceived++;
            if (latencyMs > 0) BleTestStats.UpdateLatency(latencyMs);

            if (_bleToComLastSeq.HasValue && pkt.Seq != _bleToComLastSeq.Value + 1)
            {
                uint expectedSeq = _bleToComLastSeq.Value + 1;
                uint gap         = pkt.Seq - expectedSeq;
                if (!BleTestHasError)
                {
                    BleTestErrorType     = $"Sequence gap — expected seq={expectedSeq}, got seq={pkt.Seq}  ({gap} packet(s) lost)";
                    BleTestErrorSent     = FormatHex(BuildPacket(expectedSeq));
                    BleTestErrorReceived = $"Next packet received had seq={pkt.Seq}\n\n" + FormatHex(pkt.Payload);
                    BleTestHasError      = true;
                }
                BleTestStats.Errors++;
                if (StopOnError) { StopAllTests(); return; }
            }

            if (!payloadOk)
            {
                if (!BleTestHasError)
                {
                    BleTestErrorType     = $"Payload mismatch  seq={pkt.Seq}  expected {expectedPayload!.Length}B  got {pkt.Payload.Length}B";
                    BleTestErrorSent     = FormatHex(expectedPayload!);
                    BleTestErrorReceived = FormatHex(pkt.Payload);
                    BleTestHasError      = true;
                }
                BleTestStats.Errors++;
                if (StopOnError) { StopAllTests(); return; }
            }

            _bleToComLastSeq = pkt.Seq;
        });
    }

    private void OnBleToComSyncError(byte[] snapshot)
        => MainThread.BeginInvokeOnMainThread(() =>
        {
            if (!IsBleTestRunning) return;
            if (!BleTestHasError)
            {
                string expectedHex = _bleToComLastSeq.HasValue
                    ? FormatHex(BuildPacket(_bleToComLastSeq.Value + 1))
                    : (snapshot.Length >= 4
                        ? FormatHex(BuildPacket(BinaryPrimitives.ReadUInt32LittleEndian(snapshot)))
                        : "— (no prior packet received)");

                string label = _bleToComLastSeq.HasValue
                    ? $"CRC / sync error  —  last good seq={_bleToComLastSeq.Value}  ({snapshot.Length} bytes in parser buffer)"
                    : $"CRC / sync error  —  no prior seq  ({snapshot.Length} bytes in parser buffer)";

                BleTestErrorType     = label;
                BleTestErrorSent     = expectedHex;
                BleTestErrorReceived = FormatHex(snapshot);
                BleTestHasError      = true;
            }
            BleTestStats.Errors++;
            if (StopOnError) StopAllTests();
        });

    private static byte[] BuildExpectedPayload(uint seq)
    {
        int len = (int)(seq % 100) + 8;
        byte[] p = new byte[len];
        for (int i = 0; i < len; i++) p[i] = (byte)((seq + i) & 0xFF);
        return p;
    }

    // ── Stats timer ───────────────────────────────────────────────────────────

    private void StartStatsTimer()
    {
        _statsTimer = Application.Current!.Dispatcher.CreateTimer();
        _statsTimer.Interval = TimeSpan.FromMilliseconds(250);
        _statsTimer.Tick += OnStatsTick;
        _statsTimer.Start();
    }

    private void StopStatsTimer()
    {
        _statsTimer?.Stop();
        _statsTimer = null;
    }

    private void OnStatsTick(object? sender, EventArgs e)
    {
        long comBytes = Interlocked.Exchange(ref _comToBleRecvBytes, 0);
        long bleBytes = Interlocked.Exchange(ref _bleToComRecvBytes, 0);

        if (IsComTestRunning)
        {
            ComTestStats.BytesPerSec = comBytes * 4; // 250 ms → extrapolate to /s
            var elapsed = TimeSpan.FromSeconds(
                (Stopwatch.GetTimestamp() - _comTestStartTick) / (double)Stopwatch.Frequency);
            ComTestStats.Elapsed = elapsed.ToString(@"hh\:mm\:ss");
        }

        if (IsBleTestRunning)
        {
            BleTestStats.BytesPerSec = bleBytes * 4;
            var elapsed = TimeSpan.FromSeconds(
                (Stopwatch.GetTimestamp() - _bleTestStartTick) / (double)Stopwatch.Frequency);
            BleTestStats.Elapsed = elapsed.ToString(@"hh\:mm\:ss");
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private static void PurgeOldPending(ConcurrentDictionary<uint, long> dict)
    {
        long expiry = Stopwatch.GetTimestamp() - (long)(10 * Stopwatch.Frequency);
        foreach (var kvp in dict)
            if (kvp.Value < expiry) dict.TryRemove(kvp.Key, out _);
    }

    public static string FormatHex(byte[] data)
    {
        if (data.Length == 0) return "(empty)";
        const int W = 16;
        var sb = new StringBuilder();
        for (int i = 0; i < data.Length; i += W)
        {
            sb.Append($"{i:X4}  ");
            for (int j = 0; j < W; j++)
            {
                if (i + j < data.Length) sb.Append($"{data[i + j]:X2} ");
                else                     sb.Append("   ");
                if (j == 7) sb.Append(' ');
            }
            sb.Append(' ');
            for (int j = 0; j < W && i + j < data.Length; j++)
            {
                byte b = data[i + j];
                sb.Append(b is >= 0x20 and <= 0x7E ? (char)b : '.');
            }
            sb.AppendLine();
        }
        return sb.ToString();
    }
}
