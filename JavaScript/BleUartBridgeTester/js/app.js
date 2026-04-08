/**
 * BLE UART Bridge Tester — main application
 *
 * COM→BLE test : send via Serial → receive via BLE notifications
 * BLE→COM test : send via BLE write → receive via Serial
 */

// ── Transport clients ─────────────────────────────────────────────────────────

const bleNus    = new BleNusClient();
const bleBle232 = new BleBle232Client();
let   ble       = bleNus;   // active BLE client — reassigned by profile selector
const serial    = new SerialPortClient();

// ── Test state ────────────────────────────────────────────────────────────────

function makeTestState() {
    return {
        running:  false,
        sent:     0,
        recv:     0,
        errors:   0,
        bps:      0,          // bytes received in last 250 ms window
        bpsDisplay: 0,        // extrapolated bytes/sec
        latSum:   0,
        latCount: 0,
        latAvg:   0,
        latMin:   Infinity,
        latMax:   0,
        elapsed:  '00:00:00',
        startMs:  0,
        recvBytes:  0,        // cumulative for bps calc
        prevBytes:  0,
        parser:   null,       // StreamPacketParser
        pending:  new Map(),  // seq → performance.now() at send
        lastSeq:  null,       // last successfully received seq
        hasError: false,
        errorType:        '',
        errorExpected:    '',
        errorReceived:    '',
        errorExpectedRaw: null,   // Uint8Array — for diff colouring
        errorReceivedRaw: null,
    };
}

const comTest = makeTestState();
const bleTest = makeTestState();
comTest.parser = new StreamPacketParser();
bleTest.parser = new StreamPacketParser();

// Simulated Rx delay config
const sim = {
    comToBle: { enabled: false, maxMs: 150 },
    bleToCom: { enabled: false, maxMs: 150 },
};

let stopOnError   = true;
let statsInterval = null;

// Generation counters — incremented each time a test starts so that stale
// ble.onData handlers still pending in _notifyQueue (backed up from a previous
// run) can detect they belong to an old generation and discard their data.
// Without this, restarting after a stopOnError with both delays active causes
// old high-seq notifications to feed the freshly-reset parser, triggering a
// false "expected N, got 0" sequence-gap error.
let comTestGeneration = 0;
let bleTestGeneration = 0;

// ── Connection event handlers ─────────────────────────────────────────────────

for (const client of [bleNus, bleBle232]) {
    client.onConnect    = () => updateConnectionUI();
    client.onDisconnect = () => { stopAllTests(); updateConnectionUI(); };
}

serial.onConnect    = () => updateConnectionUI();
serial.onDisconnect = () => { stopAllTests(); updateConnectionUI(); };

// ── Receive handlers ──────────────────────────────────────────────────────────
// Both handlers await onData (via the transport impls) so that a simulated
// delay blocks the read loop and creates real backpressure.

async function onBleData(data) {
    // Capture generation NOW (at event-queue time) so we can detect if the
    // test was restarted while this handler was sitting in _notifyQueue.
    const gen = comTestGeneration;
    // Apply COM→BLE simulated receiver delay before processing
    if (sim.comToBle.enabled && sim.comToBle.maxMs > 0) {
        await sleep(Math.floor(Math.random() * (sim.comToBle.maxMs + 1)));
    }
    if (comTest.running && comTestGeneration === gen) {
        comTest.recvBytes += data.length;
        comTest.parser.feed(data);
    }
}
bleNus.onData = bleBle232.onData = onBleData;

serial.onData = async (data) => {
    const gen = bleTestGeneration;
    // Apply BLE→COM simulated receiver delay before processing
    if (sim.bleToCom.enabled && sim.bleToCom.maxMs > 0) {
        await sleep(Math.floor(Math.random() * (sim.bleToCom.maxMs + 1)));
    }
    if (bleTest.running && bleTestGeneration === gen) {
        bleTest.recvBytes += data.length;
        bleTest.parser.feed(data);
    }
};

// ── Parser callbacks ──────────────────────────────────────────────────────────

comTest.parser.onPacket = (seq, payload) => {
    const payloadOk = validatePayload(seq, payload);
    const now = performance.now();
    let latMs = 0;
    if (comTest.pending.has(seq)) {
        latMs = now - comTest.pending.get(seq);
        comTest.pending.delete(seq);
    }
    comTest.recv++;

    if (latMs > 0) updateLatency(comTest, latMs);

    // Sequence gap detection
    if (comTest.lastSeq !== null) {
        const expected = (comTest.lastSeq + 1) >>> 0;
        if (seq !== expected) {
            const gap = (seq - expected) >>> 0;
            captureError(comTest,
                `Sequence gap — expected seq=${expected}, got seq=${seq}  (${gap} packet(s) lost)`,
                formatHex(buildPacket(expected)),
                `Next packet received had seq=${seq}\n\n` + formatHex(payload));
            if (stopOnError) { stopAllTests(); return; }
        }
    }

    if (!payloadOk) {
        const exp = buildExpectedPayload(seq);
        captureError(comTest,
            `Payload mismatch  seq=${seq}  expected ${exp.length}B  got ${payload.length}B`,
            formatHex(exp),
            formatHex(payload),
            exp, payload);
        if (stopOnError) { stopAllTests(); return; }
    }

    comTest.lastSeq = seq;
    updateTestUI('com');
};

comTest.parser.onSyncError = (snapshot) => {
    const label = comTest.lastSeq !== null
        ? `CRC / sync error — last good seq=${comTest.lastSeq}  (${snapshot.length} bytes in parser buffer)`
        : `CRC / sync error — no prior seq  (${snapshot.length} bytes in parser buffer)`;
    const nextSeq = comTest.lastSeq !== null
        ? (comTest.lastSeq + 1) >>> 0
        : (snapshot.length >= 4 ? new DataView(snapshot.buffer).getUint32(0, true) : null);
    const expectedRaw = nextSeq !== null ? buildPacket(nextSeq) : null;
    const expectedHex = expectedRaw ? formatHex(expectedRaw) : '— (no prior packet received)';
    captureError(comTest, label, expectedHex, formatHex(snapshot), expectedRaw, snapshot);
    if (stopOnError) stopAllTests();
};

bleTest.parser.onPacket = (seq, payload) => {
    const payloadOk = validatePayload(seq, payload);
    const now = performance.now();
    let latMs = 0;
    if (bleTest.pending.has(seq)) {
        latMs = now - bleTest.pending.get(seq);
        bleTest.pending.delete(seq);
    }
    bleTest.recv++;

    if (latMs > 0) updateLatency(bleTest, latMs);

    if (bleTest.lastSeq !== null) {
        const expected = (bleTest.lastSeq + 1) >>> 0;
        if (seq !== expected) {
            const gap = (seq - expected) >>> 0;
            captureError(bleTest,
                `Sequence gap — expected seq=${expected}, got seq=${seq}  (${gap} packet(s) lost)`,
                formatHex(buildPacket(expected)),
                `Next packet received had seq=${seq}\n\n` + formatHex(payload));
            if (stopOnError) { stopAllTests(); return; }
        }
    }

    if (!payloadOk) {
        const exp = buildExpectedPayload(seq);
        captureError(bleTest,
            `Payload mismatch  seq=${seq}  expected ${exp.length}B  got ${payload.length}B`,
            formatHex(exp),
            formatHex(payload),
            exp, payload);
        if (stopOnError) { stopAllTests(); return; }
    }

    bleTest.lastSeq = seq;
    updateTestUI('ble');
};

bleTest.parser.onSyncError = (snapshot) => {
    const label = bleTest.lastSeq !== null
        ? `CRC / sync error — last good seq=${bleTest.lastSeq}  (${snapshot.length} bytes in parser buffer)`
        : `CRC / sync error — no prior seq  (${snapshot.length} bytes in parser buffer)`;
    const nextSeq = bleTest.lastSeq !== null
        ? (bleTest.lastSeq + 1) >>> 0
        : (snapshot.length >= 4 ? new DataView(snapshot.buffer).getUint32(0, true) : null);
    const expectedRaw = nextSeq !== null ? buildPacket(nextSeq) : null;
    const expectedHex = expectedRaw ? formatHex(expectedRaw) : '— (no prior packet received)';
    captureError(bleTest, label, expectedHex, formatHex(snapshot), expectedRaw, snapshot);
    if (stopOnError) stopAllTests();
};

// ── Test commands ─────────────────────────────────────────────────────────────

async function startComTest() {
    if (comTest.running) return;
    comTestGeneration++;   // discard any stale handlers still pending in _notifyQueue
    resetTestState(comTest);
    comTest.running  = true;
    comTest.startMs  = Date.now();
    updateTestUI('com');
    startStatsTimer();

    let seq = 0;
    while (comTest.running) {
        const packet = buildPacket(seq >>> 0);
        comTest.pending.set(seq >>> 0, performance.now());
        purgePending(comTest.pending);

        try {
            await serial.send(packet);
        } catch (err) {
            captureError(comTest, `COM send error: ${err.message}`, '', '');
            stopComTest();
            break;
        }

        comTest.sent++;
        seq = (seq + 1) >>> 0;
        await yieldToEventLoop();
    }
}

// Maximum packets that may be in-flight (sent but not yet received at COM).
// Chrome can pipeline multiple ATT Write Request/Response exchanges within a
// single BLE connection event, so writeValueWithResponse alone does not limit
// throughput to one write per event.  Without a window limit the sender can
// reach hundreds of packets/sec while the COM receiver (especially with a delay)
// processes only a few, building a pipeline that no ATT-level backpressure can
// drain fast enough.  The window pauses the sender whenever it is more than
// BLE_TO_COM_WINDOW packets ahead of the receiver, matching the send rate to
// the receive rate regardless of delay.
const BLE_TO_COM_WINDOW = 16;

async function startBleTest() {
    if (bleTest.running) return;
    bleTestGeneration++;   // discard any stale serial.onData handlers from prior run
    resetTestState(bleTest);
    bleTest.running  = true;
    bleTest.startMs  = Date.now();
    updateTestUI('ble');
    startStatsTimer();

    let seq = 0;
    while (bleTest.running) {
        // Sliding-window flow control: pause until the receiver has caught up
        // enough that fewer than BLE_TO_COM_WINDOW packets are in-flight.
        while (bleTest.running && (bleTest.sent - bleTest.recv) >= BLE_TO_COM_WINDOW) {
            await sleep(5);
        }
        if (!bleTest.running) break;

        const packet = buildPacket(seq >>> 0);
        bleTest.pending.set(seq >>> 0, performance.now());
        purgePending(bleTest.pending);

        if (!await bleSendWithRetry(packet)) {
            captureError(bleTest, 'BLE send failed — ESP32 TX queue persistently full', '', '');
            stopBleTest();
            break;
        }

        bleTest.sent++;
        seq = (seq + 1) >>> 0;
        await yieldToEventLoop();
    }
}

function stopComTest() {
    comTest.running = false;
    updateTestUI('com');
}

function stopBleTest() {
    bleTest.running = false;
    updateTestUI('ble');
}

function stopAllTests() {
    stopComTest();
    stopBleTest();
}

// ── Stats timer ───────────────────────────────────────────────────────────────

function startStatsTimer() {
    if (statsInterval) return;
    statsInterval = setInterval(onStatsTick, 250);
}

function onStatsTick() {
    if (!comTest.running && !bleTest.running) {
        clearInterval(statsInterval);
        statsInterval = null;
        return;
    }
    tickStats(comTest);
    tickStats(bleTest);
    updateTestUI('com');
    updateTestUI('ble');
}

function tickStats(t) {
    if (!t.running) return;
    const newBytes   = t.recvBytes;
    t.bpsDisplay     = (newBytes - t.prevBytes) * 4;  // 250 ms → /sec
    t.prevBytes      = newBytes;
    const sec = (Date.now() - t.startMs) / 1000;
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = Math.floor(sec % 60);
    t.elapsed = `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function updateLatency(t, ms) {
    t.latCount++;
    t.latSum += ms;
    t.latAvg  = t.latSum / t.latCount;
    if (ms < t.latMin) t.latMin = ms;
    if (ms > t.latMax) t.latMax = ms;
}

function captureError(t, type, expected, received, expectedRaw = null, receivedRaw = null) {
    t.errors++;
    if (!t.hasError) {
        t.errorType        = type;
        t.errorExpected    = expected;
        t.errorReceived    = received;
        t.errorExpectedRaw = expectedRaw;
        t.errorReceivedRaw = receivedRaw;
        t.hasError         = true;
    }
    updateTestUI(t === comTest ? 'com' : 'ble');
}

function resetTestState(t) {
    t.sent = t.recv = t.errors = 0;
    t.bps = t.bpsDisplay = 0;
    t.latSum = t.latCount = t.latAvg = t.latMax = 0;
    t.latMin = Infinity;
    t.elapsed = '00:00:00';
    t.recvBytes = t.prevBytes = 0;
    t.hasError = false;
    t.errorType = t.errorExpected = t.errorReceived = '';
    t.errorExpectedRaw = t.errorReceivedRaw = null;
    t.pending.clear();
    t.lastSeq = null;
    t.parser.reset();
}

function purgePending(map) {
    const expiry = performance.now() - 10000;  // 10 s
    for (const [k, v] of map) if (v < expiry) map.delete(k);
}

function yieldToEventLoop() { return new Promise(r => setTimeout(r, 0)); }

/**
 * Send one BLE packet with automatic retry on ATT error.
 *
 * When the ESP32's UART TX queue is full (CTS-driven backpressure) it returns
 * an ATT Error Response instead of silently dropping.  writeValueWithResponse
 * rejects; we wait briefly and retry so the queue has time to drain.
 *
 * Returns true on success, false if all retries were exhausted.
 */
async function bleSendWithRetry(data) {
    const MAX_RETRIES = 10;
    for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        try {
            await ble.send(data);
            return true;
        } catch {
            if (attempt === MAX_RETRIES) return false;
            await sleep(20 * (attempt + 1));  // 20 ms, 40 ms, 60 ms … up to 200 ms
        }
    }
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function fmtLat(ms) { return ms === Infinity || ms === 0 ? '—' : ms.toFixed(1) + ' ms'; }

// ── UI bindings ───────────────────────────────────────────────────────────────

function $ (id) { return document.getElementById(id); }

// BLE profile selector
const BLE_PROFILE_HINTS = {
    nus:    'Nordic NUS (6E400001…) — BP+ Bridge',
    ble232: 'BLE232 (0003ABCD…)',
};
$('sel-ble-profile').addEventListener('change', (e) => {
    ble = e.target.value === 'ble232' ? bleBle232 : bleNus;
    $('ble-hint').textContent = BLE_PROFILE_HINTS[e.target.value] || '';
});

// BLE connect
$('btn-ble-connect').addEventListener('click', async () => {
    if (!ble.isAvailable) { alert('Web Bluetooth is not available in this browser.'); return; }
    try {
        $('ble-status').textContent = 'Connecting…';
        await ble.connect();
    } catch (err) {
        $('ble-status').textContent = 'Error: ' + err.message;
    }
});

$('btn-ble-disconnect').addEventListener('click', async () => {
    stopAllTests();     // halt send loops before disconnecting so no new writes
    await ble.disconnect();  // are submitted to the OS BLE driver after this point
});

// Serial connect
$('btn-serial-connect').addEventListener('click', async () => {
    if (!serial.isAvailable) {
        alert('Neither WebSerial nor WebUSB is available.\nTry Chrome on desktop (WebSerial) or Android (WebUSB with PL2303 adapter).');
        return;
    }
    try {
        $('serial-status').textContent = 'Connecting…';
        const opts = {
            baudRate:    parseInt($('sel-baud').value),
            dataBits:    parseInt($('sel-data').value),
            parity:      $('sel-parity').value.toLowerCase(),
            stopBits:    parseFloat($('sel-stop').value),
            flowControl: $('chk-flow').checked,
        };
        await serial.connect(opts);
    } catch (err) {
        $('serial-status').textContent = 'Error: ' + err.message;
    }
});

$('btn-serial-disconnect').addEventListener('click', async () => { await serial.disconnect(); });

// Test buttons
$('btn-com-ble-start').addEventListener('click', () => startComTest());
$('btn-com-ble-stop').addEventListener('click',  () => stopComTest());
$('btn-ble-com-start').addEventListener('click', () => startBleTest());
$('btn-ble-com-stop').addEventListener('click',  () => stopBleTest());
$('btn-stop-all').addEventListener('click',      () => stopAllTests());
$('btn-clear-stats').addEventListener('click',   () => {
    resetTestState(comTest);
    resetTestState(bleTest);
    updateTestUI('com');
    updateTestUI('ble');
});

// Stop-on-error toggle
$('chk-stop-on-error').addEventListener('change', (e) => { stopOnError = e.target.checked; });

// Delay controls — COM→BLE
$('chk-com-ble-delay').addEventListener('change', (e) => {
    sim.comToBle.enabled = e.target.checked;
    $('sld-com-ble-delay').disabled = !e.target.checked;
});
$('sld-com-ble-delay').addEventListener('input', (e) => {
    sim.comToBle.maxMs = parseInt(e.target.value);
    $('lbl-com-ble-delay').textContent = e.target.value + ' ms';
});

// Delay controls — BLE→COM
$('chk-ble-com-delay').addEventListener('change', (e) => {
    sim.bleToCom.enabled = e.target.checked;
    $('sld-ble-com-delay').disabled = !e.target.checked;
});
$('sld-ble-com-delay').addEventListener('input', (e) => {
    sim.bleToCom.maxMs = parseInt(e.target.value);
    $('lbl-ble-com-delay').textContent = e.target.value + ' ms';
});

// ── UI update ─────────────────────────────────────────────────────────────────

function updateConnectionUI() {
    const bleOk    = ble.isConnected;
    const serOk    = serial.isConnected;
    const bothOk   = bleOk && serOk;

    // BLE card
    $('ble-dot').className      = 'dot ' + (bleOk ? 'connected' : 'disconnected');
    $('ble-status').textContent = bleOk ? 'Connected  (' + (ble._device?.name || 'device') + ')' : 'Not connected';
    $('btn-ble-connect').hidden    = bleOk;
    $('btn-ble-disconnect').hidden = !bleOk;
    $('sel-ble-profile').disabled  = bleOk;

    // Serial card
    const apiName = serial.isWebSerial ? 'WebSerial' : 'WebUSB (PL2303)';
    $('serial-dot').className      = 'dot ' + (serOk ? 'connected' : 'disconnected');
    $('serial-status').textContent = serOk ? `Connected  (${apiName})` : 'Not connected';
    $('btn-serial-connect').hidden    = serOk;
    $('btn-serial-disconnect').hidden = !serOk;

    // Global status bar
    $('connected-dot').className      = 'dot ' + (bothOk ? 'connected' : 'disconnected');
    $('connected-status').textContent = bothOk ? 'Connected' : 'Not connected';

    // Enable/disable test start buttons
    $('btn-com-ble-start').disabled = !bothOk || comTest.running;
    $('btn-ble-com-start').disabled = !bothOk || bleTest.running;
    $('btn-stop-all').disabled      = !bothOk;

    // Show API hint if serial not yet supported
    $('serial-api-hint').hidden = serial.isAvailable;
}

function updateTestUI(dir) {
    const t     = dir === 'com' ? comTest : bleTest;
    const pfx   = dir === 'com' ? 'com-ble' : 'ble-com';

    $(`${pfx}-elapsed`).textContent = t.elapsed;
    $(pfx + '-running-dot').className = 'dot ' + (t.running ? 'running' : 'idle');
    $(pfx + '-spinner').style.display = t.running ? 'inline-block' : 'none';

    $(`${pfx}-sent`).textContent  = t.sent.toLocaleString();
    $(`${pfx}-recv`).textContent  = t.recv.toLocaleString();
    $(`${pfx}-errors`).textContent = t.errors.toLocaleString();
    $(`${pfx}-bps`).textContent   = t.bpsDisplay >= 1000
        ? (t.bpsDisplay / 1024).toFixed(1) + ' KB/s'
        : t.bpsDisplay.toFixed(0) + ' B/s';
    $(`${pfx}-lat-avg`).textContent = fmtLat(t.latAvg);
    $(`${pfx}-lat-min`).textContent = fmtLat(t.latMin);
    $(`${pfx}-lat-max`).textContent = fmtLat(t.latMax);

    // Start/stop button state
    $(`btn-${pfx}-start`).disabled = !ble.isConnected || !serial.isConnected || t.running;
    $(`btn-${pfx}-stop`).disabled  = !t.running;

    // Error panel
    const errPanel = $(`${pfx}-error`);
    if (t.hasError) {
        errPanel.hidden = false;
        $(`${pfx}-error-type`).textContent = t.errorType;
        // Use diff colouring when raw bytes are available for both sides
        if (t.errorExpectedRaw && t.errorReceivedRaw) {
            $(`${pfx}-error-expected`).innerHTML = formatHex(t.errorExpectedRaw);
            $(`${pfx}-error-received`).innerHTML = formatHexDiff(t.errorReceivedRaw, t.errorExpectedRaw);
        } else {
            $(`${pfx}-error-expected`).innerHTML = t.errorExpected || '—';
            $(`${pfx}-error-received`).innerHTML = t.errorReceived || '—';
        }
    } else {
        errPanel.hidden = true;
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────

updateConnectionUI();
updateTestUI('com');
updateTestUI('ble');
