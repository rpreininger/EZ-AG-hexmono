// Die Klangrechnung ohne Plattform: reSID, Modulation, Register, Ein- und Ausblenden.
//
// Box und Mac-App übersetzen genau diese Datei. Drumherum hat jede Seite ihre eigene
// Hülle — auf der Box Warteschlange, I2S und Lastmessung (engine.cpp), in der App eine
// Warteschlange und der Audio-Callback. Beide rufen nur apply() und render_block() auf,
// deshalb klingt ein Preset in der App so wie auf der Box.
//
// Alles hier gehört der einen Audio-Aufgabe. Nichts davon ist für mehrere Threads
// gebaut — Befehle von aussen kommen über die Warteschlange der jeweiligen Hülle.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "params.h"

namespace synth {

constexpr uint32_t kSampleRate = 44100;

namespace core {

/// Samples je Block; die Modulation läuft einmal je Block (≈ 344,5-mal pro Sekunde).
constexpr int kBlock = 128;

enum class CommandType : uint8_t { SetParam, NoteOn, NoteOff, Bend, AllOff, Mute };

struct Command {
    CommandType type;
    uint8_t a;
    int16_t b;
};

/// Setzt reSID und den ganzen Zustand zurück und übernimmt die Werte des Klangs.
void begin(const Patch& patch);

/// Ein Befehl, zwischen zwei Blöcken.
void apply(const Command& command);

/// Rechnet einen Block: Modulation, SID-Register, reSID, Ein- und Ausblenden. Liefert
/// den höchsten Ausschlag in diesem Block (0..32768).
int32_t render_block(int16_t out[kBlock]);

/// true, wenn ganz ausgeblendet ist (nach einem Mute-Befehl).
bool silent();

}  // namespace core
}  // namespace synth
