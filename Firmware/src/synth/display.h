// Das 4,3″-Display des Guition-Boards (480×272).
//
// Zwei Bilder:
//   Tafel     — im Konzert die vierte Tafel: das 32×16-Bild vom Tab5, jeder Punkt als
//               Block von 15×15 Pixeln (480×240, oben und unten 16 Pixel schwarz)
//   SID-Seite — Platz und Name des Presets, die drei Live-Regler, Pegel, MIDI, Netz
// Die SID-Seite erscheint, sobald jemand an einem Encoder dreht oder drückt, und
// bleibt vier Sekunden nach dem letzten Dreh. Kommt seit zwei Sekunden kein Bild vom
// Tab5 (Probe, kein Konzert), bleibt sie ganz stehen.
//
// Gezeichnet wird in einer eigenen Aufgabe auf Kern 0; loop() reicht nur den Stand
// herüber (set_view). Auf Geräten ohne Display sind alle Aufrufe leer.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace synth {
namespace display {

/// Was die SID-Seite zeigt. loop() füllt es aus, die Anzeige-Aufgabe zeichnet nur, was
/// sich gegenüber dem letzten Bild geändert hat.
/// Ohne Voreinstellungen, damit memset/memcmp darauf erlaubt sind — vor dem Ausfüllen
/// mit memset leeren (auch die Füllbytes), set_view() vergleicht byteweise.
struct View {
    int slot;          // ab 0
    char name[24];
    bool dirty;
    uint8_t knob[3];   // Index in kParams
    int16_t value[3];  // Rohwert
    int16_t peak;      // höchster Ausschlag der letzten Sekunde, bis 32767
    bool midi;         // gerade MIDI empfangen
    bool encoders;     // Encoder-Board da
    char net[24];      // IP-Adresse oder Name des eigenen WLAN
};

void begin();
void set_view(const View& view);

/// Jemand hat am Encoder gedreht oder gedrückt: SID-Seite zeigen.
void wake();

enum class Mode : uint8_t { Auto, Sid, Panel, Colors };
/// Für Tests an der Konsole (`screen ...`); Auto ist der Normalbetrieb.
void set_mode(Mode mode);
Mode mode();
bool present();

}  // namespace display
}  // namespace synth
