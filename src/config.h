#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include "flock_oui.h"

struct TargetOUI {
    uint8_t ouiCount;
    uint8_t ouis[5][3];
    char name[64];
};

// ---------- Hardcoded special targets (always active) ----------

enum class SpecialHit { NONE, EVIL_BIRD, EVIL_BIRD_RAVEN, RAYBAN };

// Non-Flock surveillance vendor OUIs (linear scan, small list)
static const uint8_t SURV_VENDOR_OUIS[][3] = {
    // SoundThinking (ShotSpotter)
    {0xD4, 0x11, 0xD6},
    // Neology
    {0x00, 0x17, 0x3D},
    // Axon
    {0x00, 0x25, 0xDF},
    // GENETEC
    {0x00, 0x0A, 0xB1},
    {0x00, 0x50, 0xC2},
    {0x00, 0xBF, 0x15},
    // Leonardo UK Ltd
    {0x00, 0x80, 0xE7},
};
static constexpr int SURV_VENDOR_OUI_COUNT =
    (int)(sizeof(SURV_VENDOR_OUIS) / sizeof(SURV_VENDOR_OUIS[0]));

static const uint8_t RAYBAN_OUIS[][3] = {
    {0x7C, 0x2A, 0x9E},
    {0xCC, 0x66, 0x0A},
    {0xF4, 0x03, 0x43},
    {0x5C, 0xE9, 0x1E},
};
static constexpr int RAYBAN_OUI_COUNT =
    (int)(sizeof(RAYBAN_OUIS) / sizeof(RAYBAN_OUIS[0]));

// Hardcoded BLE/WiFi name patterns (case-insensitive, * wildcard)
static const char* const EVIL_BIRD_NAMES[] = {
    "FS Ext Battery",
    "Penguin*",
    "Flock*",
    "Pigvision*",
};
static constexpr int EVIL_BIRD_NAME_COUNT =
    (int)(sizeof(EVIL_BIRD_NAMES) / sizeof(EVIL_BIRD_NAMES[0]));

// Check OUI against Flock (bucket-indexed) + surveillance vendors + RayBan.
// Uses masked matching to handle BLE random-address top-2-bit encoding.
inline SpecialHit checkSpecialOUI(const uint8_t* bda) {
    if (!bda) return SpecialHit::NONE;
    // Flock OUI: bucket-indexed, with BLE random-address masking
    if (flockMatchOuiMasked(bda)) return SpecialHit::EVIL_BIRD;
    // Non-Flock surveillance vendors (small list, linear)
    uint8_t m0 = bda[0] & 0x3Fu;
    for (int i = 0; i < SURV_VENDOR_OUI_COUNT; i++) {
        uint8_t v0 = SURV_VENDOR_OUIS[i][0] & 0x3Fu;
        if (m0 == v0 &&
            bda[1] == SURV_VENDOR_OUIS[i][1] &&
            bda[2] == SURV_VENDOR_OUIS[i][2])
            return SpecialHit::EVIL_BIRD;
    }
    // RayBan / Meta
    for (int i = 0; i < RAYBAN_OUI_COUNT; i++) {
        uint8_t r0 = RAYBAN_OUIS[i][0] & 0x3Fu;
        if (m0 == r0 &&
            bda[1] == RAYBAN_OUIS[i][1] &&
            bda[2] == RAYBAN_OUIS[i][2])
            return SpecialHit::RAYBAN;
    }
    return SpecialHit::NONE;
}

// ISR-safe variant for WiFi promiscuous callback (no lazy init).
inline SpecialHit checkSpecialOUI_ISR(const uint8_t* bda) {
    if (!bda) return SpecialHit::NONE;
    if (flockMatchOuiMaskedISR(bda)) return SpecialHit::EVIL_BIRD;
    uint8_t m0 = bda[0] & 0x3Fu;
    for (int i = 0; i < SURV_VENDOR_OUI_COUNT; i++) {
        uint8_t v0 = SURV_VENDOR_OUIS[i][0] & 0x3Fu;
        if (m0 == v0 &&
            bda[1] == SURV_VENDOR_OUIS[i][1] &&
            bda[2] == SURV_VENDOR_OUIS[i][2])
            return SpecialHit::EVIL_BIRD;
    }
    for (int i = 0; i < RAYBAN_OUI_COUNT; i++) {
        uint8_t r0 = RAYBAN_OUIS[i][0] & 0x3Fu;
        if (m0 == r0 &&
            bda[1] == RAYBAN_OUIS[i][1] &&
            bda[2] == RAYBAN_OUIS[i][2])
            return SpecialHit::RAYBAN;
    }
    return SpecialHit::NONE;
}

// Case-insensitive wildcard match (same algorithm as OUIConfig::matchWildcard)
static bool matchWildcardCI(const char* pat, const char* str) {
    while (*pat && *str) {
        if (*pat == '*') {
            ++pat;
            if (!*pat) return true;
            while (*str) {
                if (matchWildcardCI(pat, str)) return true;
                ++str;
            }
            return false;
        }
        char p = (*pat >= 'A' && *pat <= 'Z') ? *pat + 32 : *pat;
        char s = (*str >= 'A' && *str <= 'Z') ? *str + 32 : *str;
        if (p != s) return false;
        ++pat; ++str;
    }
    while (*pat == '*') ++pat;
    return !*pat && !*str;
}

inline SpecialHit checkSpecialName(const char* name) {
    if (!name || !name[0]) return SpecialHit::NONE;
    // Hardcoded wildcard patterns (from config.h)
    for (int i = 0; i < EVIL_BIRD_NAME_COUNT; i++) {
        if (matchWildcardCI(EVIL_BIRD_NAMES[i], name))
            return SpecialHit::EVIL_BIRD;
    }
    // Flock BLE name patterns (substring, from flock_oui.h)
    if (flockCheckName(name)) return SpecialHit::EVIL_BIRD;
    return SpecialHit::NONE;
}

// Check BLE advertisement for Flock manufacturer ID or Raven service UUIDs.
// Call with raw adv data from Bluedroid scan result.
inline SpecialHit checkFlockBleAdv(const uint8_t* adv, uint8_t advLen) {
    if (!adv || advLen == 0) return SpecialHit::NONE;
    // Manufacturer company ID
    uint16_t mfgId;
    if (flockExtractMfgID(adv, advLen, &mfgId) && flockCheckMfgID(mfgId)) {
        return SpecialHit::EVIL_BIRD;
    }
    // Raven gunshot detector service UUIDs
    if (flockCheckRavenAdvUUIDs(adv, advLen)) {
        return SpecialHit::EVIL_BIRD_RAVEN;
    }
    return SpecialHit::NONE;
}

// ---------------------------------------------------------------

class OUIConfig {
private:
    static constexpr const char* CONFIG_FILE = "/oui_targets.json";
    static constexpr int MAX_TARGETS = 16;
    TargetOUI targets[MAX_TARGETS];
    int targetCount = 0;

public:
    bool load() {
        File f = SD_MMC.open(CONFIG_FILE, FILE_READ);
        if (!f) {
            Serial.println("[oui] no config found");
            return false;
        }

        JsonDocument doc;
        if (!deserializeJson(doc, f)) {
            targetCount = 0;
            JsonArray arr = doc["targets"].as<JsonArray>();
            for (JsonObject obj : arr) {
                if (targetCount >= MAX_TARGETS) break;
                targets[targetCount].ouiCount = 0;
                JsonArray ouisArr = obj["ouis"].as<JsonArray>();
                for (JsonArray ouiArr : ouisArr) {
                    if (targets[targetCount].ouiCount >= 5) break;
                    targets[targetCount].ouis[targets[targetCount].ouiCount][0] =
                        ouiArr[0].as<uint8_t>();
                    targets[targetCount].ouis[targets[targetCount].ouiCount][1] =
                        ouiArr[1].as<uint8_t>();
                    targets[targetCount].ouis[targets[targetCount].ouiCount][2] =
                        ouiArr[2].as<uint8_t>();
                    targets[targetCount].ouiCount++;
                }
                strlcpy(
                    targets[targetCount].name,
                    obj["name"].as<const char*>(),
                    sizeof(targets[targetCount].name)
                );
                targetCount++;
            }
            f.close();
            Serial.printf("[oui] loaded %d targets\n", targetCount);
            return targetCount > 0;
        }
        f.close();
        return false;
    }

    TargetOUI* addTargetNameOnly(const char* name) {
        if (targetCount >= MAX_TARGETS) return nullptr;
        targets[targetCount].ouiCount = 0;
        strlcpy(targets[targetCount].name, name ? name : "",
            sizeof(targets[targetCount].name));
        return &targets[targetCount++];
    }

    bool save() {
        JsonDocument doc;
        JsonArray arr = doc["targets"].to<JsonArray>();
        for (int i = 0; i < targetCount; i++) {
            JsonObject obj = arr.add<JsonObject>();
            JsonArray ouisArr = obj["ouis"].to<JsonArray>();
            for (int j = 0; j < targets[i].ouiCount; j++) {
                JsonArray ouiArr = ouisArr.add<JsonArray>();
                ouiArr.add(targets[i].ouis[j][0]);
                ouiArr.add(targets[i].ouis[j][1]);
                ouiArr.add(targets[i].ouis[j][2]);
            }
            obj["name"] = targets[i].name;
        }

        File f = SD_MMC.open(CONFIG_FILE, FILE_WRITE);
        if (!f) return false;
        bool ok = serializeJson(doc, f) > 0;
        f.close();
        return ok;
    }

    static bool matchWildcard(const char* pat, const char* str) {
        return matchWildcardCI(pat, str);
    }

    bool isNameTarget(const char* devName) const {
        if (!devName || !devName[0]) return false;
        for (int i = 0; i < targetCount; i++) {
            if (!targets[i].name[0]) continue;
            if (matchWildcardCI(targets[i].name, devName)) return true;
        }
        return false;
    }

    bool isTarget(const uint8_t* bda) {
        if (!bda) return false;
        for (int i = 0; i < targetCount; i++) {
            for (int j = 0; j < targets[i].ouiCount; j++) {
                if (targets[i].ouis[j][0] == 0 &&
                    targets[i].ouis[j][1] == 0 &&
                    targets[i].ouis[j][2] == 0) continue;

                if (bda[0] == targets[i].ouis[j][0] &&
                    bda[1] == targets[i].ouis[j][1] &&
                    bda[2] == targets[i].ouis[j][2]) {
                    return true;
                }
            }
        }
        return false;
    }

    const TargetOUI* getTargets() const { return targets; }
    int getCount() const { return targetCount; }

    void deleteTarget(int idx) {
        if (idx < 0 || idx >= targetCount) return;
        for (int i = idx; i < targetCount - 1; i++) {
            memcpy(&targets[i], &targets[i + 1], sizeof(TargetOUI));
        }
        targetCount--;
    }

    void addTarget(const uint8_t oui[3], const char* name) {
        if (targetCount >= MAX_TARGETS) return;
        targets[targetCount].ouiCount = 1;
        memcpy(targets[targetCount].ouis[0], oui, 3);
        strlcpy(targets[targetCount].name, name,
            sizeof(targets[targetCount].name));
        targetCount++;
    }

    void addOuiToTarget(int targetIdx, const uint8_t oui[3]) {
        if (targetIdx < 0 || targetIdx >= targetCount) return;
        if (targets[targetIdx].ouiCount >= 5) return;
        memcpy(targets[targetIdx].ouis[targets[targetIdx].ouiCount], oui, 3);
        targets[targetIdx].ouiCount++;
    }

    void clearTargets() { targetCount = 0; }
};

#endif
