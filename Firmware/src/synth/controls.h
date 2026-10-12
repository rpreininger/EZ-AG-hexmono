// Das Encoder-Board: Adafruit I2C Quad Rotary Encoder („NeoRotary 4", Produkt 5752).
//
// Vier Drehregler mit Taster und je einer LED, über ein STEMMA-QT-Kabel am I2C. Was die
// Regler tun, entscheidet synth_main.cpp; hier wird nur gelesen und geleuchtet.
//
// Eine eigene Aufgabe auf Kern 0 fragt das Board 50-mal pro Sekunde ab. Jede Abfrage
// wartet auf den I2C-Bus — das soll weder die Klangerzeugung noch loop() aufhalten.
// Gefundenes landet in einer Warteschlange, loop() holt es mit poll() ab. Fehlt das
// Board, sucht die Aufgabe alle zwei Sekunden danach: es darf später eingesteckt werden.
//
// Ansteuerung wie im Chromakey-Controller (D:\Developer\Arduino\ESP32), dort auf der
// Hardware geprüft: Adresse 0x49, Taster an den seesaw-Pins 12/14/17/9, LEDs an Pin 18,
// und das Board zählt im Uhrzeigersinn rückwärts.

#pragma once

#include <stdint.h>

namespace synth {
namespace controls {

constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;
constexpr int kKnobs = 4;

struct Event {
    enum class Type : uint8_t { Turn, Press };
    Type type;
    uint8_t knob;   // 0..3, von links
    int16_t delta;  // Rasten, im Uhrzeigersinn positiv
};

void begin();

/// In loop(): liefert true, solange Ereignisse anstehen.
bool poll(Event& event);

/// Ist das Board angeschlossen und ansprechbar?
bool present();

/// Farbe einer LED. Von jeder Aufgabe aus erlaubt, geschrieben wird beim nächsten Abfragen.
void set_led(int knob, uint8_t red, uint8_t green, uint8_t blue);

}  // namespace controls
}  // namespace synth
