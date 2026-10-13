#include "presets.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace synth {
namespace presets {
namespace {

constexpr const char* kTempPath = "/patch-tmp.json";

Patch g_slots[kSlots];
bool g_used[kSlots];
bool g_from_file[kSlots];

void path_for(int slot, char (&path)[24]) { snprintf(path, sizeof(path), "/patch-%02d.json", slot); }

// MARK: - JSON

void fill(JsonDocument& doc, const Patch& patch) {
    doc["fmt"] = 1;
    doc["name"] = patch.name;
    JsonObject values = doc["v"].to<JsonObject>();
    for (size_t i = 0; i < kParamCount; ++i) values[kParams[i].key] = patch.values[i];
}

/// Unbekannte Schlüssel werden übergangen, fehlende behalten die Voreinstellung — so
/// bleiben alte Dateien lesbar, wenn Parameter dazukommen.
void from_json(JsonObjectConst object, Patch& patch) {
    init_patch(patch, object["name"] | "Ohne Namen");
    JsonObjectConst values = object["v"].as<JsonObjectConst>();
    for (size_t i = 0; i < kParamCount; ++i) {
        JsonVariantConst value = values[kParams[i].key];
        if (value.is<int>()) patch.values[i] = clamp_value(i, value.as<int>());
    }
}

bool read_file(const char* path, Patch& patch) {
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return false;
    from_json(doc.as<JsonObjectConst>(), patch);
    return true;
}

/// Eine volle Sicherung braucht beim Lesen einige zehn KB. Die gehören in den PSRAM, nicht
/// in den knappen internen Speicher, den WLAN und Webserver brauchen.
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* pointer) override { heap_caps_free(pointer); }
    void* reallocate(void* pointer, size_t size) override {
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM);
    }
};
PsramAllocator g_psram;

}  // namespace

bool begin() {
    const bool mounted = LittleFS.begin(true);  // beim allerersten Start wird formatiert

    // Einmal durchs Verzeichnis statt 32-mal nachfragen — exists() meldet jede fehlende
    // Datei als Fehler auf der Konsole.
    bool on_disk[kSlots] = {};
    if (mounted) {
        File root = LittleFS.open("/");
        for (File file = root.openNextFile(); file; file = root.openNextFile()) {
            const char* name = file.name();
            const char* slash = strrchr(name, '/');
            int slot = -1;
            if (sscanf(slash != nullptr ? slash + 1 : name, "patch-%d.json", &slot) == 1 &&
                slot >= 0 && slot < kSlots) {
                on_disk[slot] = true;
            }
        }
    }

    for (int slot = 0; slot < kSlots; ++slot) {
        char path[24];
        path_for(slot, path);
        g_from_file[slot] = on_disk[slot] && read_file(path, g_slots[slot]);
        g_used[slot] = g_from_file[slot] || factory(slot, g_slots[slot]);
        if (!g_used[slot]) init_patch(g_slots[slot], "");
    }
    return mounted;
}

const Patch& get(int slot) { return g_slots[slot < 0 || slot >= kSlots ? 0 : slot]; }

bool used(int slot) { return slot >= 0 && slot < kSlots && g_used[slot]; }

bool from_file(int slot) { return slot >= 0 && slot < kSlots && g_from_file[slot]; }

int step(int from, int direction) {
    for (int i = 1; i <= kSlots; ++i) {
        const int slot = ((from + direction * i) % kSlots + kSlots) % kSlots;
        if (g_used[slot]) return slot;
    }
    return from;
}

bool save(int slot, const Patch& patch) {
    if (slot < 0 || slot >= kSlots) return false;
    File file = LittleFS.open(kTempPath, "w");
    if (!file) return false;
    const bool written = write_json(file, patch) > 0;
    file.close();
    char path[24];
    path_for(slot, path);
    if (!written || !LittleFS.rename(kTempPath, path)) return false;

    g_slots[slot] = patch;
    g_used[slot] = true;
    g_from_file[slot] = true;
    return true;
}

bool erase(int slot) {
    if (!from_file(slot)) return false;
    char path[24];
    path_for(slot, path);
    if (!LittleFS.remove(path)) return false;
    g_from_file[slot] = false;
    g_used[slot] = factory(slot, g_slots[slot]);
    if (!g_used[slot]) init_patch(g_slots[slot], "");
    return true;
}

size_t write_json(Print& out, const Patch& patch) {
    JsonDocument doc;
    fill(doc, patch);
    return serializeJson(doc, out);
}

void write_backup(Print& out) {
    out.print("{\"fmt\":1,\"presets\":[");
    bool first = true;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (!g_used[slot]) continue;
        if (!first) out.print(',');
        first = false;
        JsonDocument doc;
        doc["slot"] = slot + 1;
        fill(doc, g_slots[slot]);
        serializeJson(doc, out);
    }
    out.print("]}");
}

int read_backup(const char* text, size_t length, Patch (&patches)[kSlots],
                bool (&found)[kSlots]) {
    for (bool& f : found) f = false;
    JsonDocument doc(&g_psram);
    if (deserializeJson(doc, text, length) || (doc["fmt"] | 1) != 1) return -1;
    JsonArrayConst entries = doc["presets"].as<JsonArrayConst>();
    if (entries.isNull()) return -1;

    int count = 0;
    for (JsonObjectConst entry : entries) {
        const int slot = (entry["slot"] | 0) - 1;
        if (slot < 0 || slot >= kSlots || !entry["v"].is<JsonObjectConst>()) continue;
        from_json(entry, patches[slot]);
        if (!found[slot]) ++count;
        found[slot] = true;  // kommt ein Platz zweimal vor, gilt der letzte
    }
    return count;
}

}  // namespace presets
}  // namespace synth
