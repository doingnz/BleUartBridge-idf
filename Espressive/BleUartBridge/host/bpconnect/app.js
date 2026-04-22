/*
 * app.js — BP+ Bridge management UI (Web Bluetooth).
 *
 * Talks to the BP+ Mgmt service (7E500001-...) on a BleUartBridge device:
 *   - AUTH          HMAC-SHA256 challenge/response
 *   - INFO          80-byte fixed struct (see main/ble_mgmt.c)
 *   - CFG_CONTROL   TLV GET / SET / COMMIT / ENUM / RESET
 *   - DFU_CONTROL   START / VERIFY / APPLY / ABORT / STATUS
 *   - DFU_DATA      u16 seq + payload
 *
 * See main/ble_mgmt.c and main/dfu.c for the authoritative wire layouts.
 */
'use strict';

// ── UUIDs ────────────────────────────────────────────────────────────────────

const NUS_SERVICE_UUID  = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_SERVICE_UUID = '7e500001-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_AUTH_UUID    = '7e500002-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_INFO_UUID    = '7e500003-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_CFG_UUID     = '7e500004-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_DFU_CTL_UUID = '7e500005-b5a3-f393-e0a9-e50e24dcca9e';
const MGMT_DFU_DAT_UUID = '7e500006-b5a3-f393-e0a9-e50e24dcca9e';

// ── CFG opcodes (mirror of main/ble_mgmt.c) ──────────────────────────────────
const CFG_OP_GET=0x01, CFG_OP_SET=0x02, CFG_OP_COMMIT=0x03, CFG_OP_RESET=0x04, CFG_OP_ENUM=0x05;
const CFG_RSP_GET=0x81, CFG_RSP_SET=0x82, CFG_RSP_COMMIT=0x83, CFG_RSP_ENUM=0x85, CFG_RSP_ENUM_END=0x86;

// ── DFU opcodes (mirror of main/dfu.c) ──────────────────────────────────────
const DFU_OP_START=0x01, DFU_OP_VERIFY=0x03, DFU_OP_APPLY=0x04, DFU_OP_ABORT=0x05, DFU_OP_STATUS=0x06;
const DFU_RSP_START=0x81, DFU_RSP_CHUNK=0x82, DFU_RSP_VERIFY=0x83;
const DFU_RSP_APPLY=0x84, DFU_EVT_ABORT=0x85, DFU_RSP_STATUS=0x86;

const CFG_TYPE_NAMES = { 0:'u8', 1:'i8', 2:'u16', 3:'u32', 4:'str', 5:'trigger' };

const TLV_META = {
    0x01: { name: 'uart_baud',          type: 'u32',  min: 300,  max: 3000000 },
    0x02: { name: 'uart_flowctrl',      type: 'bool' },
    0x03: { name: 'uart_databits',      type: 'u8',   min: 5, max: 8 },
    0x04: { name: 'uart_parity',        type: 'enum', options: ['None','Even','Odd'] },
    0x05: { name: 'uart_stopbits',      type: 'enum', options: { 1: '1', 2: '2' } },
    0x06: { name: 'name_suffix',        type: 'str',  maxLen: 8 },
    0x07: { name: 'ble_tx_power_dbm',   type: 'i8',   min: -27, max: 9 },
    0x08: { name: 'hexdump_default',    type: 'bool' },
    0x09: { name: 'dfu_enabled',        type: 'bool' },
    0x0A: { name: 'auth_required',      type: 'bool' },
    0x0B: { name: 'factory_reset',      type: 'trigger' },
    0x0C: { name: 'ble_adv_interval_ms',type: 'u16',  min: 20, max: 10240 },
    0x0D: { name: 'ble_preferred_mtu',  type: 'u16',  min: 23, max: 512 },
};

// ── State ────────────────────────────────────────────────────────────────────

const S = {
    device: null,
    server: null,
    chars: { auth: null, info: null, cfg: null, dfuCtl: null, dfuDat: null },
    info: null,                  // parsed INFO struct
    authed: false,
    cfgPending: null,            // { resolve, reject, type }
    cfgValues: {},               // id -> { value, dirty }
    cfgEnumBuffer: null,
    dfu: null,                   // { file, sig, sha, version, offset, seq, outstanding, window, chunkSize, aborted, resolve }
};

// ── Logging ──────────────────────────────────────────────────────────────────

function log(...args) {
    const s = args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ');
    const el = document.getElementById('log');
    el.textContent += `[${new Date().toISOString().slice(11, 19)}] ${s}\n`;
    el.scrollTop = el.scrollHeight;
    console.log(...args);
}

function setPill(id, text, kind) {
    const el = document.getElementById(id);
    el.textContent = text;
    el.className = 'pill' + (kind ? ' ' + kind : '');
}

// ── Utilities ────────────────────────────────────────────────────────────────

function hexToBytes(hex) {
    hex = hex.replace(/\s+/g, '').toLowerCase();
    if (hex.length % 2) throw new Error('odd-length hex');
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; ++i) out[i] = parseInt(hex.substr(i*2, 2), 16);
    return out;
}
function bytesToHex(u8) { return Array.from(u8, b => b.toString(16).padStart(2,'0')).join(''); }
function macToStr(u8) {
    // INFO.mac is in radio order (LSB first) — reverse for the conventional
    // colon-separated MSB-first display.
    return Array.from(u8).reverse().map(b => b.toString(16).padStart(2,'0')).join(':');
}

async function hmacSha256(keyBytes, msgBytes) {
    const key = await crypto.subtle.importKey(
        'raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const sig = await crypto.subtle.sign('HMAC', key, msgBytes);
    return new Uint8Array(sig);
}

async function sha256(bytes) {
    const d = await crypto.subtle.digest('SHA-256', bytes);
    return new Uint8Array(d);
}

// ── Connect / disconnect ─────────────────────────────────────────────────────

async function connect() {
    try {
        S.device = await navigator.bluetooth.requestDevice({
            filters: [{ namePrefix: 'BP+ Bridge' }, { services: [NUS_SERVICE_UUID] }],
            optionalServices: [MGMT_SERVICE_UUID, NUS_SERVICE_UUID],
        });
        S.device.addEventListener('gattserverdisconnected', onDisconnect);
        log('chosen:', S.device.name);

        S.server = await S.device.gatt.connect();
        const svc = await S.server.getPrimaryService(MGMT_SERVICE_UUID);
        S.chars.auth   = await svc.getCharacteristic(MGMT_AUTH_UUID);
        S.chars.info   = await svc.getCharacteristic(MGMT_INFO_UUID);
        S.chars.cfg    = await svc.getCharacteristic(MGMT_CFG_UUID);
        S.chars.dfuCtl = await svc.getCharacteristic(MGMT_DFU_CTL_UUID);
        S.chars.dfuDat = await svc.getCharacteristic(MGMT_DFU_DAT_UUID);

        await S.chars.auth.startNotifications();
        S.chars.auth.addEventListener('characteristicvaluechanged', onAuthNotify);
        await S.chars.cfg.startNotifications();
        S.chars.cfg.addEventListener('characteristicvaluechanged', onCfgNotify);
        await S.chars.dfuCtl.startNotifications();
        S.chars.dfuCtl.addEventListener('characteristicvaluechanged', onDfuNotify);

        await readInfo();

        setPill('conn-pill', 'connected', 'ok');
        document.getElementById('btn-connect').disabled    = true;
        document.getElementById('btn-disconnect').disabled = false;
        document.getElementById('info-section').hidden = false;
        document.getElementById('auth-section').hidden = false;
        document.getElementById('cfg-section').hidden  = false;
        document.getElementById('dfu-section').hidden  = false;

        // Pre-fill master key from localStorage if previously saved.
        const stored = localStorage.getItem('bp_master_key');
        if (stored) document.getElementById('master-key').value = stored;
    } catch (e) {
        log('connect failed:', e.message || e);
    }
}

function onDisconnect() {
    log('disconnected');
    setPill('conn-pill', 'disconnected');
    setPill('auth-pill', 'idle');
    setPill('cfg-pill',  ' ');
    setPill('dfu-pill',  'idle');
    S.authed = false;
    S.server = null;
    document.getElementById('btn-connect').disabled    = false;
    document.getElementById('btn-disconnect').disabled = true;
}

async function disconnect() {
    if (S.device && S.device.gatt.connected) S.device.gatt.disconnect();
}

// ── INFO ─────────────────────────────────────────────────────────────────────

async function readInfo() {
    const dv = await S.chars.info.readValue();
    // Packed struct: magic(2) + version(1) + target(1) + fw_version(32) +
    //                elf_sha8(8) + running_addr(4) + running_size(4) +
    //                next_addr(4) + next_size(4) + preferred_mtu(2) +
    //                auth_required(1) + dfu_enabled(1) + pubkey_fp(16) +
    //                mac(6) + reserved(2) = 88 bytes.
    if (dv.byteLength < 88) throw new Error('INFO too short: ' + dv.byteLength);
    const u8 = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);

    const magic   = dv.getUint16(0, true);
    const version = dv.getUint8(2);
    const target  = dv.getUint8(3);
    const fw      = new TextDecoder().decode(u8.subarray(4, 36)).replace(/\0.*$/, '');
    const sha8    = u8.subarray(36, 44);
    const runAddr = dv.getUint32(44, true);
    const runSize = dv.getUint32(48, true);
    const nxtAddr = dv.getUint32(52, true);
    const nxtSize = dv.getUint32(56, true);
    const prefMtu = dv.getUint16(60, true);
    const authReq = dv.getUint8(62);
    const dfuEn   = dv.getUint8(63);
    const fp16    = u8.subarray(64, 80);
    const mac     = u8.subarray(80, 86);

    S.info = {
        magic, version, target, fw, sha8, runAddr, runSize, nxtAddr, nxtSize,
        prefMtu, authReq, dfuEn, fp: fp16, mac,
    };

    const rows = [
        ['Name',              S.device.name],
        ['MAC (radio order)', bytesToHex(S.info.mac)],
        ['MAC (display)',     macToStr(S.info.mac)],
        ['Target',            S.info.target === 1 ? 'esp32s3' : 'esp32'],
        ['Firmware',          S.info.fw || '(empty)'],
        ['ELF SHA-256 (8B)',  bytesToHex(S.info.sha8)],
        ['Running partition', `0x${S.info.runAddr.toString(16)} / ${(S.info.runSize/1024)|0} KB`],
        ['Next OTA slot',     `0x${S.info.nxtAddr.toString(16)} / ${(S.info.nxtSize/1024)|0} KB`],
        ['Preferred MTU',     S.info.prefMtu],
        ['Auth required',     S.info.authReq ? 'yes' : 'no'],
        ['DFU enabled',       S.info.dfuEn  ? 'yes' : 'no'],
        ['Pubkey FP (16B)',   bytesToHex(S.info.fp)],
    ];
    const tbl = document.getElementById('info-table');
    tbl.innerHTML = rows.map(([k,v]) => `<tr><th>${k}</th><td><code>${v}</code></td></tr>`).join('');
}

// ── AUTH ─────────────────────────────────────────────────────────────────────

let authResolver = null;

function onAuthNotify(ev) {
    const dv = ev.target.value;
    const op = dv.getUint8(0);
    if (op === 0x81) {
        const challenge = new Uint8Array(dv.buffer, dv.byteOffset + 1, 16);
        if (authResolver) authResolver({ kind: 'challenge', challenge });
    } else if (op === 0x82) {
        const status = dv.getUint8(1);
        if (authResolver) authResolver({ kind: 'status', status });
    }
}

function waitForAuthNotify() {
    return new Promise(resolve => { authResolver = resolve; });
}

async function authenticate() {
    if (!S.chars.auth) return;
    const rawKey = document.getElementById('master-key').value.trim();
    if (rawKey.length !== 64) { log('master key must be 64 hex chars'); return; }
    localStorage.setItem('bp_master_key', rawKey);
    const master = hexToBytes(rawKey);

    setPill('auth-pill', 'deriving', 'warn');
    const deviceSecret = await hmacSha256(master, S.info.mac);

    // BEGIN
    setPill('auth-pill', 'challenging', 'warn');
    const begin = Promise.resolve().then(() => S.chars.auth.writeValue(new Uint8Array([0x01])));
    const chalP = waitForAuthNotify();
    await begin;
    const chalMsg = await chalP;
    if (chalMsg.kind !== 'challenge') { log('unexpected AUTH notify'); return; }

    // RESPONSE
    const response = await hmacSha256(deviceSecret, chalMsg.challenge);
    const frame = new Uint8Array(1 + 32);
    frame[0] = 0x02;
    frame.set(response, 1);
    const sendP = S.chars.auth.writeValue(frame);
    const statP = waitForAuthNotify();
    await sendP;
    const stat = await statP;
    if (stat.kind !== 'status') { log('unexpected AUTH status'); return; }

    if (stat.status === 0) {
        S.authed = true;
        setPill('auth-pill', 'authenticated', 'ok');
        log('authenticated');
    } else if (stat.status === 2) {
        setPill('auth-pill', 'locked 30 s', 'bad');
        log('auth locked out');
    } else {
        setPill('auth-pill', 'failed', 'bad');
        log('auth failed');
    }
}

function forgetKey() {
    localStorage.removeItem('bp_master_key');
    document.getElementById('master-key').value = '';
    log('cleared master key from localStorage');
}

// ── CFG ──────────────────────────────────────────────────────────────────────

function decodeTlvValue(id, bytes) {
    const meta = TLV_META[id];
    if (!meta) return `0x${bytesToHex(bytes)}`;
    switch (meta.type) {
    case 'bool': return bytes[0] ? 1 : 0;
    case 'u8':   return bytes[0];
    case 'i8':   return bytes[0] < 128 ? bytes[0] : bytes[0] - 256;
    case 'u16':  return bytes[0] | (bytes[1] << 8);
    case 'u32':  return (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
    case 'str':  return new TextDecoder().decode(bytes);
    case 'enum':
        if (Array.isArray(meta.options)) return meta.options[bytes[0]] || bytes[0];
        return meta.options[bytes[0]] || String(bytes[0]);
    default:     return bytes[0];
    }
}

function encodeTlvValue(id, value) {
    const meta = TLV_META[id];
    switch (meta.type) {
    case 'bool': return Uint8Array.of(value ? 1 : 0);
    case 'u8':   return Uint8Array.of(value & 0xFF);
    case 'i8':   return Uint8Array.of((value + 256) & 0xFF);
    case 'u16':  return Uint8Array.of(value & 0xFF, (value >> 8) & 0xFF);
    case 'u32':  return Uint8Array.of(value & 0xFF, (value >> 8) & 0xFF,
                                      (value >> 16) & 0xFF, (value >> 24) & 0xFF);
    case 'str':  return new TextEncoder().encode(value || '');
    case 'enum': {
        if (Array.isArray(meta.options)) {
            const i = meta.options.indexOf(value);
            return Uint8Array.of(i < 0 ? 0 : i);
        }
        const entries = Object.entries(meta.options);
        for (const [k, label] of entries) if (label === value) return Uint8Array.of(+k);
        return Uint8Array.of(+entries[0][0]);
    }
    case 'trigger': return Uint8Array.of(1);
    }
}

function onCfgNotify(ev) {
    const dv = ev.target.value;
    const op = dv.getUint8(0);

    if (op === CFG_RSP_GET || op === CFG_RSP_SET || op === CFG_RSP_COMMIT) {
        if (S.cfgPending) S.cfgPending.resolve({ op, dv });
        return;
    }
    if (op === CFG_RSP_ENUM) {
        const id = dv.getUint8(1);
        const type = dv.getUint8(2);
        const live = dv.getUint8(3);
        const len  = dv.getUint8(4);
        const val  = new Uint8Array(dv.buffer, dv.byteOffset + 5, len);
        S.cfgEnumBuffer.push({ id, type, live, value: decodeTlvValue(id, val) });
        return;
    }
    if (op === CFG_RSP_ENUM_END) {
        if (S.cfgPending) S.cfgPending.resolve({ op });
        return;
    }
}

// IMPORTANT: call awaitCfgResp() BEFORE issuing the matching writeValue().
// NimBLE often delivers the response notifications before Chrome resolves
// the writeValue Promise — if the waiter is registered after the write
// resolves, the matching notification arrives when S.cfgPending is still
// null and is silently dropped, producing a spurious 5 s timeout.
function awaitCfgResp() {
    return new Promise((resolve, reject) => {
        S.cfgPending = { resolve, reject };
        setTimeout(() => {
            if (S.cfgPending) {
                const p = S.cfgPending;
                S.cfgPending = null;
                p.reject(new Error('timeout'));
            }
        }, 5000);
    }).finally(() => { S.cfgPending = null; });
}

async function cfgReadAll() {
    S.cfgEnumBuffer = [];
    const waitP = awaitCfgResp();
    await S.chars.cfg.writeValue(Uint8Array.of(CFG_OP_ENUM));
    await waitP;
    renderCfgTable(S.cfgEnumBuffer);
    setPill('cfg-pill', `loaded ${S.cfgEnumBuffer.length} values`, 'ok');
}

function renderCfgTable(entries) {
    S.cfgValues = {};
    const rows = entries.map(e => {
        S.cfgValues[e.id] = { value: e.value, dirty: false };
        const meta = TLV_META[e.id] || { name: `tlv_0x${e.id.toString(16)}`, type: 'u8' };
        let editor;
        switch (meta.type) {
        case 'bool':
            editor = `<input type="checkbox" data-id="${e.id}" ${e.value ? 'checked' : ''}>`;
            break;
        case 'enum': {
            const opts = Array.isArray(meta.options)
                ? meta.options.map((o,i) => `<option ${o===e.value?'selected':''}>${o}</option>`).join('')
                : Object.entries(meta.options).map(([k,v]) => `<option ${v===String(e.value)?'selected':''}>${v}</option>`).join('');
            editor = `<select data-id="${e.id}">${opts}</select>`;
            break;
        }
        case 'str':
            editor = `<input type="text" data-id="${e.id}" maxlength="${meta.maxLen || 8}" value="${e.value}">`;
            break;
        case 'trigger':
            editor = `<em>write 1 to trigger</em>`;
            break;
        default:
            editor = `<input type="number" data-id="${e.id}" value="${e.value}"` +
                    (meta.min !== undefined ? ` min="${meta.min}"` : '') +
                    (meta.max !== undefined ? ` max="${meta.max}"` : '') + '>';
        }
        return `<tr>
            <td><code>0x${e.id.toString(16).padStart(2,'0')}</code></td>
            <td>${meta.name}</td>
            <td>${meta.type}</td>
            <td>${e.live ? '<span class="pill ok">live</span>' : '<span class="pill">reboot</span>'}</td>
            <td>${editor}</td>
        </tr>`;
    });
    document.getElementById('cfg-table').innerHTML =
        `<tr><th>ID</th><th>Name</th><th>Type</th><th>Apply</th><th>Value</th></tr>` + rows.join('');

    document.querySelectorAll('#cfg-table [data-id]').forEach(el => {
        el.addEventListener('change', () => {
            const id = +el.dataset.id;
            const meta = TLV_META[id];
            let v;
            if (meta.type === 'bool') v = el.checked ? 1 : 0;
            else if (el.tagName === 'SELECT') v = el.value;
            else if (meta.type === 'str') v = el.value;
            else v = Number(el.value);
            S.cfgValues[id].value = v;
            S.cfgValues[id].dirty = true;
            document.getElementById('btn-cfg-commit').disabled = false;
        });
    });
}

async function cfgCommit() {
    const dirty = Object.entries(S.cfgValues).filter(([id, v]) => v.dirty);
    if (dirty.length === 0) return;

    for (const [idStr, slot] of dirty) {
        const id = +idStr;
        const raw = encodeTlvValue(id, slot.value);
        const frame = new Uint8Array(3 + raw.length);
        frame[0] = CFG_OP_SET;
        frame[1] = id;
        frame[2] = raw.length;
        frame.set(raw, 3);
        const waitP = awaitCfgResp();
        await S.chars.cfg.writeValue(frame);
        const resp = await waitP;
        const status = resp.dv.getUint8(2);
        if (status !== 0) {
            setPill('cfg-pill', `SET 0x${id.toString(16)} failed (status ${status})`, 'bad');
            return;
        }
        slot.dirty = false;
    }

    const waitP = awaitCfgResp();
    await S.chars.cfg.writeValue(Uint8Array.of(CFG_OP_COMMIT));
    const resp = await waitP;
    const status = resp.dv.getUint8(1);
    if (status === 0) {
        setPill('cfg-pill', 'committed', 'ok');
        document.getElementById('btn-cfg-commit').disabled = true;
    } else {
        setPill('cfg-pill', `COMMIT failed (status ${status})`, 'bad');
    }
}

async function cfgFactoryReset() {
    if (!confirm('Factory-reset the device? This will erase all settings and reboot.')) return;
    try { await S.chars.cfg.writeValue(Uint8Array.of(CFG_OP_RESET)); }
    catch (e) { /* device will reboot before ACK */ }
    log('RESET written — device should reboot');
}

// ── DFU ──────────────────────────────────────────────────────────────────────

let dfuResolver = null;

function onDfuNotify(ev) {
    const dv = ev.target.value;
    const op = dv.getUint8(0);
    if (dfuResolver) dfuResolver({ op, dv });
}

function waitForDfuNotify() {
    return new Promise((resolve, reject) => {
        dfuResolver = resolve;
        setTimeout(() => { if (dfuResolver) { dfuResolver = null; reject(new Error('DFU notify timeout')); } }, 30000);
    }).finally(() => { dfuResolver = null; });
}

async function dfuReadFiles() {
    const binEl = document.getElementById('file-bin');
    const sigEl = document.getElementById('file-sig');
    if (!binEl.files[0] || !sigEl.files[0]) throw new Error('choose both .bin and .sig');
    const bin = new Uint8Array(await binEl.files[0].arrayBuffer());
    const sig = new Uint8Array(await sigEl.files[0].arrayBuffer());
    if (sig.length !== 64) throw new Error('signature must be 64 bytes raw r||s');
    const sha = await sha256(bin);
    document.getElementById('sha-display').textContent = 'SHA-256: ' + bytesToHex(sha);
    return { bin, sig, sha };
}

async function dfuFlash() {
    let bin, sig, sha;
    try { ({ bin, sig, sha } = await dfuReadFiles()); }
    catch (e) { log('DFU:', e.message); return; }

    const version = Number(document.getElementById('fw-version').value) >>> 0;

    const progressEl = document.getElementById('dfu-progress');
    progressEl.value = 0;
    progressEl.max   = bin.length;
    document.getElementById('dfu-status').textContent = '';

    setPill('dfu-pill', 'starting', 'warn');
    document.getElementById('btn-flash').disabled     = true;
    document.getElementById('btn-dfu-abort').disabled = false;

    try {
        // START message = 1 (op) + 4 (size) + 32 (sha) + 4 (version) + 64 (sig) = 105 bytes
        const start = new Uint8Array(1 + 4 + 32 + 4 + 64);
        start[0] = DFU_OP_START;
        new DataView(start.buffer).setUint32(1, bin.length, true);
        start.set(sha, 5);
        new DataView(start.buffer).setUint32(37, version, true);
        start.set(sig, 41);

        const startWait = waitForDfuNotify();
        await S.chars.dfuCtl.writeValue(start);
        const startRsp = await startWait;
        if (startRsp.op !== DFU_RSP_START) throw new Error('expected START_RSP');
        const status = startRsp.dv.getUint8(1);
        if (status !== 0) throw new Error(`START rejected, status=${status}`);
        const maxChunk = startRsp.dv.getUint16(2, true);
        const window   = startRsp.dv.getUint16(4, true);
        log(`START ok: maxChunk=${maxChunk}  window=${window}`);

        setPill('dfu-pill', 'uploading', 'warn');

        // Stream chunks with a sliding window.  We do not use the chunk-ack
        // notify for individual ordering (the device acks linearly); we just
        // count outstanding writeWithoutResponse to cap our rate.
        S.dfu = { aborted: false, outstanding: 0, window, maxChunk, bin, progressEl };

        // Listen for CHUNK_ACK / ABORT_EVT in a side handler.
        const chunkListener = (ev) => {
            const dv = ev.target.value;
            const op = dv.getUint8(0);
            if (op === DFU_RSP_CHUNK) {
                S.dfu.outstanding--;
                const received = dv.getUint32(3, true);
                progressEl.value = received;
                document.getElementById('dfu-status').textContent =
                    `${received.toLocaleString()} / ${bin.length.toLocaleString()} B`;
            } else if (op === DFU_EVT_ABORT) {
                S.dfu.aborted = true;
                const reason = dv.getUint8(1);
                log(`device aborted DFU, reason=${reason}`);
            }
        };
        S.chars.dfuCtl.addEventListener('characteristicvaluechanged', chunkListener);

        try {
            let offset = 0, seq = 0;
            while (offset < bin.length) {
                if (S.dfu.aborted) throw new Error('device aborted');

                // Respect window
                while (S.dfu.outstanding >= window) {
                    await new Promise(r => setTimeout(r, 5));
                    if (S.dfu.aborted) throw new Error('device aborted');
                }

                const len = Math.min(maxChunk, bin.length - offset);
                const frame = new Uint8Array(2 + len);
                frame[0] = seq & 0xFF;
                frame[1] = (seq >> 8) & 0xFF;
                frame.set(bin.subarray(offset, offset + len), 2);

                S.dfu.outstanding++;
                await S.chars.dfuDat.writeValueWithoutResponse(frame);

                offset += len;
                seq++;
            }

            // Drain remaining acks
            const drainStart = Date.now();
            while (S.dfu.outstanding > 0 && !S.dfu.aborted && (Date.now() - drainStart) < 30000) {
                await new Promise(r => setTimeout(r, 10));
            }
            if (S.dfu.aborted) throw new Error('device aborted during drain');
        } finally {
            S.chars.dfuCtl.removeEventListener('characteristicvaluechanged', chunkListener);
        }

        // VERIFY
        setPill('dfu-pill', 'verifying', 'warn');
        const verifyWait = waitForDfuNotify();
        await S.chars.dfuCtl.writeValue(Uint8Array.of(DFU_OP_VERIFY));
        const verifyRsp = await verifyWait;
        if (verifyRsp.op !== DFU_RSP_VERIFY) throw new Error('expected VERIFY_RSP');
        const vstatus = verifyRsp.dv.getUint8(1);
        if (vstatus !== 0) throw new Error(`VERIFY rejected, status=${vstatus}`);
        log('VERIFY ok');

        // APPLY
        setPill('dfu-pill', 'applying', 'warn');
        const applyWait = waitForDfuNotify();
        await S.chars.dfuCtl.writeValue(Uint8Array.of(DFU_OP_APPLY));
        const applyRsp = await applyWait;
        if (applyRsp.op !== DFU_RSP_APPLY) throw new Error('expected APPLY_RSP');
        const astatus = applyRsp.dv.getUint8(1);
        if (astatus !== 0) throw new Error(`APPLY rejected, status=${astatus}`);
        log('APPLY ok — device will reboot');
        setPill('dfu-pill', 'rebooting', 'ok');
    } catch (e) {
        log('DFU failed:', e.message || e);
        setPill('dfu-pill', 'failed: ' + (e.message || e), 'bad');
        try { await S.chars.dfuCtl.writeValue(Uint8Array.of(DFU_OP_ABORT)); } catch {}
    } finally {
        document.getElementById('btn-flash').disabled     = false;
        document.getElementById('btn-dfu-abort').disabled = true;
    }
}

async function dfuAbort() {
    if (S.dfu) S.dfu.aborted = true;
    try { await S.chars.dfuCtl.writeValue(Uint8Array.of(DFU_OP_ABORT)); } catch {}
    setPill('dfu-pill', 'aborted', 'bad');
}

// ── Wire up ──────────────────────────────────────────────────────────────────

document.getElementById('btn-connect').addEventListener('click', connect);
document.getElementById('btn-disconnect').addEventListener('click', disconnect);
document.getElementById('btn-authenticate').addEventListener('click', authenticate);
document.getElementById('btn-forget-key').addEventListener('click', forgetKey);
document.getElementById('btn-cfg-read').addEventListener('click',   () => cfgReadAll().catch(e => log('cfg read:', e.message)));
document.getElementById('btn-cfg-commit').addEventListener('click', () => cfgCommit().catch(e => log('cfg commit:', e.message)));
document.getElementById('btn-cfg-reset').addEventListener('click',  cfgFactoryReset);
document.getElementById('btn-flash').addEventListener('click',      dfuFlash);
document.getElementById('btn-dfu-abort').addEventListener('click',  dfuAbort);

document.getElementById('file-bin').addEventListener('change', async () => {
    document.getElementById('btn-flash').disabled =
        !(document.getElementById('file-bin').files[0] &&
          document.getElementById('file-sig').files[0]);
    if (document.getElementById('file-bin').files[0]) {
        const bytes = new Uint8Array(await document.getElementById('file-bin').files[0].arrayBuffer());
        const s = await sha256(bytes);
        document.getElementById('sha-display').textContent = 'SHA-256: ' + bytesToHex(s);
    }
});
document.getElementById('file-sig').addEventListener('change', () => {
    document.getElementById('btn-flash').disabled =
        !(document.getElementById('file-bin').files[0] &&
          document.getElementById('file-sig').files[0]);
});

if (!navigator.bluetooth) {
    log('Web Bluetooth not available in this browser — use Chrome or Edge.');
    document.getElementById('btn-connect').disabled = true;
}
