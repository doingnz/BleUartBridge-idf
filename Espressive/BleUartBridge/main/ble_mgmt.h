/**
 * ble_mgmt.h — "BP+ Mgmt" GATT service (auth, info, config, DFU).
 *
 * Registered as a second primary service alongside NUS.  NUS remains in the
 * ADV PDU; this service is discovered after connection.  Web Bluetooth
 * clients must list the mgmt service UUID in optionalServices.
 *
 * Service UUID:  7E500001-B5A3-F393-E0A9-E50E24DCCA9E
 *   AUTH        7E500002   write+notify   HMAC-SHA256 challenge/response
 *   INFO        7E500003   read           fw_version, partitions, MTU, …
 *   CFG_CONTROL 7E500004   write+notify   TLV GET / SET / COMMIT / RESET
 *   DFU_CONTROL 7E500005   write+notify   START / VERIFY / APPLY / ABORT
 *   DFU_DATA    7E500006   write_no_rsp   u16 seq + payload chunks
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"
#include "host/ble_gap.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Derive the per-device auth secret and prepare internal state.  Must be
 * called after the BLE MAC is available (i.e. after on_sync) and before
 * ble_mgmt_gatt_register.  Idempotent.
 */
void ble_mgmt_init(const uint8_t ble_mac[6]);

/**
 * Register the BP+ Mgmt GATT service table.  Call from inside the BLE stack
 * init path, after the NUS service is registered and before
 * nimble_port_freertos_init starts the host task.
 */
void ble_mgmt_gatt_register(void);

/**
 * Per-connection hooks.  Called from gap_event_cb in ble_nus.c so that mgmt
 * can track the active connection handle and reset session state (auth,
 * DFU progress) across connects/disconnects.
 */
void ble_mgmt_on_connect(uint16_t conn_handle);
void ble_mgmt_on_disconnect(void);
void ble_mgmt_on_mtu(uint16_t mtu);

/**
 * True while the current connection has completed an authenticated handshake.
 * Used by the NUS path and uart_rx_task to gate DFU-privileged behaviour,
 * e.g. the bridge suspension during DFU.
 */
bool ble_mgmt_is_authenticated(void);

/**
 * Negotiated ATT MTU for the active connection, or 23 (BLE default) before
 * MTU exchange has completed.  dfu.c reads this to cap DFU_DATA chunks at
 * MTU-3 so that Chrome's per-write size check passes.
 */
uint16_t ble_mgmt_get_mtu(void);

/**
 * True while a DFU transfer is in progress on this connection.  NUS suspends
 * its RX callback and uart_rx_task drains+discards while this is true.
 * Wired in M5/M7 — stub returns false for now.
 */
bool ble_mgmt_dfu_active(void);

#ifdef __cplusplus
}
#endif
