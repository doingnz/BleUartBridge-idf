/**
 * secrets.example.h — template for the gitignored secrets.h
 *
 *   cp main/secrets.example.h main/secrets.h
 *   # then replace MASTER_KEY with 32 cryptographically-random bytes
 *
 * The master key never leaves this repo (or the host tooling that talks to
 * this fleet).  Each device derives its per-unit auth secret as
 *
 *   device_secret[32] = HMAC-SHA256(MASTER_KEY, ble_mac[6])
 *
 * so rotating the master key invalidates every device's auth session in one
 * step, and an attacker who extracts one device's secret does not gain access
 * to other units (as long as they cannot extract the master key itself —
 * without Secure Boot + Flash Encryption, JTAG access to any one unit
 * compromises the fleet.  See CLAUDE.md §Security for the upgrade path).
 */
#pragma once

#include <stdint.h>

static const uint8_t MASTER_KEY[32] = {
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
};
