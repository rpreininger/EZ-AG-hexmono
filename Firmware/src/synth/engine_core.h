// Die Klangrechnung ohne Plattform: reSID, Modulation, Register, Ein- und Ausblenden.
//
// Box und Mac-App übersetzen genau diese Datei. Drumherum hat jede Seite ihre eigene
// Hülle — auf der Box Warteschlange, I2S und Lastmessung (engine.cpp), in der App eine
// Warteschlange und der Audio-Callback. Beide rufen nur apply() und render_block() auf,
// deshalb klingt ein Preset in der App so wie auf der Box.
//
// Eine Instanz gehört genau einer Audio-Aufgabe. Nichts davon ist für mehrere Threads
// gebaut — Befehle von aussen kommen über die Warteschlange der jeweiligen Hülle.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "params.h"
#include "sid.h"

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

/// Ein SID mit seiner Modulation. Box und Mac-App haben je eine Instanz, ein Plug-in eine
/// je geladenem Exemplar.
class Core {
public:
    /// Setzt reSID und den ganzen Zustand zurück und übernimmt die Werte des Klangs.
    void begin(const Patch& patch);

    /// Ein Befehl, zwischen zwei Blöcken.
    void apply(const Command& command);

    /// Rechnet einen Block: Modulation, SID-Register, reSID, Ein- und Ausblenden. Liefert
    /// den höchsten Ausschlag in diesem Block (0..32768).
    int32_t render_block(int16_t out[kBlock]);

    /// true, wenn ganz ausgeblendet ist (nach einem Mute-Befehl).
    bool silent() const;

private:
    static constexpr uint8_t kRegisters = 0x19;

    struct Lfo {
        float phase = 0;
        float value = 0;
        uint32_t random = 0x9E3779B9;
    };

    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    struct Envelope {
        Stage stage = Stage::Idle;
        float level = 0;
    };

    struct Voice {
        uint8_t held[8] = {};  // gedrückte Noten, die letzte oben
        uint8_t held_count = 0;
        uint8_t note = 60;
        uint8_t velocity = 100;
        bool gate = false;
        bool retrigger = false;  // einen Block Gate aus, damit die SID-Hüllkurve neu startet
        bool has_pitch = false;  // vor der ersten Note gibt es nichts, von dem man gleitet
        float pitch = 60;        // klingende Tonhöhe in Halbtönen, gleitet zur Note
        float since_note = 0;    // Sekunden seit dem Anschlag, für den LFO-Einsatz
    };

    int16_t v(ParamId id) const;
    float value_of(ParamId id) const;
    void write_reg(uint8_t address, uint8_t value);
    void forget(uint8_t note);
    void hold(uint8_t note);
    void play(uint8_t note, uint8_t velocity, bool attack);
    void press(uint8_t note, uint8_t velocity);
    void release(uint8_t note);
    void update_lfos(float scale[2]);
    void update_envelope();
    void update_glide();
    void write_registers(const float lfo_scale[2]);
    void update_chip();

    reSID::SID sid_;
    int16_t values_[kParamCount] = {};
    int chip_ = -1;
    uint8_t shadow_[kRegisters] = {};
    bool shadow_valid_[kRegisters] = {};
    Lfo lfo_[2];
    Envelope env_;
    Voice voice_;
    float bend_ = 0;  // -1..1
    int32_t gain_ = 0;
    int32_t gain_target_ = 32768;
};

}  // namespace core
}  // namespace synth
