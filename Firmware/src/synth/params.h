// Alle Klangparameter an einer Stelle.
//
// Engine, Presets und Web-Editor lesen dieselbe Tabelle: ein neuer Parameter ist eine
// neue Zeile in SYNTH_PARAMS. Der Editor baut sich aus `/api/schema` selbst auf, Presets
// speichern jeden Wert unter seinem Schlüssel — fehlt einer in einer alten Datei, gilt
// die Voreinstellung.
//
// Aufbau wie bei der SidStation: drei Oszillatoren auf derselben Note, ein Filter, eine
// Hüllkurve für Filter und Tonhöhe, zwei LFOs. Die Hüllkurven der Oszillatoren sind die
// echten des SID (Werte 0–15 wie im Datenblatt), alles andere rechnet die Engine etwa
// 340-mal pro Sekunde selbst aus.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace synth {

enum class Kind : uint8_t { Range, Choice, Toggle };

/// Wie der Editor einen Rohwert anzeigt. Die Namen gehen klein geschrieben ins Schema.
enum class Format : uint8_t {
    Int,        // die Zahl selbst
    Semitones,  // Halbtöne mit Vorzeichen
    Cents,      // Cent mit Vorzeichen
    Percent,    // Rohwert ist der Prozentwert
    Pulse,      // 0..4095 als Prozent
    SidAttack,  // SID-Hüllkurve 0..15, Zeiten laut Datenblatt
    SidDecay,
    Exp,        // exponentiell von lo bis hi
    Exp0,       // 0 heisst aus, sonst exponentiell von lo bis hi
};

struct Param {
    const char* key;
    const char* label;
    const char* group;
    Kind kind;
    int16_t min;
    int16_t max;
    int16_t def;
    Format format;
    const char* choices;  // Kind::Choice: Einträge durch '|' getrennt
    float lo;             // Format::Exp und Exp0
    float hi;
    const char* unit;
};

#define SYNTH_WAVES "Aus|Dreieck|Sägezahn|Puls|Rauschen|Dreieck+Säge|Dreieck+Puls|Säge+Puls|Alle drei"
#define SYNTH_TARGETS "Alle|Osz. 1|Osz. 2|Osz. 3"

// X(id, key, label, group, kind, min, max, def, format, choices, lo, hi, unit)
//
// SRC ist der Oszillator, von dem Sync und Ringmodulation kommen — beim SID fest
// verdrahtet: 1 hängt an 3, 2 an 1, 3 an 2.
#define SYNTH_OSC_PARAMS(X, N, SRC, WAVE)                                                    \
    X(O##N##_WAVE, "o" #N ".wave", "Wellenform", "Oszillator " #N, Choice, 0, 8, WAVE, Int,   \
      SYNTH_WAVES, 0, 0, "")                                                                 \
    X(O##N##_SEMI, "o" #N ".semi", "Transponieren", "Oszillator " #N, Range, -24, 24, 0,      \
      Semitones, "", 0, 0, "")                                                               \
    X(O##N##_FINE, "o" #N ".fine", "Feinstimmung", "Oszillator " #N, Range, -50, 50, 0,       \
      Cents, "", 0, 0, "")                                                                   \
    X(O##N##_PW, "o" #N ".pw", "Pulsbreite", "Oszillator " #N, Range, 0, 4095, 2048, Pulse,   \
      "", 0, 0, "")                                                                          \
    X(O##N##_A, "o" #N ".a", "Attack", "Oszillator " #N, Range, 0, 15, 0, SidAttack, "", 0,   \
      0, "")                                                                                 \
    X(O##N##_D, "o" #N ".d", "Decay", "Oszillator " #N, Range, 0, 15, 9, SidDecay, "", 0, 0,  \
      "")                                                                                    \
    X(O##N##_S, "o" #N ".s", "Sustain", "Oszillator " #N, Range, 0, 15, 12, Int, "", 0, 0,    \
      "")                                                                                    \
    X(O##N##_R, "o" #N ".r", "Release", "Oszillator " #N, Range, 0, 15, 6, SidDecay, "", 0,   \
      0, "")                                                                                 \
    X(O##N##_SYNC, "o" #N ".sync", "Sync mit Osz. " #SRC, "Oszillator " #N, Toggle, 0, 1, 0, \
      Int, "", 0, 0, "")                                                                     \
    X(O##N##_RING, "o" #N ".ring", "Ringmod. mit Osz. " #SRC, "Oszillator " #N, Toggle, 0,   \
      1, 0, Int, "", 0, 0, "")                                                               \
    X(O##N##_FILT, "o" #N ".filt", "Durch den Filter", "Oszillator " #N, Toggle, 0, 1, 1,     \
      Int, "", 0, 0, "")

#define SYNTH_LFO_PARAMS(X, N, RATE)                                                         \
    X(L##N##_WAVE, "l" #N ".wave", "Form", "LFO " #N, Choice, 0, 3, 0, Int,                  \
      "Dreieck|Sägezahn|Rechteck|Zufall", 0, 0, "")                                          \
    X(L##N##_RATE, "l" #N ".rate", "Tempo", "LFO " #N, Range, 0, 127, RATE, Exp, "", 0.05f,  \
      30.0f, "Hz")                                                                           \
    X(L##N##_DELAY, "l" #N ".delay", "Einsatz nach", "LFO " #N, Range, 0, 127, 0, Exp0, "",  \
      10.0f, 4000.0f, "ms")                                                                  \
    X(L##N##_PITCH, "l" #N ".pitch", "→ Tonhöhe", "LFO " #N, Range, 0, 127, 0, Exp0, "",     \
      1.0f, 2400.0f, "ct")                                                                   \
    X(L##N##_PTGT, "l" #N ".ptgt", "Tonhöhe von", "LFO " #N, Choice, 0, 3, 0, Int,           \
      SYNTH_TARGETS, 0, 0, "")                                                               \
    X(L##N##_PW, "l" #N ".pw", "→ Pulsbreite", "LFO " #N, Range, 0, 100, 0, Percent, "", 0,  \
      0, "")                                                                                 \
    X(L##N##_CUT, "l" #N ".cut", "→ Cutoff", "LFO " #N, Range, 0, 100, 0, Percent, "", 0, 0, \
      "")

#define SYNTH_PARAMS(X)                                                                      \
    SYNTH_OSC_PARAMS(X, 1, 3, 2)                                                             \
    SYNTH_OSC_PARAMS(X, 2, 1, 3)                                                             \
    SYNTH_OSC_PARAMS(X, 3, 2, 0)                                                             \
    X(F_CUT, "f.cut", "Cutoff", "Filter", Range, 0, 2047, 1100, Int, "", 0, 0, "")            \
    X(F_RES, "f.res", "Resonanz", "Filter", Range, 0, 15, 6, Int, "", 0, 0, "")               \
    X(F_MODE, "f.mode", "Typ", "Filter", Choice, 0, 7, 1, Int,                                \
      "Aus|Tiefpass|Bandpass|Hochpass|Kerbe|Tief+Band|Band+Hoch|Alle", 0, 0, "")             \
    X(F_ENV, "f.env", "Hüllkurve", "Filter", Range, -100, 100, 30, Percent, "", 0, 0, "")     \
    X(F_KEY, "f.key", "Folgt der Tonhöhe", "Filter", Range, 0, 100, 0, Percent, "", 0, 0, "") \
    X(F_VEL, "f.vel", "Anschlagstärke", "Filter", Range, -100, 100, 20, Percent, "", 0, 0,    \
      "")                                                                                    \
    X(ME_A, "me.a", "Attack", "Hüllkurve", Range, 0, 127, 0, Exp0, "", 1.0f, 5000.0f, "ms")   \
    X(ME_D, "me.d", "Decay", "Hüllkurve", Range, 0, 127, 70, Exp0, "", 1.0f, 10000.0f, "ms")  \
    X(ME_S, "me.s", "Sustain", "Hüllkurve", Range, 0, 100, 30, Percent, "", 0, 0, "")         \
    X(ME_R, "me.r", "Release", "Hüllkurve", Range, 0, 127, 60, Exp0, "", 1.0f, 10000.0f,      \
      "ms")                                                                                  \
    X(ME_PITCH, "me.pitch", "→ Tonhöhe", "Hüllkurve", Range, -24, 24, 0, Semitones, "", 0, 0, \
      "")                                                                                    \
    X(ME_PTGT, "me.ptgt", "Tonhöhe von", "Hüllkurve", Choice, 0, 3, 0, Int, SYNTH_TARGETS, 0, \
      0, "")                                                                                 \
    SYNTH_LFO_PARAMS(X, 1, 93)                                                               \
    SYNTH_LFO_PARAMS(X, 2, 49)                                                               \
    X(P_VOL, "p.vol", "Lautstärke", "Spielen", Range, 0, 15, 13, Int, "", 0, 0, "")           \
    X(P_GLIDE, "p.glide", "Glide", "Spielen", Range, 0, 127, 0, Exp0, "", 5.0f, 2000.0f,      \
      "ms")                                                                                  \
    X(P_LEGATO, "p.legato", "Legato", "Spielen", Toggle, 0, 1, 0, Int, "", 0, 0, "")          \
    X(P_BEND, "p.bend", "Pitchbend-Bereich", "Spielen", Range, 0, 12, 2, Semitones, "", 0, 0, \
      "")                                                                                    \
    X(P_TRANSP, "p.transp", "Transponieren", "Spielen", Range, -24, 24, 0, Semitones, "", 0,  \
      0, "")                                                                                 \
    X(P_CHIP, "p.chip", "Chip", "Spielen", Choice, 0, 1, 0, Int, "6581|8580", 0, 0, "")

#define SYNTH_PARAM_ENUM(id, ...) id,
enum class ParamId : uint8_t { SYNTH_PARAMS(SYNTH_PARAM_ENUM) Count };
#undef SYNTH_PARAM_ENUM

constexpr size_t kParamCount = static_cast<size_t>(ParamId::Count);

extern const Param kParams[kParamCount];

inline const Param& param(ParamId id) { return kParams[static_cast<size_t>(id)]; }

/// Index in kParams, oder -1 bei unbekanntem Schlüssel.
int find_param(const char* key);

/// Begrenzt einen Wert auf den Bereich des Parameters.
int16_t clamp_value(size_t index, long value);

/// Der Wert in seiner Einheit, für Format::Exp und Exp0 (Exp0 liefert bei 0 genau 0).
float curve(const Param& p, int16_t raw);

/// Umkehrung von curve(): der Rohwert, der einer Zeit oder Frequenz am nächsten kommt.
int16_t raw_for(const Param& p, float value);

/// Name des Formats, so wie ihn das Schema für den Editor ausgibt.
const char* format_name(Format format);

constexpr size_t kNameLength = 24;

struct Patch {
    char name[kNameLength];
    int16_t values[kParamCount];

    int16_t get(ParamId id) const { return values[static_cast<size_t>(id)]; }
    void set(ParamId id, long value) {
        const size_t index = static_cast<size_t>(id);
        values[index] = clamp_value(index, value);
    }
};

/// Alle Werte auf die Voreinstellung.
void init_patch(Patch& patch, const char* name);

}  // namespace synth
