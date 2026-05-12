#ifndef FLOCK_OUI_H
#define FLOCK_OUI_H

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

// Merged Flock Safety OUI table from oui-spy-unified-blue + invader-sniffer.
// Sorted by first byte for bucket indexing.
static const uint8_t FLOCK_OUI_TABLE[][3] = {
    {0x00,0xf4,0x8d},
    {0x04,0x0d,0x84},
    {0x08,0x3a,0x88},
    {0x14,0x5a,0xfc},
    {0x14,0xb5,0xcd},
    {0x1c,0x34,0xf1},
    {0x24,0xb2,0xb9},
    {0x38,0x5b,0x44},
    {0x3c,0x71,0xbf},
    {0x3c,0x91,0x80},
    {0x48,0x27,0xea},
    {0x58,0x00,0xe3},
    {0x58,0x8e,0x81},
    {0x5c,0x93,0xa2},
    {0x64,0x6e,0x69},
    {0x70,0x08,0x94},
    {0x70,0xc9,0x4e},
    {0x74,0x4c,0xa1},
    {0x80,0x30,0x49},
    {0x82,0x6b,0xf2},
    {0x90,0x35,0xea},
    {0x94,0x08,0x53},
    {0x94,0x2a,0x6f},
    {0x94,0x34,0x69},
    {0x9c,0x2f,0x9d},
    {0xa4,0xcf,0x12},
    {0xb4,0x1e,0x52},
    {0xb4,0xe3,0xf9},
    {0xb8,0x1e,0xa4},
    {0xb8,0x35,0x32},
    {0xc0,0x35,0x32},
    {0xcc,0xcc,0xcc},
    {0xd0,0x39,0x57},
    {0xd4,0x11,0xd6},
    {0xd8,0xf3,0xbc},
    {0xe0,0x0a,0xf6},
    {0xe0,0x4f,0x43},
    {0xe4,0xaa,0xea},
    {0xe8,0xd0,0xfc},
    {0xec,0x1b,0xbd},
    {0xf0,0x82,0xc0},
    {0xf4,0x6a,0xdd},
    {0xf4,0xe2,0xc6},
    {0xf8,0xa2,0xd6},
};
static const int FLOCK_OUI_COUNT = sizeof(FLOCK_OUI_TABLE) / sizeof(FLOCK_OUI_TABLE[0]);

// Bucket index: first-byte -> {start, count} into sorted table.
// Turns full-table scan into O(small) per lookup.
struct FlockOuiBucket {
    uint8_t start;
    uint8_t count;
};

static FlockOuiBucket flockOuiBuckets[256];
static bool flockOuiBucketsReady = false;

static inline void flockOuiInitBuckets() {
    if (flockOuiBucketsReady) return;
    memset(flockOuiBuckets, 0, sizeof(flockOuiBuckets));
    for (int i = 0; i < FLOCK_OUI_COUNT; i++) {
        uint8_t b = FLOCK_OUI_TABLE[i][0];
        if (flockOuiBuckets[b].count == 0) {
            flockOuiBuckets[b].start = (uint8_t)i;
        }
        flockOuiBuckets[b].count++;
    }
    flockOuiBucketsReady = true;
}

// Normal context OUI match.
// Skips locally-administered MACs (bit 1 of first byte).
static inline bool flockMatchOui(const uint8_t* mac) {
    if (mac[0] & 0x02) return false;
    if (!flockOuiBucketsReady) flockOuiInitBuckets();
    const FlockOuiBucket& b = flockOuiBuckets[mac[0]];
    for (int i = b.start; i < b.start + b.count; i++) {
        if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
            mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
    }
    return false;
}

// OUI match that also handles BLE random-address bit masking.
// BLE static-random addresses have bits 7:6 = 11 in byte 0, which
// the spoofer sets. Mask those bits before lookup so spoofed MACs match.
static inline bool flockMatchOuiMasked(const uint8_t* mac) {
    uint8_t masked[3] = { (uint8_t)(mac[0] & 0x3Fu), mac[1], mac[2] };
    if (!flockOuiBucketsReady) flockOuiInitBuckets();
    // Try exact first
    const FlockOuiBucket& b1 = flockOuiBuckets[mac[0]];
    for (int i = b1.start; i < b1.start + b1.count; i++) {
        if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
            mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
    }
    // Try masked (different first-byte bucket)
    if (masked[0] != mac[0]) {
        const FlockOuiBucket& b2 = flockOuiBuckets[masked[0]];
        for (int i = b2.start; i < b2.start + b2.count; i++) {
            if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
                mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
        }
    }
    return false;
}

// ISR-safe variant: no lazy init, returns false if buckets not ready.
static inline bool IRAM_ATTR flockMatchOuiISR(const uint8_t* mac) {
    if (mac[0] & 0x02) return false;
    if (!flockOuiBucketsReady) return false;
    const FlockOuiBucket& b = flockOuiBuckets[mac[0]];
    for (int i = b.start; i < b.start + b.count; i++) {
        if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
            mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
    }
    return false;
}

// ISR-safe variant with BLE random-address masking.
static inline bool IRAM_ATTR flockMatchOuiMaskedISR(const uint8_t* mac) {
    if (!flockOuiBucketsReady) return false;
    // Try exact
    const FlockOuiBucket& b1 = flockOuiBuckets[mac[0]];
    for (int i = b1.start; i < b1.start + b1.count; i++) {
        if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
            mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
    }
    // Try masked
    uint8_t m0 = mac[0] & 0x3Fu;
    if (m0 != mac[0]) {
        const FlockOuiBucket& b2 = flockOuiBuckets[m0];
        for (int i = b2.start; i < b2.start + b2.count; i++) {
            if (mac[1] == FLOCK_OUI_TABLE[i][1] &&
                mac[2] == FLOCK_OUI_TABLE[i][2]) return true;
        }
    }
    return false;
}

// Parse 802.11 tagged parameters to detect wildcard (empty SSID) probe requests.
// Returns 1 if wildcard (SSID IE present with length 0), 0 if directed, -1 if no SSID IE.
static inline int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
    if (!body || len < 2) return -1;
    while (len >= 2) {
        uint8_t id   = body[0];
        uint8_t elen = body[1];
        if ((int)elen + 2 > len) break;
        if (id == 0) return (elen == 0) ? 1 : 0;
        body += elen + 2;
        len  -= elen + 2;
    }
    return -1;
}

// ---------- BLE detection constants ----------

// Manufacturer company ID used by Flock/XUNTONG hardware
static const uint16_t FLOCK_MFG_IDS[] = { 0x09C8 };
static const int FLOCK_MFG_ID_COUNT = sizeof(FLOCK_MFG_IDS) / sizeof(FLOCK_MFG_IDS[0]);

// Raven gunshot detector BLE service UUIDs
#define RAVEN_GPS_SVC       "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SVC     "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOC_SVC   "00001819-0000-1000-8000-00805f9b34fb"

static const char* RAVEN_UUIDS[] = {
    "0000180a-0000-1000-8000-00805f9b34fb",
    RAVEN_GPS_SVC,
    RAVEN_POWER_SVC,
    "00003300-0000-1000-8000-00805f9b34fb",
    "00003400-0000-1000-8000-00805f9b34fb",
    "00003500-0000-1000-8000-00805f9b34fb",
    "00001809-0000-1000-8000-00805f9b34fb",
    RAVEN_OLD_LOC_SVC
};
static const int RAVEN_UUID_COUNT = sizeof(RAVEN_UUIDS) / sizeof(RAVEN_UUIDS[0]);

// BLE advertisement name patterns (case-insensitive substring match)
static const char* FLOCK_BLE_NAMES[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision"
};
static const int FLOCK_BLE_NAME_COUNT = sizeof(FLOCK_BLE_NAMES) / sizeof(FLOCK_BLE_NAMES[0]);

// Check manufacturer company ID from BLE adv data
static inline bool flockCheckMfgID(uint16_t id) {
    for (int i = 0; i < FLOCK_MFG_ID_COUNT; i++) {
        if (FLOCK_MFG_IDS[i] == id) return true;
    }
    return false;
}

// Check BLE device name against Flock patterns (case-insensitive substring)
static inline bool flockCheckName(const char* name) {
    if (!name || !name[0]) return false;
    for (int i = 0; i < FLOCK_BLE_NAME_COUNT; i++) {
        if (strcasestr(name, FLOCK_BLE_NAMES[i])) return true;
    }
    return false;
}

// Extract manufacturer company ID from raw BLE advertisement data.
// AD type 0xFF = manufacturer specific data, first 2 bytes = company ID (LE).
static inline bool flockExtractMfgID(const uint8_t* adv, uint8_t advLen, uint16_t* outId) {
    for (uint8_t pos = 0; pos + 1 < advLen; ) {
        uint8_t adLen = adv[pos];
        if (adLen == 0) break;
        if (pos + 1 + adLen > advLen) break;
        uint8_t adType = adv[pos + 1];
        if (adType == 0xFF && adLen >= 3) {
            *outId = ((uint16_t)adv[pos + 3] << 8) | (uint16_t)adv[pos + 2];
            return true;
        }
        pos += adLen + 1;
    }
    return false;
}

// Check if raw BLE advertisement contains a 128-bit service UUID matching Raven.
// AD types 0x06/0x07 = complete/incomplete list of 128-bit service UUIDs (16 bytes each, LE).
static inline bool flockCheckRavenAdvUUIDs(const uint8_t* adv, uint8_t advLen) {
    for (uint8_t pos = 0; pos + 1 < advLen; ) {
        uint8_t adLen = adv[pos];
        if (adLen == 0) break;
        if (pos + 1 + adLen > advLen) break;
        uint8_t adType = adv[pos + 1];
        if ((adType == 0x06 || adType == 0x07) && adLen >= 17) {
            int uuidCount = (adLen - 1) / 16;
            for (int u = 0; u < uuidCount; u++) {
                const uint8_t* uuid = &adv[pos + 2 + u * 16];
                // Convert LE bytes to UUID string for comparison
                char buf[37];
                snprintf(buf, sizeof(buf),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    uuid[15], uuid[14], uuid[13], uuid[12],
                    uuid[11], uuid[10],
                    uuid[9], uuid[8],
                    uuid[7], uuid[6],
                    uuid[5], uuid[4], uuid[3], uuid[2], uuid[1], uuid[0]);
                for (int r = 0; r < RAVEN_UUID_COUNT; r++) {
                    if (strcasecmp(buf, RAVEN_UUIDS[r]) == 0) return true;
                }
            }
        }
        pos += adLen + 1;
    }
    return false;
}

// Estimate Raven firmware version from advertised service UUIDs.
// Returns "1.1.x", "1.2.x", "1.3.x", or "?".
static inline const char* flockEstimateRavenFW(const uint8_t* adv, uint8_t advLen) {
    bool has_new_gps = false, has_old_loc = false, has_power = false;
    for (uint8_t pos = 0; pos + 1 < advLen; ) {
        uint8_t adLen = adv[pos];
        if (adLen == 0) break;
        if (pos + 1 + adLen > advLen) break;
        uint8_t adType = adv[pos + 1];
        if ((adType == 0x06 || adType == 0x07) && adLen >= 17) {
            int uuidCount = (adLen - 1) / 16;
            for (int u = 0; u < uuidCount; u++) {
                const uint8_t* uuid = &adv[pos + 2 + u * 16];
                char buf[37];
                snprintf(buf, sizeof(buf),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    uuid[15], uuid[14], uuid[13], uuid[12],
                    uuid[11], uuid[10],
                    uuid[9], uuid[8],
                    uuid[7], uuid[6],
                    uuid[5], uuid[4], uuid[3], uuid[2], uuid[1], uuid[0]);
                if (strcasecmp(buf, RAVEN_GPS_SVC) == 0)     has_new_gps = true;
                if (strcasecmp(buf, RAVEN_OLD_LOC_SVC) == 0)  has_old_loc = true;
                if (strcasecmp(buf, RAVEN_POWER_SVC) == 0)    has_power = true;
            }
        }
        pos += adLen + 1;
    }
    if (has_old_loc && !has_new_gps) return "1.1.x";
    if (has_new_gps && !has_power)   return "1.2.x";
    if (has_new_gps && has_power)    return "1.3.x";
    return "?";
}

#endif
