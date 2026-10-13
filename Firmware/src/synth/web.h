// WLAN-Zugangspunkt, Webserver und WebSocket für den Klang-Editor.
//
// Läuft auf Kern 0. Befehle aus dem Browser landen in einer Warteschlange und werden
// in loop() abgearbeitet, damit der Zustand nur von einer Aufgabe geändert wird; ebenso
// eine hochgeladene Sicherungsdatei. Nur
// `ping` und `audio` beantwortet der Webserver gleich selbst — die betreffen allein
// den einen Browser.

#pragma once

#include <Arduino.h>

namespace synth {
namespace web {

constexpr char kSsid[] = "SID-Synth";
constexpr char kPassword[] = "sid12345";
constexpr char kHostname[] = "sid";  // http://sid.local

/// Wird aus loop() mit jeder Befehlszeile aufgerufen. Darf die Zeile zerlegen.
using CommandHandler = void (*)(char* line);

/// Wird aus loop() mit einer Sicherungsdatei aufgerufen, die über POST /api/restore kam.
using RestoreHandler = void (*)(const char* text, size_t length);

void begin(CommandHandler handler, RestoreHandler restore);

/// In loop(): eingegangene Befehle abarbeiten, getrennte Browser aufräumen.
void poll();

/// An alle offenen Browser.
void broadcast(const String& text);

/// Die Parametertabelle als JSON, so wie der Editor sie unter /api/schema abholt.
void write_schema(Print& out);

size_t clients();

}  // namespace web
}  // namespace synth
