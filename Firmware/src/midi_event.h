// Nachrichtenmodell und Byte-Parser für rohes DIN-MIDI.
//
// Auf dem Mac nimmt CoreMIDI uns das Zerlegen des Byte-Stroms ab. An einer UART kommt
// MIDI so an, wie es der Standard vorsieht: mit Running Status, mit dazwischengestreuten
// Echtzeit-Bytes und mit SysEx-Blöcken, die man überspringen muss. Genau das macht diese
// Datei — und nichts sonst, damit sie sich auf dem Rechner testen lässt.

#pragma once

#include <stdint.h>

namespace midifilter {

enum class MessageKind : uint8_t {
    NoteOff,
    NoteOn,
    PolyPressure,
    ControlChange,
    ProgramChange,
    ChannelPressure,
    PitchBend,
};

struct Event {
    uint8_t channel = 0;  // 0..15, angezeigt wird 1..16
    MessageKind kind = MessageKind::NoteOff;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint32_t time_us = 0;

    bool is_note_on() const { return kind == MessageKind::NoteOn; }
    bool is_note_off() const { return kind == MessageKind::NoteOff; }
    uint8_t note() const { return data1; }
    uint8_t velocity() const { return data2; }
};

/// Schreibt die Nachricht als Bytes. Rückgabe ist die Länge (2 oder 3).
uint8_t to_bytes(const Event& event, uint8_t out[3]);

/// Zerlegt einen MIDI-Byte-Strom in einzelne Kanalnachrichten.
///
/// Zustandsbehaftet und bewusst ohne Speicherverwaltung — eine Instanz pro Eingang.
class Parser {
public:
    /// Ein Byte hineingeben. Liefert true, sobald eine vollständige Kanalnachricht
    /// vorliegt, und schreibt sie nach `out`.
    bool feed(uint8_t byte, uint32_t time_us, Event& out);

    void reset();

private:
    uint8_t status_ = 0;  // Running Status: bleibt über Nachrichten hinweg stehen
    uint8_t data_[2] = {0, 0};
    uint8_t data_count_ = 0;
    uint8_t system_data_remaining_ = 0;
    bool in_sysex_ = false;
};

}  // namespace midifilter
