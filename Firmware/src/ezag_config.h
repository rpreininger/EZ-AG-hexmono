// Die an der EZ-AG gemessenen Filtereinstellungen — eine Quelle für Filter-Box und Synth.

#pragma once

#include "midi_filter.h"

/// Die am 2026-08-20 an der EZ-AG gemessenen Werte. Siehe Firmware/README.md.
inline midifilter::Config ezag_config() {
    midifilter::Config config;
    config.lowest_input_channel = 0;   // Kanal 1: hohe E-Saite
    config.highest_input_channel = 5;  // Kanal 6: tiefe E-Saite
    config.output_channel = 0;

    config.monophonic = true;
    config.priority = midifilter::NotePriority::Last;
    config.legato = true;
    config.retrigger_same_note = true;
    config.fallback_to_held_note = false;

    // Bund-Noten kommen immer mit Velocity 16. Für Anschläge stand hier ursprünglich
    // 48 bis 96 und damit eine Schwelle von 32 — am 2026-08-25 an der laufenden Box
    // gemessen reichen leise Anschläge aber bis auf 25 herunter, sechs davon fielen in
    // zehn Sekunden stumm heraus. 20 liegt sauber zwischen 16 und 25.
    config.fret_mode = midifilter::FretMode::Velocity;
    config.velocity_threshold = 20;

    // Hammer-On und Pull-Off werden mit CC 84 angekündigt.
    config.suppress_hammer_ons = true;
    return config;
}
