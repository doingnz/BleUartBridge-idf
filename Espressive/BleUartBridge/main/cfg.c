/**
 * cfg.c  —  NVS-backed runtime configuration with TLV wire encoding
 *
 * Storage layout: NVS namespace "bpcfg".  One key per TLV named "t<hex id>"
 * (e.g. "t01" for CFG_UART_BAUD).  Integer types stored with the matching
 * nvs_set_uN / nvs_set_i8 call; strings stored with nvs_set_str.
 *
 * In-RAM cache (s_values) is the source of truth for reads.  cfg_commit()
 * diffs the cache against a snapshot taken at the previous commit and writes
 * changed keys.
 */
#include "cfg.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CFG";
static const char *NVS_NS = "bpcfg";

static cfg_values_t  s_values;
static cfg_values_t  s_snapshot;   /* last persisted state, for diffing        */
static cfg_live_cb_t s_live_cbs[CFG_MAX_LIVE_CBS];
static uint8_t       s_live_cb_count = 0;

/* ── Schema ───────────────────────────────────────────────────────────────── */

typedef struct {
    cfg_id_t   id;
    cfg_type_t type;
    uint8_t    max_len;     /* for STR; ignored for integer types              */
    bool       live_apply;
    const char *nvs_key;    /* "t01" etc.                                      */
} cfg_meta_t;

static const cfg_meta_t s_meta[] = {
    { CFG_UART_BAUD,         CFG_TYPE_U32,     0, false, "t01" },
    { CFG_UART_FLOWCTRL,     CFG_TYPE_U8,      0, false, "t02" },
    { CFG_UART_DATABITS,     CFG_TYPE_U8,      0, false, "t03" },
    { CFG_UART_PARITY,       CFG_TYPE_U8,      0, false, "t04" },
    { CFG_UART_STOPBITS,     CFG_TYPE_U8,      0, false, "t05" },
    { CFG_NAME_SUFFIX,       CFG_TYPE_STR,     8, false, "t06" },
    { CFG_BLE_TX_POWER,      CFG_TYPE_I8,      0, true,  "t07" },
    { CFG_HEXDUMP_DEFAULT,   CFG_TYPE_U8,      0, true,  "t08" },
    { CFG_DFU_ENABLED,       CFG_TYPE_U8,      0, true,  "t09" },
    { CFG_AUTH_REQUIRED,     CFG_TYPE_U8,      0, true,  "t0a" },
    { CFG_FACTORY_RESET,     CFG_TYPE_TRIGGER, 0, false, NULL  },
    { CFG_BLE_ADV_INTERVAL,  CFG_TYPE_U16,     0, true,  "t0c" },
    { CFG_BLE_PREFERRED_MTU, CFG_TYPE_U16,     0, false, "t0d" },
};
static const size_t s_meta_n = sizeof(s_meta) / sizeof(s_meta[0]);

static const cfg_meta_t *find_meta(cfg_id_t id)
{
    for (size_t i = 0; i < s_meta_n; ++i) {
        if (s_meta[i].id == id) return &s_meta[i];
    }
    return NULL;
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */

static void set_defaults(cfg_values_t *v)
{
    v->uart_baud           = 115200;
    v->uart_flowctrl       = 1;
    v->uart_databits       = 8;
    v->uart_parity         = 0;
    v->uart_stopbits       = 1;
    v->name_suffix[0]      = '\0';       /* empty = derive from MAC            */
    v->ble_tx_power        = 3;
    v->hexdump_default     = 0;
    v->dfu_enabled         = 1;
    v->auth_required       = 1;
    v->ble_adv_interval_ms = 100;
    v->ble_preferred_mtu   = 509;
}

/* ── Load / store helpers ─────────────────────────────────────────────────── */

static esp_err_t load_from_nvs(cfg_values_t *v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* first boot — keep defaults */
    if (err != ESP_OK) return err;

    uint32_t u32;  uint16_t u16;  uint8_t u8;  int8_t i8;

    if (nvs_get_u32(h, "t01", &u32) == ESP_OK) v->uart_baud           = u32;
    if (nvs_get_u8 (h, "t02", &u8)  == ESP_OK) v->uart_flowctrl       = u8;
    if (nvs_get_u8 (h, "t03", &u8)  == ESP_OK) v->uart_databits       = u8;
    if (nvs_get_u8 (h, "t04", &u8)  == ESP_OK) v->uart_parity         = u8;
    if (nvs_get_u8 (h, "t05", &u8)  == ESP_OK) v->uart_stopbits       = u8;

    size_t sl = sizeof(v->name_suffix);
    nvs_get_str(h, "t06", v->name_suffix, &sl);

    if (nvs_get_i8 (h, "t07", &i8)  == ESP_OK) v->ble_tx_power        = i8;
    if (nvs_get_u8 (h, "t08", &u8)  == ESP_OK) v->hexdump_default     = u8;
    if (nvs_get_u8 (h, "t09", &u8)  == ESP_OK) v->dfu_enabled         = u8;
    if (nvs_get_u8 (h, "t0a", &u8)  == ESP_OK) v->auth_required       = u8;
    if (nvs_get_u16(h, "t0c", &u16) == ESP_OK) v->ble_adv_interval_ms = u16;
    if (nvs_get_u16(h, "t0d", &u16) == ESP_OK) v->ble_preferred_mtu   = u16;

    nvs_close(h);
    return ESP_OK;
}

/* Returns 1 if value changed (and was written), 0 if unchanged, <0 on NVS error. */
static int write_if_changed(nvs_handle_t h, const cfg_meta_t *m,
                            const cfg_values_t *cur, const cfg_values_t *prev)
{
    esp_err_t err = ESP_OK;
    switch (m->id) {
    case CFG_UART_BAUD:
        if (cur->uart_baud == prev->uart_baud) return 0;
        err = nvs_set_u32(h, m->nvs_key, cur->uart_baud); break;
    case CFG_UART_FLOWCTRL:
        if (cur->uart_flowctrl == prev->uart_flowctrl) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->uart_flowctrl); break;
    case CFG_UART_DATABITS:
        if (cur->uart_databits == prev->uart_databits) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->uart_databits); break;
    case CFG_UART_PARITY:
        if (cur->uart_parity == prev->uart_parity) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->uart_parity); break;
    case CFG_UART_STOPBITS:
        if (cur->uart_stopbits == prev->uart_stopbits) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->uart_stopbits); break;
    case CFG_NAME_SUFFIX:
        if (strcmp(cur->name_suffix, prev->name_suffix) == 0) return 0;
        err = nvs_set_str(h, m->nvs_key, cur->name_suffix); break;
    case CFG_BLE_TX_POWER:
        if (cur->ble_tx_power == prev->ble_tx_power) return 0;
        err = nvs_set_i8(h, m->nvs_key, cur->ble_tx_power); break;
    case CFG_HEXDUMP_DEFAULT:
        if (cur->hexdump_default == prev->hexdump_default) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->hexdump_default); break;
    case CFG_DFU_ENABLED:
        if (cur->dfu_enabled == prev->dfu_enabled) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->dfu_enabled); break;
    case CFG_AUTH_REQUIRED:
        if (cur->auth_required == prev->auth_required) return 0;
        err = nvs_set_u8(h, m->nvs_key, cur->auth_required); break;
    case CFG_BLE_ADV_INTERVAL:
        if (cur->ble_adv_interval_ms == prev->ble_adv_interval_ms) return 0;
        err = nvs_set_u16(h, m->nvs_key, cur->ble_adv_interval_ms); break;
    case CFG_BLE_PREFERRED_MTU:
        if (cur->ble_preferred_mtu == prev->ble_preferred_mtu) return 0;
        err = nvs_set_u16(h, m->nvs_key, cur->ble_preferred_mtu); break;
    case CFG_FACTORY_RESET:
        return 0;   /* trigger is never persisted */
    }
    return (err == ESP_OK) ? 1 : -1;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void cfg_init(void)
{
    /* nvs_flash_init() is called by ble_nus.c as well; harmless if repeated. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    set_defaults(&s_values);
    ESP_ERROR_CHECK(load_from_nvs(&s_values));
    s_snapshot = s_values;

    ESP_LOGI(TAG, "loaded: baud=%lu fc=%u db=%u par=%u sb=%u",
             (unsigned long)s_values.uart_baud, s_values.uart_flowctrl,
             s_values.uart_databits, s_values.uart_parity, s_values.uart_stopbits);
    ESP_LOGI(TAG, "        name_suffix='%s' tx_power=%d dfu=%u auth=%u",
             s_values.name_suffix, s_values.ble_tx_power,
             s_values.dfu_enabled, s_values.auth_required);
    ESP_LOGI(TAG, "        adv_itvl=%u ms pref_mtu=%u hexdump=%u",
             s_values.ble_adv_interval_ms, s_values.ble_preferred_mtu,
             s_values.hexdump_default);
}

const cfg_values_t *cfg_values(void)
{
    return &s_values;
}

bool cfg_describe(cfg_id_t id, cfg_type_t *out_type, uint8_t *out_max_len,
                  bool *out_live_apply)
{
    const cfg_meta_t *m = find_meta(id);
    if (!m) return false;
    if (out_type)       *out_type       = m->type;
    if (out_max_len)    *out_max_len    = m->max_len;
    if (out_live_apply) *out_live_apply = m->live_apply;
    return true;
}

static uint32_t read_le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int cfg_set_tlv(cfg_id_t id, const uint8_t *val, uint8_t len)
{
    const cfg_meta_t *m = find_meta(id);
    if (!m) return CFG_ERR_BAD_ID;

    switch (m->type) {
    case CFG_TYPE_U8:
    case CFG_TYPE_I8:
        if (len != 1) return CFG_ERR_BAD_LEN;
        break;
    case CFG_TYPE_U16:
        if (len != 2) return CFG_ERR_BAD_LEN;
        break;
    case CFG_TYPE_U32:
        if (len != 4) return CFG_ERR_BAD_LEN;
        break;
    case CFG_TYPE_STR:
        if (len > m->max_len) return CFG_ERR_BAD_LEN;
        break;
    case CFG_TYPE_TRIGGER:
        if (len != 1 || val[0] != 1) return CFG_ERR_BAD_LEN;
        break;
    }

    switch (id) {
    case CFG_UART_BAUD: {
        uint32_t v = read_le_u32(val);
        if (v < 300 || v > 3000000) return CFG_ERR_RANGE;
        s_values.uart_baud = v;
        return CFG_OK;
    }
    case CFG_UART_FLOWCTRL:
        if (val[0] > 1) return CFG_ERR_RANGE;
        s_values.uart_flowctrl = val[0];
        return CFG_OK;
    case CFG_UART_DATABITS:
        if (val[0] < 5 || val[0] > 8) return CFG_ERR_RANGE;
        s_values.uart_databits = val[0];
        return CFG_OK;
    case CFG_UART_PARITY:
        if (val[0] > 2) return CFG_ERR_RANGE;
        s_values.uart_parity = val[0];
        return CFG_OK;
    case CFG_UART_STOPBITS:
        if (val[0] != 1 && val[0] != 2) return CFG_ERR_RANGE;
        s_values.uart_stopbits = val[0];
        return CFG_OK;
    case CFG_NAME_SUFFIX:
        memcpy(s_values.name_suffix, val, len);
        s_values.name_suffix[len] = '\0';
        return CFG_OK;
    case CFG_BLE_TX_POWER: {
        int8_t v = (int8_t)val[0];
        if (v < -27 || v > 9) return CFG_ERR_RANGE;   /* ESP32 TX power range */
        s_values.ble_tx_power = v;
        return CFG_OK;
    }
    case CFG_HEXDUMP_DEFAULT:
        if (val[0] > 1) return CFG_ERR_RANGE;
        s_values.hexdump_default = val[0];
        return CFG_OK;
    case CFG_DFU_ENABLED:
        if (val[0] > 1) return CFG_ERR_RANGE;
        s_values.dfu_enabled = val[0];
        return CFG_OK;
    case CFG_AUTH_REQUIRED:
        if (val[0] > 1) return CFG_ERR_RANGE;
        s_values.auth_required = val[0];
        return CFG_OK;
    case CFG_FACTORY_RESET:
        /* Caller should call cfg_factory_reset() after seeing this TLV set. */
        return CFG_OK;
    case CFG_BLE_ADV_INTERVAL: {
        uint16_t v = read_le_u16(val);
        if (v < 20 || v > 10240) return CFG_ERR_RANGE;
        s_values.ble_adv_interval_ms = v;
        return CFG_OK;
    }
    case CFG_BLE_PREFERRED_MTU: {
        uint16_t v = read_le_u16(val);
        if (v < 23 || v > 512) return CFG_ERR_RANGE;
        s_values.ble_preferred_mtu = v;
        return CFG_OK;
    }
    }
    return CFG_ERR_BAD_ID;
}

int cfg_get_tlv(cfg_id_t id, uint8_t *out, uint8_t *out_len)
{
    if (!out || !out_len) return CFG_ERR_BAD_ID;
    uint8_t cap = *out_len;

    switch (id) {
    case CFG_UART_BAUD:
        if (cap < 4) return CFG_ERR_BAD_LEN;
        out[0] = s_values.uart_baud & 0xFF;
        out[1] = (s_values.uart_baud >> 8) & 0xFF;
        out[2] = (s_values.uart_baud >> 16) & 0xFF;
        out[3] = (s_values.uart_baud >> 24) & 0xFF;
        *out_len = 4; return CFG_OK;
    case CFG_UART_FLOWCTRL:     out[0] = s_values.uart_flowctrl;     *out_len=1; return CFG_OK;
    case CFG_UART_DATABITS:     out[0] = s_values.uart_databits;     *out_len=1; return CFG_OK;
    case CFG_UART_PARITY:       out[0] = s_values.uart_parity;       *out_len=1; return CFG_OK;
    case CFG_UART_STOPBITS:     out[0] = s_values.uart_stopbits;     *out_len=1; return CFG_OK;
    case CFG_NAME_SUFFIX: {
        size_t n = strlen(s_values.name_suffix);
        if (cap < n) return CFG_ERR_BAD_LEN;
        memcpy(out, s_values.name_suffix, n);
        *out_len = (uint8_t)n; return CFG_OK;
    }
    case CFG_BLE_TX_POWER:      out[0] = (uint8_t)s_values.ble_tx_power;   *out_len=1; return CFG_OK;
    case CFG_HEXDUMP_DEFAULT:   out[0] = s_values.hexdump_default;   *out_len=1; return CFG_OK;
    case CFG_DFU_ENABLED:       out[0] = s_values.dfu_enabled;       *out_len=1; return CFG_OK;
    case CFG_AUTH_REQUIRED:     out[0] = s_values.auth_required;     *out_len=1; return CFG_OK;
    case CFG_FACTORY_RESET:
        /* Trigger — no readable value. */
        *out_len = 0; return CFG_OK;
    case CFG_BLE_ADV_INTERVAL:
        if (cap < 2) return CFG_ERR_BAD_LEN;
        out[0] = s_values.ble_adv_interval_ms & 0xFF;
        out[1] = (s_values.ble_adv_interval_ms >> 8) & 0xFF;
        *out_len = 2; return CFG_OK;
    case CFG_BLE_PREFERRED_MTU:
        if (cap < 2) return CFG_ERR_BAD_LEN;
        out[0] = s_values.ble_preferred_mtu & 0xFF;
        out[1] = (s_values.ble_preferred_mtu >> 8) & 0xFF;
        *out_len = 2; return CFG_OK;
    }
    return CFG_ERR_BAD_ID;
}

int cfg_commit(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %d", err);
        return CFG_ERR_NVS;
    }

    bool any_change = false;
    bool any_live_change = false;
    for (size_t i = 0; i < s_meta_n; ++i) {
        const cfg_meta_t *m = &s_meta[i];
        if (m->type == CFG_TYPE_TRIGGER) continue;

        int changed = write_if_changed(h, m, &s_values, &s_snapshot);
        if (changed < 0) {
            ESP_LOGE(TAG, "nvs_set for %s failed", m->nvs_key);
            nvs_close(h);
            return CFG_ERR_NVS;
        }
        if (changed) {
            any_change = true;
            if (m->live_apply) any_live_change = true;
        }
    }

    if (any_change) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit: %d", err);
            nvs_close(h);
            return CFG_ERR_NVS;
        }
    }
    nvs_close(h);

    s_snapshot = s_values;

    if (any_live_change) {
        for (uint8_t i = 0; i < s_live_cb_count; ++i) {
            if (s_live_cbs[i]) s_live_cbs[i](&s_values);
        }
    }
    return CFG_OK;
}

void cfg_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset — erasing NVS namespace and rebooting");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Flush logs and restart. */
    fflush(stdout);
    esp_restart();
}

void cfg_register_live_cb(cfg_live_cb_t cb)
{
    if (!cb || s_live_cb_count >= CFG_MAX_LIVE_CBS) return;
    s_live_cbs[s_live_cb_count++] = cb;
}
