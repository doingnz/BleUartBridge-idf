/**
 * auth.c — HMAC-SHA256 challenge/response authentication.
 *
 * Uses mbedTLS for HMAC-SHA256 and esp_fill_random() for the 16-byte nonce.
 * Both functions are available unconditionally in ESP-IDF v5+.
 */
#include "auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "secrets.h"
#include "sha256.h"

static const char *TAG = "AUTH";

#define CHALLENGE_LEN  16
#define RESPONSE_LEN   32
#define MAX_FAILS       3
#define LOCKOUT_MS  30000

typedef enum {
    ST_IDLE,
    ST_CHALLENGE_ISSUED,
    ST_AUTHED,
} auth_state_t;

static uint8_t      s_device_secret[32];
static auth_state_t s_state        = ST_IDLE;
static uint8_t      s_challenge[CHALLENGE_LEN];
static uint8_t      s_fail_count   = 0;
static int64_t      s_lockout_until_us = 0;

void auth_init(const uint8_t ble_mac[6])
{
    bp_hmac_sha256(MASTER_KEY, sizeof(MASTER_KEY), ble_mac, 6, s_device_secret);
    ESP_LOGI(TAG, "device secret derived from MAC "
                  "%02x:%02x:%02x:%02x:%02x:%02x (fp=%02x%02x%02x%02x)",
             ble_mac[5], ble_mac[4], ble_mac[3],
             ble_mac[2], ble_mac[1], ble_mac[0],
             s_device_secret[0], s_device_secret[1],
             s_device_secret[2], s_device_secret[3]);
}

void auth_on_connect(void)
{
    s_state = ST_IDLE;
    memset(s_challenge, 0, sizeof(s_challenge));
}

void auth_on_disconnect(void)
{
    s_state = ST_IDLE;
    memset(s_challenge, 0, sizeof(s_challenge));
    /* s_fail_count and lockout persist across disconnects to slow brute force */
}

bool auth_is_authenticated(void)
{
    return s_state == ST_AUTHED;
}

bool auth_is_locked_out(void)
{
    return s_lockout_until_us != 0 && esp_timer_get_time() < s_lockout_until_us;
}

bool auth_handle_write(const uint8_t *data, size_t len, auth_notify_cb_t notify)
{
    if (!data || len < 1) return false;
    uint8_t op = data[0];

    if (auth_is_locked_out()) {
        uint8_t resp[2] = { 0x82, 2 /* LOCKED */ };
        if (notify) notify(resp, sizeof(resp));
        return true;
    }

    switch (op) {

    case 0x01: { /* BEGIN — issue 16-byte challenge */
        esp_fill_random(s_challenge, sizeof(s_challenge));
        s_state = ST_CHALLENGE_ISSUED;
        uint8_t resp[1 + CHALLENGE_LEN];
        resp[0] = 0x81;
        memcpy(&resp[1], s_challenge, CHALLENGE_LEN);
        if (notify) notify(resp, sizeof(resp));
        return true;
    }

    case 0x02: { /* RESPONSE — verify HMAC */
        if (s_state != ST_CHALLENGE_ISSUED || len != 1 + RESPONSE_LEN) {
            uint8_t resp[2] = { 0x82, 1 /* FAIL */ };
            if (notify) notify(resp, sizeof(resp));
            return false;
        }

        uint8_t expected[32];
        bp_hmac_sha256(s_device_secret, sizeof(s_device_secret),
                    s_challenge, CHALLENGE_LEN, expected);

        /* Constant-time compare to avoid timing side channel. */
        uint8_t diff = 0;
        for (size_t i = 0; i < 32; ++i) diff |= expected[i] ^ data[1 + i];

        if (diff == 0) {
            s_state      = ST_AUTHED;
            s_fail_count = 0;
            ESP_LOGI(TAG, "authenticated");
            uint8_t resp[2] = { 0x82, 0 /* OK */ };
            if (notify) notify(resp, sizeof(resp));
            return true;
        }

        s_fail_count++;
        s_state = ST_IDLE;
        ESP_LOGW(TAG, "handshake failed (%u/%u)", s_fail_count, MAX_FAILS);

        if (s_fail_count >= MAX_FAILS) {
            s_lockout_until_us = esp_timer_get_time() + (int64_t)LOCKOUT_MS * 1000;
            s_fail_count = 0;     /* reset counter; lockout timer takes over */
            ESP_LOGW(TAG, "auth lockout for %d ms", LOCKOUT_MS);
            uint8_t resp[2] = { 0x82, 2 /* LOCKED */ };
            if (notify) notify(resp, sizeof(resp));
        } else {
            uint8_t resp[2] = { 0x82, 1 /* FAIL */ };
            if (notify) notify(resp, sizeof(resp));
        }
        return true;
    }

    default:
        return false;
    }
}
