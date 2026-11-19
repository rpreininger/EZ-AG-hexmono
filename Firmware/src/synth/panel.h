// Das Gerät als vierte Flaschen-Taschen-Tafel: an Stelle des iPixel-Panels (32×16).
//
// Der Tab5 (oder die Mac-App) schickt seinen Ausschnitt der Leinwand als UDP-Pakete an
// Port 1337, jedes ein PPM-Bild (P6) mit dem Versatz im Kommentar „#FT: x y“ — der Tab5
// zerlegt das Bild in Streifen zu drei Zeilen. Hier landet alles in einem Bild von
// 32×16 Punkten; das Display vergrößert es (display.cpp), auf dem Netz bleibt es klein.
//
// Nur auf Geräten mit Display (board.h); sonst sind alle Aufrufe leer.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace synth {
namespace panel {

constexpr int kWidth = 32;
constexpr int kHeight = 16;
constexpr uint16_t kPort = 1337;

/// Lauscht ab jetzt auf UDP 1337. Nach dem Start des WLAN aufrufen.
void begin();

/// Kopiert das aktuelle Bild (RGB, kWidth × kHeight × 3 Bytes) und liefert einen Zähler,
/// der bei jeder Änderung weiterläuft.
uint32_t snapshot(uint8_t* rgb);

/// millis() des letzten gültigen Pakets, 0 = noch keins.
uint32_t last_frame_ms();

/// Ein Testbild für `seconds` Sekunden, so als käme es vom Tab5 — derselbe Weg durch den
/// PPM-Leser wie echte Pakete. 0 hält es an.
void test(uint32_t seconds);

struct Stats {
    uint32_t packets = 0;  // gültige Pakete
    uint32_t bad = 0;      // verworfen: kein P6, falsche Größe, abgeschnitten
};
Stats stats();

}  // namespace panel
}  // namespace synth
