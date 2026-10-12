// Presets: 32 Plätze, jeder eine JSON-Datei im LittleFS, alle beim Start in den RAM.
//
// Ein Platz ohne Datei zeigt den Werksklang, falls es für ihn einen gibt — so überlebt
// ein eigener Klang jede neue Firmware, und die Werksklänge lassen sich trotzdem
// verbessern. Geschrieben wird nur auf ausdrücklichen Wunsch: ein Schreibzugriff hält
// beide Kerne bis zu 40 ms an, siehe engine::set_muted().

#pragma once

#include <Print.h>

#include "params.h"

namespace synth {
namespace presets {

constexpr int kSlots = 32;

/// Bindet das Dateisystem ein und lädt alle Plätze. false, wenn das nicht ging — dann
/// gibt es nur die Werksklänge, und Speichern schlägt fehl.
bool begin();

const Patch& get(int slot);
/// Hat der Platz einen Klang — eigenen oder Werksklang?
bool used(int slot);
/// Ist es ein selbst gespeicherter Klang?
bool from_file(int slot);

/// Der nächste belegte Platz in Richtung `direction` (+1 oder -1), rundherum.
int step(int from, int direction);

/// Schreibt erst in eine Hilfsdatei und benennt dann um — das ist bei LittleFS atomar,
/// ein Stromausfall mittendrin lässt den alten Stand stehen.
bool save(int slot, const Patch& patch);

/// Löscht den eigenen Klang. Der Platz zeigt danach wieder den Werksklang oder ist leer.
bool erase(int slot);

/// Ein Klang als JSON, so wie er in der Datei steht. Liefert die Anzahl Bytes.
size_t write_json(Print& out, const Patch& patch);
/// Alle belegten Plätze als ein JSON-Dokument, zum Sichern auf dem Rechner.
void write_backup(Print& out);

}  // namespace presets
}  // namespace synth
