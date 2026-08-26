// Portierung von EZAGFilter.swift auf C++17 für den ESP32.
//
// Gleiches Verhalten, gleiche Namen, aber ohne dynamische Speicherverwaltung, ohne
// Ausnahmen und mit uint32_t-Mikrosekunden statt double-Sekunden — Fließkomma will man
// auf einem Mikrocontroller nicht im MIDI-Pfad haben.
//
// Die Zeitvergleiche laufen über vorzeichenlose Differenzen und überstehen dadurch den
// Überlauf von micros() nach etwa 71 Minuten von selbst.

#pragma once

#include <stdint.h>

#include "midi_event.h"

namespace midifilter {

enum class NotePriority : uint8_t { Last, Highest, Lowest };

/// Wie eine Bund-Note von einem Anschlag unterschieden wird.
///
/// Die Messung an der EZ-AG am 2026-08-20 hat ergeben: Bund-Noten haben immer exakt
/// Velocity 16, Anschläge liegen zwischen 48 und 96. Deshalb ist Velocity die
/// Voreinstellung. Der Blitzer-Modus der Mac-App fehlt hier bewusst — die Messung hat
/// gezeigt, dass Bund-Noten gehalten werden und keine kurzen Blitzer sind.
enum class FretMode : uint8_t { Off, Velocity, Retrigger };

enum class Verdict : uint8_t {
    Passed,
    SuppressedFret,
    SuppressedHammerOn,
    SwallowedNoteOff,
    FilteredChannel,
    DroppedOther,
    /// Note Off der klingenden Note zurückgehalten — es könnte ein Griffwechsel folgen.
    HeldForSlide,
    /// Griffwechsel auf klingender Saite: die Tonhöhe ist gewandert.
    Slid,
};

struct Config {
    uint8_t lowest_input_channel = 0;
    uint8_t highest_input_channel = 5;
    uint8_t output_channel = 0;
    bool pass_other_channels = false;

    bool monophonic = true;
    NotePriority priority = NotePriority::Last;
    bool legato = true;
    bool retrigger_same_note = true;
    bool fallback_to_held_note = false;

    FretMode fret_mode = FretMode::Velocity;
    uint8_t velocity_threshold = 16;
    bool velocity_exact_match = false;
    uint32_t retrigger_window_us = 60000;
    bool allow_first_note_on_idle_string = true;

    /// Ein Griffwechsel auf einer noch klingenden Saite verstimmt die Note, statt die
    /// Bund-Note zu verwerfen — das Rutschen auf dem Bund ändert die Tonhöhe, wie an
    /// einer echten Saite. Die Velocity des Anschlags bleibt erhalten, es klingt ja
    /// weiterhin derselbe Anschlag. Braucht eine arbeitende Bund-Erkennung.
    bool fret_slide = false;
    /// Beim Rutschen klingt der angeschlagene Ton weiter — die Tonhöhe zu verschieben ist
    /// also ein Pitch Bend und keine neue Note. Die Hüllkurve wird dabei überhaupt nicht
    /// angefasst. Passt das Intervall nicht in den Bend-Bereich, wird auf einen echten
    /// Notenwechsel zurückgefallen: eine neu angerissene Note ist besser als eine falsche.
    bool slide_uses_pitch_bend = true;
    /// Halbtöne, die der Synth pro Richtung beugt. Muss mit der Einstellung am Synth
    /// übereinstimmen, sonst kommt jedes Rutschen im falschen Intervall heraus. Zwei ist
    /// die fast überall übliche Voreinstellung.
    uint8_t pitch_bend_range = 2;
    /// So lange wird das Note Off einer klingenden Note zurückgehalten, um zu sehen, ob
    /// eine Bund-Note folgt. Die EZ-AG schickt das Paar rund eine halbe Millisekunde
    /// auseinander; genau so viel Nachklang kostet es bei einem echten Loslassen.
    uint32_t slide_window_us = 6000;

    bool suppress_hammer_ons = true;
    uint32_t hammer_on_window_us = 20000;

    int8_t transpose = 0;
    bool pass_other_messages = true;
};

/// Ergebnis einer verarbeiteten Nachricht. Mehr als drei Ereignisse entstehen nie —
/// Note Off der alten Note, Note On der neuen, und beim Wechsel des Ausgangskanals ein
/// zusätzliches Aufräum-Note-Off.
struct Result {
    static constexpr uint8_t kMaxEvents = 4;
    Event events[kMaxEvents];
    uint8_t count = 0;
    Verdict verdict = Verdict::Passed;

    void add(const Event& event) {
        if (count < kMaxEvents) {
            events[count++] = event;
        }
    }
};

class Filter {
public:
    Filter() = default;
    explicit Filter(const Config& config) : config_(config) {}

    const Config& config() const { return config_; }

    /// Setzt eine neue Konfiguration und liefert die Ereignisse, die nötig sind, damit
    /// unter einer klingenden Note nicht der Kanal weggezogen wird.
    Result set_config(const Config& config);

    Result process(const Event& event);

    /// Treibt das Zurückhalten der Note Offs im Rutsch-Modus. Muss in der Hauptschleife
    /// laufen, solange `fret_slide` an ist; liefert die Loslass-Ereignisse, die keine
    /// Bund-Note für sich beansprucht hat.
    Result tick(uint32_t now_us);

    /// Alles aus, Zustand zurücksetzen.
    Result panic();

    void reset();

private:
    static constexpr uint8_t kChannels = 16;
    static constexpr uint8_t kStackSize = 16;

    struct StringState {
        uint32_t held[4] = {0, 0, 0, 0};        // Bitfeld über 128 Noten
        uint32_t suppressed[4] = {0, 0, 0, 0};  // deren Note Off mitverworfen wird

        // Für die Retrigger-Erkennung genügt das letzte Note Off je Saite: die EZ-AG
        // schickt es unmittelbar vor dem zugehörigen Note On.
        uint8_t last_note_off_note = 0xFF;
        uint32_t last_note_off_us = 0;

        bool has_portamento = false;
        uint32_t portamento_us = 0;

        bool legato_guard = false;
        uint32_t legato_guard_until_us = 0;

        // Rutsch-Modus: das zurückgehaltene Note Off der klingenden Note.
        bool has_pending_off = false;
        uint8_t pending_off_note = 0;
        uint32_t pending_off_until_us = 0;
    };

    struct HeldNote {
        uint8_t channel = 0;
        uint8_t source = 0;
        uint8_t out = 0;
        uint8_t velocity = 0;
    };

    Result note_on(const Event& event);
    Result note_off(const Event& event);
    bool is_fret_press(const Event& event, bool string_was_silent) const;
    bool can_detect_fret_press() const;
    void slide(uint8_t channel, uint8_t from, uint8_t to, Result& out);
    uint16_t bend_value(int semitones) const;
    void centre_bend(Result& out);

    void emit_note_on(uint8_t channel, uint8_t note, uint8_t velocity, Result& out);
    void emit_note_off(uint8_t channel, uint8_t note, Result& out);
    void update_voice(const HeldNote* attacked, Result& out);
    const HeldNote* pick_winner(const HeldNote* attacked) const;

    Event make(MessageKind kind, uint8_t data1, uint8_t data2) const;
    uint8_t transposed(uint8_t note) const;

    Config config_;
    StringState strings_[kChannels];
    HeldNote stack_[kStackSize];
    uint8_t stack_count_ = 0;
    HeldNote sounding_;
    bool has_sounding_ = false;
    /// Halbtöne, um die der Ausgangskanal gerade gebeugt ist — ein Rutschen lässt das
    /// stehen, bis etwas Neues angeschlagen wird.
    int8_t bend_ = 0;
};

}  // namespace midifilter
