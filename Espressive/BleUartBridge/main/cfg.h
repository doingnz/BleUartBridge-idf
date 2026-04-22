/**
 * cfg.h  —  NVS-backed runtime configuration with TLV wire encoding
 *
 * A single in-RAM values struct is the source of truth.  Reads are free.
 * Writes update the cache; cfg_commit() persists the cache to NVS.
 *
 * Live-apply settings (tx_power, hexdump_default, dfu_enabled, auth_required,
 * ble_adv_interval_ms) take effect immediately when commit is called.
 *
 * Reboot-apply settings (uart_*, name_suffix, ble_preferred_mtu) are persisted
 * but only take effect on the next boot.
 *
 * TLV wire encoding (for BLE CFG_CONTROL):
 *   u8  id        cfg_id_t
 *   u8  len       payload length in bytes
 *   u8  val[len]  little-endian for multi-byte integers; raw UTF-8 for strings
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TLV identifiers — also used as NVS key indices. Do not renumber. */
typedef enum {
    CFG_UART_BAUD          = 0x01,  /* u32  default 115200                    */
    CFG_UART_FLOWCTRL      = 0x02,  /* u8   0=none 1=RTS/CTS  default 1       */
    CFG_UART_DATABITS      = 0x03,  /* u8   5..8             default 8        */
    CFG_UART_PARITY        = 0x04,  /* u8   0=N 1=E 2=O      default 0        */
    CFG_UART_STOPBITS      = 0x05,  /* u8   1 or 2           default 1        */
    CFG_NAME_SUFFIX        = 0x06,  /* str  max 8; empty = derive from MAC    */
    CFG_BLE_TX_POWER       = 0x07,  /* i8   dBm              default 3        */
    CFG_HEXDUMP_DEFAULT    = 0x08,  /* u8   0/1              default 0        */
    CFG_DFU_ENABLED        = 0x09,  /* u8   0/1              default 1        */
    CFG_AUTH_REQUIRED      = 0x0A,  /* u8   0/1              default 1        */
    CFG_FACTORY_RESET      = 0x0B,  /* trigger (write 1 -> erase + reboot)    */
    CFG_BLE_ADV_INTERVAL   = 0x0C,  /* u16  ms 20..10240     default 100      */
    CFG_BLE_PREFERRED_MTU  = 0x0D,  /* u16  23..512          default 509      */
} cfg_id_t;

typedef enum {
    CFG_TYPE_U8,
    CFG_TYPE_I8,
    CFG_TYPE_U16,
    CFG_TYPE_U32,
    CFG_TYPE_STR,
    CFG_TYPE_TRIGGER,
} cfg_type_t;

typedef struct {
    uint32_t uart_baud;
    uint8_t  uart_flowctrl;
    uint8_t  uart_databits;
    uint8_t  uart_parity;
    uint8_t  uart_stopbits;
    char     name_suffix[9];        /* 8 chars + NUL                          */
    int8_t   ble_tx_power;
    uint8_t  hexdump_default;
    uint8_t  dfu_enabled;
    uint8_t  auth_required;
    uint16_t ble_adv_interval_ms;
    uint16_t ble_preferred_mtu;
} cfg_values_t;

/* Error codes returned by cfg_set_tlv / cfg_get_tlv. */
#define CFG_OK            0
#define CFG_ERR_BAD_ID   -1
#define CFG_ERR_BAD_LEN  -2
#define CFG_ERR_RANGE    -3
#define CFG_ERR_NVS      -4

/**
 * Open NVS namespace "bpcfg" and load all values into the cache.  Missing
 * values are initialised to their defaults.  Must be called once before any
 * other cfg_* function, and before nus_init() / uart_init() so they can read
 * the current configuration.
 *
 * Returns ESP_OK on success; aborts on unrecoverable NVS failure.
 */
void cfg_init(void);

/** Read-only access to the current cached values. */
const cfg_values_t *cfg_values(void);

/**
 * Describe a TLV by id.  Returns false if id is unknown.
 * Populated fields let the BLE layer validate SET payload size without
 * duplicating the schema.
 */
bool cfg_describe(cfg_id_t id, cfg_type_t *out_type, uint8_t *out_max_len,
                  bool *out_live_apply);

/**
 * TLV wire-format SET: applies value to the in-RAM cache, does not persist.
 *   val points to a little-endian integer or a raw string (no NUL required).
 *   len must match the type (1/2/4 for integers; any 0..max for strings).
 *
 * For CFG_FACTORY_RESET the caller should invoke cfg_factory_reset() directly.
 *
 * Returns CFG_OK or one of CFG_ERR_*.
 */
int cfg_set_tlv(cfg_id_t id, const uint8_t *val, uint8_t len);

/**
 * TLV wire-format GET: serialises the cached value into out.
 *   out must have room for (*out_len) bytes on entry; updated to bytes written.
 *
 * Returns CFG_OK or CFG_ERR_BAD_ID.
 */
int cfg_get_tlv(cfg_id_t id, uint8_t *out, uint8_t *out_len);

/**
 * Persist the current cache to NVS and run live-apply callbacks for any
 * settings whose values changed since the last commit.
 *
 * Live callbacks are invoked from the calling task's context; keep them cheap.
 *
 * Returns CFG_OK or CFG_ERR_NVS.
 */
int cfg_commit(void);

/**
 * Erase the "bpcfg" namespace and reboot.  Does not return.
 */
void cfg_factory_reset(void);

/* ── Live-apply callback registration ────────────────────────────────────── */

typedef void (*cfg_live_cb_t)(const cfg_values_t *values);

/**
 * Register a callback invoked from cfg_commit() when a live-apply TLV changed.
 * The entire values struct is passed; callbacks should compare against their
 * own remembered state to decide what to re-apply.
 *
 * A fixed-size slot table is used; up to CFG_MAX_LIVE_CBS (4) registrations.
 */
#define CFG_MAX_LIVE_CBS 4
void cfg_register_live_cb(cfg_live_cb_t cb);

#ifdef __cplusplus
}
#endif
