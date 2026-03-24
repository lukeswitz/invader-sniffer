#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoJson.h>
#include <SD_MMC.h>

struct TargetOUI {
    uint8_t ouiCount;
    uint8_t ouis[5][3];
    char name[64];
};

// ---------- Hardcoded special targets (always active) ----------

enum class SpecialHit { NONE, EVIL_BIRD, RAYBAN };

static const uint8_t EVIL_BIRD_OUIS[5][3] = {
    {0xD4, 0x11, 0xD6},  // Axis Communications
    {0x00, 0x17, 0x3D},  // Axis Communications
    {0xE0, 0x0A, 0xF6},  // Axis Communications
    {0x00, 0x25, 0xDF},  // Axis Communications
    {0xB4, 0x1E, 0x52},  // Axis Communications
};

static const uint8_t RAYBAN_OUIS[4][3] = {
    {0x7C, 0x2A, 0x9E},  // Meta Ray-Ban
    {0xCC, 0x66, 0x0A},  // Meta Ray-Ban
    {0xF4, 0x03, 0x43},  // Meta Ray-Ban
    {0x5C, 0xE9, 0x1E},  // Meta Ray-Ban
};

// Mask the top 2 bits of the first byte before comparing — they encode the
// BLE random-address type and are not part of the OUI.  This lets the spoofer
// (which must set those bits to 11 for static-random addresses) still trigger
// detection for OUIs whose natural first byte doesn't already have bits 7:6=11.
static inline uint8_t ouiByte0(uint8_t b) { return b & 0x3Fu; }

inline SpecialHit checkSpecialOUI(const uint8_t* bda) {
    if (!bda) return SpecialHit::NONE;
    for (int i = 0; i < 5; i++) {
        if (ouiByte0(bda[0]) == ouiByte0(EVIL_BIRD_OUIS[i][0]) &&
            bda[1] == EVIL_BIRD_OUIS[i][1] &&
            bda[2] == EVIL_BIRD_OUIS[i][2])
            return SpecialHit::EVIL_BIRD;
    }
    for (int i = 0; i < 4; i++) {
        if (ouiByte0(bda[0]) == ouiByte0(RAYBAN_OUIS[i][0]) &&
            bda[1] == RAYBAN_OUIS[i][1] &&
            bda[2] == RAYBAN_OUIS[i][2])
            return SpecialHit::RAYBAN;
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