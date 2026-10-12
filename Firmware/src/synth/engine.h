// Die Klangerzeugung: reSID auf Kern 1, Ausgabe über I2S an einen PCM5102A.
//
// Alle Aufrufe hier sind von jeder Aufgabe aus erlaubt. Sie legen nur einen Befehl in
// eine Warteschlange; die Audio-Aufgabe holt ihn vor dem nächsten Block ab, spätestens
// nach 2,9 ms. Damit sieht reSID nie einen halb geänderten Zustand.

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "params.h"

namespace synth {

/// Verdrahtung des DAC-Moduls (PCM5102A: BCK, LCK, DIN; SCK auf GND).
constexpr int kI2sBckPin = 12;
constexpr int kI2sWsPin = 13;
constexpr int kI2sDoutPin = 14;

constexpr uint32_t kSampleRate = 44100;

/// Mithören im Browser, solange kein DAC angeschlossen ist: halbe Abtastrate, Mono.
class PreviewRing {
public:
    static constexpr size_t kSize = 8192;  // gut 370 ms

    void set_enabled(bool on) { enabled_.store(on); }
    bool enabled() const { return enabled_.load(); }

    /// Nur aus der Audio-Aufgabe. Mittelt je zwei Samples.
    void write(const int16_t* samples, int count);

    uint32_t write_position() const { return write_.load(std::memory_order_acquire); }
    int16_t at(uint32_t position) const { return data_[position % kSize]; }

private:
    int16_t data_[kSize] = {};
    std::atomic<uint32_t> write_{0};
    std::atomic<bool> enabled_{false};
};

struct EngineStats {
    float load_avg = 0;       // Prozent eines Kerns, gemittelt über eine Sekunde
    float load_max = 0;       // schlechtester Block in dieser Sekunde
    uint32_t max_gap_us = 0;  // längste Pause zwischen zwei Blöcken
    uint32_t worst_gap_us = 0;
    int16_t peak = 0;         // höchster Ausschlag in dieser Sekunde, bis 32767
    uint32_t dropped_commands = 0;
};

namespace engine {

/// Startet reSID, I2S und die Audio-Aufgabe mit diesem Klang.
void begin(const Patch& patch);

void set_param(ParamId id, int16_t value);
void set_param(size_t index, int16_t value);
void load(const Patch& patch);

void note_on(uint8_t note, uint8_t velocity);
void note_off(uint8_t note);
/// -8192 bis 8191, wie es im MIDI-Pitchbend steht.
void pitch_bend(int value);
void all_notes_off();

/// Blendet in 5 ms aus oder wieder ein. Vor jedem Schreiben in den Flash: dabei stehen
/// beide Kerne still, und ohne Ausblenden gäbe es einen hörbaren Knacks.
void set_muted(bool muted);
/// true, sobald nach set_muted(true) wirklich nichts mehr herauskommt.
bool silent();

EngineStats stats();
/// Höchster Ausschlag seit dem letzten Aufruf, bis 32767 — für kurze Klänge, die in
/// den Sekundenwerten von stats() zwischen zwei Messungen fallen können.
int16_t take_peak();
PreviewRing& preview();

}  // namespace engine
}  // namespace synth
