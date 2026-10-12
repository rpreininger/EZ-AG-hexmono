#include "presets.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

namespace synth {
namespace presets {
namespace {

using P = ParamId;

constexpr const char* kTempPath = "/patch-tmp.json";

Patch g_slots[kSlots];
bool g_used[kSlots];
bool g_from_file[kSlots];

void path_for(int slot, char (&path)[24]) { snprintf(path, sizeof(path), "/patch-%02d.json", slot); }

/// Setzt einen Wert in seiner Einheit (Millisekunden, Hertz, Cent).
void set_curve(Patch& patch, P id, float value) {
    patch.values[static_cast<size_t>(id)] = raw_for(param(id), value);
}

// MARK: - Werksklänge

bool factory(int slot, Patch& p) {
    switch (slot) {
        case 0:
            init_patch(p, "Sägezahn-Lead");
            p.set(P::O2_WAVE, 2);
            p.set(P::O2_FINE, 8);
            p.set(P::F_CUT, 900);
            p.set(P::F_RES, 7);
            p.set(P::F_ENV, 35);
            set_curve(p, P::ME_D, 600);
            p.set(P::ME_S, 40);
            set_curve(p, P::ME_R, 400);
            set_curve(p, P::L1_PITCH, 25);
            set_curve(p, P::L1_DELAY, 450);
            set_curve(p, P::P_GLIDE, 40);
            return true;
        case 1:
            init_patch(p, "PWM-Bass");
            p.set(P::O1_WAVE, 3);
            p.set(P::O1_PW, 1400);
            p.set(P::O2_WAVE, 0);
            p.set(P::O3_WAVE, 3);
            p.set(P::O3_SEMI, -12);
            p.set(P::O3_PW, 2600);
            for (P id : {P::O1_D, P::O3_D}) p.set(id, 8);
            for (P id : {P::O1_S, P::O3_S}) p.set(id, 9);
            for (P id : {P::O1_R, P::O3_R}) p.set(id, 4);
            set_curve(p, P::L2_RATE, 0.6f);
            p.set(P::L2_PW, 35);
            p.set(P::F_CUT, 450);
            p.set(P::F_RES, 5);
            p.set(P::F_ENV, 45);
            set_curve(p, P::ME_D, 250);
            p.set(P::ME_S, 15);
            p.set(P::P_TRANSP, -12);
            return true;
        case 2:
            // Oszillator 1 klingt nicht, gibt aber den Takt für den Sync von 2 vor. Die
            // Hüllkurve zieht Oszillator 2 nach oben — das typische Sync-Kreischen.
            init_patch(p, "Sync-Lead");
            p.set(P::O1_WAVE, 0);
            p.set(P::O2_WAVE, 2);
            p.set(P::O2_SYNC, 1);
            p.set(P::O2_SEMI, 7);
            p.set(P::ME_PITCH, 19);
            p.set(P::ME_PTGT, 2);
            set_curve(p, P::ME_D, 700);
            p.set(P::ME_S, 0);
            set_curve(p, P::ME_R, 300);
            p.set(P::F_CUT, 1500);
            p.set(P::F_RES, 4);
            p.set(P::F_ENV, 10);
            set_curve(p, P::L1_PITCH, 20);
            set_curve(p, P::L1_DELAY, 500);
            p.set(P::P_VOL, 15);  // nur eine klingende Stimme
            return true;
        case 3:
            // Ringmodulation braucht Dreieck. Oszillator 3 liefert nur die Modulation.
            init_patch(p, "Ringmod-Glocke");
            p.set(P::O1_WAVE, 1);
            p.set(P::O1_RING, 1);
            p.set(P::O1_D, 11);
            p.set(P::O1_S, 0);
            p.set(P::O1_R, 10);
            p.set(P::O2_WAVE, 0);
            p.set(P::O3_WAVE, 0);
            p.set(P::O3_SEMI, 19);
            p.set(P::O3_FINE, 13);
            p.set(P::F_MODE, 0);
            p.set(P::P_VOL, 15);
            return true;
        case 4:
            init_patch(p, "Fette Sägen");
            p.set(P::O1_FINE, -12);
            p.set(P::O2_WAVE, 2);
            p.set(P::O2_FINE, 12);
            p.set(P::O3_WAVE, 2);
            p.set(P::O3_SEMI, -12);
            p.set(P::F_CUT, 1000);
            p.set(P::F_RES, 3);
            p.set(P::F_ENV, 25);
            p.set(P::P_VOL, 12);
            return true;
        case 5:
            init_patch(p, "Filter-Wah");
            p.set(P::O2_PW, 1800);
            p.set(P::F_MODE, 2);
            p.set(P::F_CUT, 600);
            p.set(P::F_RES, 13);
            p.set(P::F_ENV, 0);
            set_curve(p, P::L2_RATE, 3.0f);
            p.set(P::L2_CUT, 70);
            p.set(P::P_VOL, 15);
            return true;
        case 6:
            init_patch(p, "Pluck");
            p.set(P::O1_WAVE, 3);
            p.set(P::O1_PW, 1000);
            p.set(P::O1_D, 7);
            p.set(P::O1_S, 0);
            p.set(P::O1_R, 6);
            p.set(P::O2_WAVE, 2);
            p.set(P::O2_SEMI, 12);
            p.set(P::O2_D, 6);
            p.set(P::O2_S, 0);
            p.set(P::O2_R, 5);
            p.set(P::F_CUT, 250);
            p.set(P::F_RES, 9);
            p.set(P::F_ENV, 75);
            p.set(P::F_VEL, 40);
            set_curve(p, P::ME_D, 180);
            p.set(P::ME_S, 0);
            set_curve(p, P::ME_R, 150);
            return true;
        case 7:
            init_patch(p, "Init");
            return true;
        default:
            return false;
    }
}

// MARK: - JSON

void fill(JsonDocument& doc, const Patch& patch) {
    doc["fmt"] = 1;
    doc["name"] = patch.name;
    JsonObject values = doc["v"].to<JsonObject>();
    for (size_t i = 0; i < kParamCount; ++i) values[kParams[i].key] = patch.values[i];
}

/// Unbekannte Schlüssel werden übergangen, fehlende behalten die Voreinstellung — so
/// bleiben alte Dateien lesbar, wenn Parameter dazukommen.
bool read_file(const char* path, Patch& patch) {
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return false;

    init_patch(patch, doc["name"] | "Ohne Namen");
    JsonObjectConst values = doc["v"].as<JsonObjectConst>();
    for (size_t i = 0; i < kParamCount; ++i) {
        JsonVariantConst value = values[kParams[i].key];
        if (value.is<int>()) patch.values[i] = clamp_value(i, value.as<int>());
    }
    return true;
}

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

}  // namespace presets
}  // namespace synth
