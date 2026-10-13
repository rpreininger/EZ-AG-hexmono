// Die Schnittstelle der Mac-App zur Klangerzeugung der Box.
//
// Dahinter steckt derselbe C++-Code wie in der Firmware: reSID, engine_core, params,
// Werksklänge, Schema und der EZ-AG-Filter. Diese Datei ist nur die Hülle, die auf der
// Box engine.cpp und synth_main.cpp heißt — eine Warteschlange für Befehle, der
// Audio-Callback, die Lastmessung und MIDI.
//
// Threads: sid_render() nur aus dem Audio-Callback. Alles andere darf von jedem Thread
// kommen; Noten und Parameter landen wie auf der Box in einer Warteschlange und werden
// vor dem nächsten 128er-Block übernommen.

#ifndef SID_HOST_H
#define SID_HOST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - Parametertabelle (params.h)

int sid_param_count(void);
const char* sid_param_key(int index);
int sid_param_default(int index);
/// Index des Schlüssels oder -1.
int sid_find_param(const char* key);
/// Begrenzt auf min/max des Parameters, wie die Box beim Laden und bei `set`.
int sid_clamp_value(int index, long value);
int sid_param_min(int index);
int sid_param_max(int index);
/// Ein Schalter (Kind::Toggle), kein Bereich und keine Auswahl.
bool sid_param_is_toggle(int index);

/// Anzahl der Presetplätze (32).
int sid_slots(void);
/// Größe des Namensfelds in Byte, mit abschließender Null (24).
int sid_name_capacity(void);
/// Kürzt einen Namen genau wie die Box (UTF-8, Steuerzeichen raus). `out` braucht
/// sid_name_capacity() Bytes.
void sid_set_name(const char* text, char* out);

/// Werksklang des Platzes (ab 0). `name` braucht sid_name_capacity() Bytes, `values`
/// sid_param_count() Einträge. false, wenn der Platz keinen Werksklang hat.
bool sid_factory(int slot, char* name, int16_t* values);

/// Das Schema für den Editor, Wort für Wort wie /api/schema der Box.
const char* sid_schema_json(void);

// MARK: - Klangerzeugung (engine.h)

/// Setzt alles zurück und übernimmt die Werte. Nur aufrufen, solange kein Audio läuft.
void sid_engine_begin(const int16_t* values);

void sid_set_param(int index, int value);
/// Alle Werte auf einmal, wie engine::load() auf der Box.
void sid_load(const int16_t* values);
void sid_note_on(int note, int velocity);
void sid_note_off(int note);
/// -8192 bis 8191.
void sid_pitch_bend(int value);
void sid_all_notes_off(void);

/// Nur aus dem Audio-Callback: `frames` Samples mono, 44,1 kHz. Gerechnet wird intern
/// immer in ganzen 128er-Blöcken wie auf der Box, ein Rest wartet auf den nächsten Aufruf.
void sid_render(float* out, int frames);

typedef struct {
    float load_avg;     // Prozent der Echtzeit, gemittelt über eine Sekunde
    float load_max;     // schlechtester Block dieser Sekunde
    int peak;           // höchster Ausschlag dieser Sekunde, bis 32767
    uint32_t dropped;   // verworfene Befehle, Warteschlange voll
    uint32_t midi_in;   // empfangene MIDI-Nachrichten
} SIDStats;

SIDStats sid_stats(void);
/// Höchster Ausschlag seit dem letzten Aufruf, bis 32767.
int sid_take_peak(void);

// MARK: - MIDI (synth_main.cpp: play)

/// Eine vollständige MIDI-1.0-Kanalnachricht (Status + 1 oder 2 Datenbytes), von jedem
/// Thread. Geht wie auf der Box durch den EZ-AG-Filter, falls eingeschaltet. Liefert die
/// Nummer eines Program Change (0..127), der durchkam — den Platz lädt der Aufrufer —,
/// sonst -1.
int sid_midi(uint8_t status, uint8_t data1, uint8_t data2);

/// EZ-AG-Filter vor der Engine an oder aus. Beim Umschalten verstummt alles.
void sid_set_ezag(bool on);

#ifdef __cplusplus
}
#endif

#endif
