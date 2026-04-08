/**
 * BleBle232Client :  Web Bluetooth client for the USConverters BLE232
 * (ESP32 version of Cypress CY8C4128 BLE RS-232 adapter, "BLE232").
 *
 * Data characteristics (verified by GATT inspection):
 *   NOTIFY  (device→app, Notify): 00031201-0000-1000-8000-00805F9B0130
 *   RxData  (app→device, Write):  00031202-0000-1000-8000-00805F9B0130
 *
 * Service UUID:
 *   Confirmed by GATT inspection to be the same as the S2B5232I adapter:
 *   0003ABCD-0000-1000-8000-00805F9B0131.  This is the first entry in
 *   SERVICE_CANDIDATES.  Additional candidates are kept as fallbacks in
 *   case firmware variants use a different service UUID.
 */
class BleBle232Client {
    // ── Characteristic UUIDs (verified by GATT inspection) ───────────────────

    static NOTIFY_UUID = '00031201-0000-1000-8000-00805f9b0130'; // device → app (Notify)
    static RXDATA_UUID = '00031202-0000-1000-8000-00805f9b0130'; // app → device (Write)

    static MAX_PAYLOAD = 20;

    // ── Service UUID candidates ───────────────────────────────────────────────
    // Confirmed service UUID shared with the S2B5232I adapter.
    // Additional candidates are kept as fallbacks for possible firmware variants.

    static SERVICE_CANDIDATES = [
        '0003abcd-0000-1000-8000-00805f9b0131', // confirmed
        '0003abcd-0000-1000-8000-00805f9b0130', // variant
    ];

    // ── Constructor ───────────────────────────────────────────────────────────

    constructor() {
        this._device      = null;
        this._writeChar   = null;
        this._connected   = false;
        this._notifyQueue = Promise.resolve();

        /** @type {function(Uint8Array): Promise<void>|void} */
        this.onData       = null;
        /** @type {function(): void} */
        this.onConnect    = null;
        /** @type {function(): void} */
        this.onDisconnect = null;
    }

    get isConnected() { return this._connected; }
    get isAvailable()  { return 'bluetooth' in navigator; }

    // ── Public API ────────────────────────────────────────────────────────────

    /**
     * Open the browser BLE picker filtered to BLE232 devices, connect to
     * GATT, locate the UART service, and subscribe to NOTIFY notifications.
     */
    async connect() {
        this._device = await navigator.bluetooth.requestDevice({
            filters: [
                { namePrefix: 'BLE232'   },
            ],
            // All service UUID candidates (including the confirmed service UUID).
            // Chrome only exposes services whose UUIDs appear in this list,
            // so every candidate must be declared here to allow GATT access.
            optionalServices: [
                ...BleBle232Client.SERVICE_CANDIDATES,
                BleBle232Client.NOTIFY_UUID,
                BleBle232Client.RXDATA_UUID,
            ],
        });

        this._device.addEventListener('gattserverdisconnected', () => {
            this._connected = false;
            this._writeChar = null;
            if (this.onDisconnect) this.onDisconnect();
        });

        const server = await this._device.gatt.connect();
        const { notifyChar, writeChar } = await this._findChars(server);

        this._writeChar = writeChar;

        await notifyChar.startNotifications();
        notifyChar.addEventListener('characteristicvaluechanged', (e) => {
            const data = new Uint8Array(e.target.value.buffer);
            this._notifyQueue = this._notifyQueue.then(
                () => this.onData ? this.onData(data) : undefined
            );
        });

        this._connected = true;
        if (this.onConnect) this.onConnect();
    }

    async disconnect() {
        this._notifyQueue = Promise.resolve();
        if (this._device?.gatt.connected) {
            this._device.gatt.disconnect();
        }
        this._connected = false;
        this._writeChar = null;
    }

    /**
     * Write data to the BLE232 RxData characteristic.
     * Chunks to MAX_PAYLOAD bytes.
     * Uses writeValueWithResponse for ATT-level backpressure.
     *
     * @param {Uint8Array} data
     */
    async send(data) {
        if (!this._writeChar) throw new Error('BLE232 not connected');
        const max = BleBle232Client.MAX_PAYLOAD;
        for (let offset = 0; offset < data.length; offset += max) {
            const chunk = data.subarray(offset, offset + max);
            if (this._writeChar.writeValueWithResponse) {
                await this._writeChar.writeValueWithResponse(chunk);
            } else {
                await this._writeChar.writeValue(chunk);   // older browsers
            }
        }
    }

    // ── Private ───────────────────────────────────────────────────────────────

    /**
     * Locate the GATT service that owns the BLE232 UART characteristics.
     *
     * Strategy:
     *  1. Try each SERVICE_CANDIDATES UUID directly via getPrimaryService().
     *  2. Fall back to walking every service returned by getPrimaryServices()
     *     (Chrome returns services whose UUIDs are in the optionalServices set,
     *     which includes both the candidates and the characteristic UUIDs).
     *
     * @param {BluetoothRemoteGATTServer} server
     * @returns {{ notifyChar, writeChar }}
     */
    async _findChars(server) {
        // Pass 1: try each documented candidate service UUID directly.
        for (const svcUuid of BleBle232Client.SERVICE_CANDIDATES) {
            try {
                const svc        = await server.getPrimaryService(svcUuid);
                const notifyChar = await svc.getCharacteristic(BleBle232Client.NOTIFY_UUID);
                const writeChar  = await svc.getCharacteristic(BleBle232Client.RXDATA_UUID);
                return { notifyChar, writeChar };
            } catch {
                // This candidate UUID is not the service UUID — try the next one.
            }
        }

        // Pass 2: enumerate all services Chrome allows us to see (those whose UUIDs
        // are in optionalServices, which also includes our characteristic UUIDs).
        let services;
        try {
            services = await server.getPrimaryServices();
        } catch {
            services = [];
        }
        for (const svc of services) {
            try {
                const notifyChar = await svc.getCharacteristic(BleBle232Client.NOTIFY_UUID);
                const writeChar  = await svc.getCharacteristic(BleBle232Client.RXDATA_UUID);
                return { notifyChar, writeChar };
            } catch {
                // This service does not contain both BLE232 characteristics.
            }
        }

        throw new Error(
            'BLE232 UART characteristics not found on this device.\n' +
            'Scan its GATT services with nRF Connect ' +
            'and add the service UUID to BleBle232Client.SERVICE_CANDIDATES in ble-ble232.js.'
        );
    }
}
