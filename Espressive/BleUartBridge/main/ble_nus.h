/**
 * ble_nus.h  —  Nordic UART Service (NUS) peripheral using NimBLE
 *
 * NUS UUIDs:
 *   Service : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX char : 6E400002-...  (client writes → forwarded to UART TX)
 *   TX char : 6E400003-...  (UART RX data → notify to client)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialise NimBLE, register the NUS GATT service, and start the NimBLE
 * host task.  Must be called once from app_main before any other nus_*
 * function.
 *
 * @param device_name  BLE advertisement name (e.g. "BP+ Bridge")
 * @param write_cb     Called from the BLE host task when the client writes
 *                     to the RX characteristic.  Must not block for long.
 *                     Signature: void cb(const uint8_t *data, size_t len)
 */
/**
 * Callback invoked (on the NimBLE host task) when the client writes to the
 * NUS RX characteristic.  Return 0 if the data was accepted, non-zero if it
 * could not be accepted (e.g. internal queue full).  A non-zero return causes
 * NimBLE to send an ATT Error Response so that clients using
 * writeValueWithResponse can retry the write; clients using
 * writeValueWithoutResponse cannot be notified of the rejection.
 */
typedef int (*nus_write_cb_t)(const uint8_t *data, size_t len);

void nus_init(const char *device_name, nus_write_cb_t write_cb);

/** Returns true while a client is connected. */
bool nus_is_connected(void);

/** Returns total number of disconnections since boot. */
unsigned long nus_disconnect_count(void);

/** Resets the disconnection counter to zero. */
void nus_reset_disconnect_count(void);

/**
 * Stop and restart advertising so it picks up the current cfg_values
 * (advertising interval, TX power).  Safe to call from any task.  Has no
 * effect while a client is connected; takes effect on the next advertising
 * cycle after disconnect.
 */
void nus_restart_advertising(void);

/** nus_notify return codes */
#define NUS_ERR_NOMEM  (-1)   /**< mbuf pool exhausted — retryable              */
#define NUS_ERR_CONN   (-2)   /**< not connected / connection error — do not retry */

/**
 * Send up to @p len bytes to the connected BLE client as a NUS TX notification.
 *
 * @return 0             success
 *         NUS_ERR_NOMEM mbuf pool temporarily exhausted — caller should retry
 *         NUS_ERR_CONN  no connection or fatal send error — caller must NOT retry
 */
int nus_notify(const uint8_t *data, size_t len);
