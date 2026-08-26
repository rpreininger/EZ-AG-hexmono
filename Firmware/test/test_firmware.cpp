// Tests für die ESP32-Portierung, ausgeführt auf dem Rechner.
//
// Kein Testframework, keine Abhängigkeiten — das lässt sich mit einem einzigen
// Compiler-Aufruf bauen und läuft in Millisekunden.

#include <cstdio>
#include <cstring>

#include "../src/midi_filter.h"

using namespace midifilter;

namespace {

int g_failures = 0;
int g_checks = 0;
const char* g_current = "";

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FEHLGESCHLAGEN  %s  (%s, Zeile %d)\n", g_current, what, line);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void begin(const char* name) {
    g_current = name;
    std::printf("• %s\n", name);
}

// Kanäle sind hier 0-basiert: channel 5 ist MIDI-Kanal 6, also die tiefe E-Saite.
Event note_on(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t time_us) {
    Event event;
    event.channel = channel;
    event.kind = MessageKind::NoteOn;
    event.data1 = note;
    event.data2 = velocity;
    event.time_us = time_us;
    return event;
}

Event note_off(uint8_t channel, uint8_t note, uint32_t time_us) {
    Event event;
    event.channel = channel;
    event.kind = MessageKind::NoteOff;
    event.data1 = note;
    event.data2 = 0;
    event.time_us = time_us;
    return event;
}

Event control(uint8_t channel, uint8_t controller, uint8_t value, uint32_t time_us) {
    Event event;
    event.channel = channel;
    event.kind = MessageKind::ControlChange;
    event.data1 = controller;
    event.data2 = value;
    event.time_us = time_us;
    return event;
}

bool is_note_on(const Result& result, uint8_t index, uint8_t channel, uint8_t note, uint8_t velocity) {
    return index < result.count && result.events[index].kind == MessageKind::NoteOn &&
           result.events[index].channel == channel && result.events[index].data1 == note &&
           result.events[index].data2 == velocity;
}

bool is_note_off(const Result& result, uint8_t index, uint8_t channel, uint8_t note) {
    return index < result.count && result.events[index].kind == MessageKind::NoteOff &&
           result.events[index].channel == channel && result.events[index].data1 == note;
}

bool is_bend(const Result& result, uint8_t index, uint8_t channel, uint16_t value) {
    if (index >= result.count || result.events[index].kind != MessageKind::PitchBend) return false;
    const uint16_t actual = static_cast<uint16_t>(result.events[index].data1) |
                            static_cast<uint16_t>(result.events[index].data2 << 7);
    return result.events[index].channel == channel && actual == value;
}

// MARK: - Kanal-Merge und Monophonie

void test_channel_merge() {
    begin("Saitenkanäle werden auf den Ausgangskanal gelegt");
    Filter filter;
    for (uint8_t channel = 0; channel <= 5; ++channel) {
        Result result = filter.process(note_on(channel, 60, 100, channel * 1000));
        CHECK(result.verdict == Verdict::Passed);
        CHECK(result.count >= 1 && result.events[result.count - 1].channel == 0);
        filter.process(note_off(channel, 60, channel * 1000 + 500));
    }

    begin("Kanäle ausserhalb des Bereichs werden verworfen");
    Result result = filter.process(note_on(9, 60, 100, 0));
    CHECK(result.verdict == Verdict::FilteredChannel);
    CHECK(result.count == 0);
}

void test_monophonic() {
    begin("Ein neuer Anschlag übernimmt die Stimme");
    Config config;
    config.legato = false;
    Filter filter(config);
    Result result = filter.process(note_on(0, 60, 90, 0));
    CHECK(is_note_on(result, 0, 0, 60, 90));

    result = filter.process(note_on(1, 64, 80, 1000));
    CHECK(result.count == 2);
    CHECK(is_note_off(result, 0, 0, 60));
    CHECK(is_note_on(result, 1, 0, 64, 80));

    begin("Loslassen beendet den Ton, ohne auf gehaltene Noten zurückzufallen");
    result = filter.process(note_off(1, 64, 2000));
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 64));
    result = filter.process(note_off(0, 60, 3000));
    CHECK(result.count == 0);

    begin("Legato schickt die neue Note vor dem Note Off der alten");
    Config legato;
    Filter legato_filter(legato);  // legato ist Voreinstellung
    legato_filter.process(note_on(0, 60, 90, 0));
    result = legato_filter.process(note_on(1, 64, 80, 1000));
    CHECK(result.count == 2);
    CHECK(is_note_on(result, 0, 0, 64, 80));
    CHECK(is_note_off(result, 1, 0, 60));

    begin("Ohne Rückfall übernimmt jeder Anschlag, unabhängig von der Priorität");
    Config highest;
    highest.priority = NotePriority::Highest;
    highest.legato = false;
    Filter highest_filter(highest);
    highest_filter.process(note_on(0, 67, 100, 0));
    result = highest_filter.process(note_on(1, 60, 90, 1000));
    CHECK(result.count == 2 && is_note_on(result, 1, 0, 60, 90));
}

// MARK: - Rutschen auf dem Bund

void test_fret_slide() {
    begin("Griffwechsel auf klingender Saite beugt die Tonhöhe");
    Config config;
    config.fret_slide = true;
    Filter filter(config);

    // Leere tiefe E-Saite, angeschlagen mit Velocity 48.
    Result result = filter.process(note_on(5, 40, 48, 0));
    CHECK(result.count == 1 && is_note_on(result, 0, 0, 40, 48));

    // Der Griffwechsel beginnt mit dem Note Off der klingenden Note — zurückhalten.
    result = filter.process(note_off(5, 40, 1000000));
    CHECK(result.verdict == Verdict::HeldForSlide);
    CHECK(result.count == 0);

    // Die Bund-Note eine halbe Millisekunde später: ein Halbton hoch, bei Bend-Bereich
    // zwei also die halbe Strecke nach oben. Keine neue Note, keine neue Hüllkurve.
    result = filter.process(note_on(5, 41, 16, 1000500));
    CHECK(result.verdict == Verdict::Slid);
    CHECK(result.count == 1 && is_bend(result, 0, 0, 12288));

    // Zurückrutschen landet wieder genau in der Mitte.
    result = filter.process(note_off(5, 41, 2000000));
    CHECK(result.verdict == Verdict::HeldForSlide);
    result = filter.process(note_on(5, 40, 16, 2000500));
    CHECK(result.count == 1 && is_bend(result, 0, 0, 8192));
}

void test_fret_slide_release_and_range() {
    begin("Losgelassen wird die Note, die wirklich klingt");
    Config config;
    config.fret_slide = true;
    Filter filter(config);
    filter.process(note_on(5, 40, 48, 0));
    filter.process(note_off(5, 40, 1000000));
    filter.process(note_on(5, 42, 16, 1000500));

    Result result = filter.process(note_off(5, 42, 2000000));
    CHECK(result.verdict == Verdict::HeldForSlide);
    // Auf dem Kabel klingt weiterhin die 40 — die muss aus.
    result = filter.tick(2010000);
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 40));

    begin("Weiter als der Bend-Bereich: zurück auf einen echten Notenwechsel");
    Filter narrow(config);
    narrow.process(note_on(5, 40, 48, 0));
    narrow.process(note_off(5, 40, 1000000));
    result = narrow.process(note_on(5, 47, 16, 1000500));
    CHECK(result.verdict == Verdict::Slid);
    CHECK(result.count == 2 && is_note_on(result, 0, 0, 47, 48) && is_note_off(result, 1, 0, 40));

    begin("Mit weitem Bend-Bereich trägt der Bend das ganze Rutschen");
    Config wide = config;
    wide.pitch_bend_range = 12;
    Filter filter_wide(wide);
    filter_wide.process(note_on(5, 40, 48, 0));
    filter_wide.process(note_off(5, 40, 1000000));
    result = filter_wide.process(note_on(5, 47, 16, 1000500));
    CHECK(result.count == 1 && is_bend(result, 0, 0, 12970));

    begin("Vor dem nächsten Anschlag geht der Bend zurück in die Mitte");
    // Die Firmware ist voreingestellt auf Legato, die neue Note kommt also vor dem
    // Loslassen der alten. In jedem Fall steht der Bend vor der neuen Note — sonst erbt
    // sie die Verstimmung des Rutschers.
    result = filter_wide.process(note_on(4, 50, 90, 2000000));
    CHECK(result.count == 3);
    CHECK(is_bend(result, 0, 0, 8192));
    CHECK(is_note_on(result, 1, 0, 50, 90));
    CHECK(is_note_off(result, 2, 0, 40));
}

void test_fret_slide_no_follower_and_pluck() {
    begin("Ohne folgende Bund-Note wird das Loslassen nachgereicht");
    Config config;
    config.fret_slide = true;
    Filter filter(config);
    filter.process(note_on(5, 40, 48, 0));

    Result result = filter.process(note_off(5, 40, 1000000));
    CHECK(result.verdict == Verdict::HeldForSlide);
    result = filter.tick(1003000);
    CHECK(result.count == 0);
    result = filter.tick(1010000);
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 40));
    result = filter.tick(1020000);
    CHECK(result.count == 0);

    begin("Folgt ein Anschlag statt eines Griffwechsels, geht das Loslassen voraus");
    Filter second(config);
    second.process(note_on(5, 47, 92, 0));
    result = second.process(note_off(5, 47, 1000000));
    CHECK(result.verdict == Verdict::HeldForSlide);
    result = second.process(note_on(5, 47, 96, 1000500));
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.count == 2 && is_note_off(result, 0, 0, 47) && is_note_on(result, 1, 0, 47, 96));
}

void test_fret_slide_off_keeps_old_behaviour() {
    begin("Ohne Rutsch-Modus beendet der Griff den Ton wie bisher");
    Config config;  // fret_slide ist voreingestellt aus
    Filter filter(config);

    Result result = filter.process(note_on(5, 40, 48, 0));
    CHECK(result.count == 1 && is_note_on(result, 0, 0, 40, 48));
    result = filter.process(note_off(5, 40, 1000000));
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 40));
    result = filter.process(note_on(5, 47, 16, 1000500));
    CHECK(result.verdict == Verdict::SuppressedFret);
    CHECK(result.count == 0);
}

void test_panic_centres_the_bend() {
    begin("Panik beendet die Note, zentriert den Bend und schickt All Notes Off");
    Config config;
    config.fret_slide = true;
    Filter filter(config);
    filter.process(note_on(5, 40, 48, 0));

    Result result = filter.panic();
    CHECK(result.count == 3);
    CHECK(is_note_off(result, 0, 0, 40));
    CHECK(is_bend(result, 1, 0, 8192));
    CHECK(result.events[2].kind == MessageKind::ControlChange && result.events[2].data1 == 123);
}

// MARK: - Am Instrument gemessene Sequenzen

void test_measured_fret_and_pluck() {
    begin("Gemessen 2026-08-20: greifen und anschlagen");
    Config config;
    config.fret_mode = FretMode::Velocity;
    config.velocity_threshold = 32;
    config.legato = false;
    Filter filter(config);

    // Leere tiefe E-Saite, vorher angeschlagen und noch klingend.
    Result result = filter.process(note_on(5, 40, 48, 0));
    CHECK(is_note_on(result, 0, 0, 40, 48));

    // Greifen beendet die alte Note und feuert die Bund-Note mit Velocity 16.
    result = filter.process(note_off(5, 40, 10000000));
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 40));
    result = filter.process(note_on(5, 47, 16, 10000400));
    CHECK(result.verdict == Verdict::SuppressedFret);
    CHECK(result.count == 0);

    // Der Anschlag: das Note Off der Bund-Note verschwindet mit ihr.
    result = filter.process(note_off(5, 47, 11000000));
    CHECK(result.verdict == Verdict::SwallowedNoteOff);
    result = filter.process(note_on(5, 47, 92, 11000500));
    CHECK(result.count == 1 && is_note_on(result, 0, 0, 47, 92));

    // Loslassen beendet den Ton.
    result = filter.process(note_off(5, 47, 11600000));
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 47));
}

void test_quiet_pluck_survives_default_threshold() {
    begin("Leise Anschläge kommen durch, die Bund-Konstante 16 nicht");
    // Voreinstellung unverändert benutzen — genau diese Schwelle läuft auf der Box.
    Filter filter{Config()};

    // Die Bund-Note liegt immer bei exakt 16 und verschwindet.
    Result result = filter.process(note_on(5, 47, 16, 0));
    CHECK(result.verdict == Verdict::SuppressedFret);
    CHECK(result.count == 0);
    result = filter.process(note_off(5, 47, 1000000));
    CHECK(result.verdict == Verdict::SwallowedNoteOff);

    // Eine Stufe über der Bund-Konstanten — der knappste mögliche Anschlag.
    result = filter.process(note_on(5, 47, 17, 1000500));
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.count == 1 && is_note_on(result, 0, 0, 47, 17));
    result = filter.process(note_off(5, 47, 2000000));
    CHECK(result.verdict == Verdict::Passed);

    // Der leiseste tatsächlich gemessene Anschlag.
    result = filter.process(note_on(5, 47, 23, 2000500));
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.count == 1 && is_note_on(result, 0, 0, 47, 23));
}

void test_measured_hammer_on() {
    begin("Gemessen 2026-08-20: Hammer-On und Pull-Off");
    Config config;
    config.fret_mode = FretMode::Velocity;
    config.velocity_threshold = 32;
    config.suppress_hammer_ons = true;
    Filter filter(config);

    // Der Anschlag.
    Result result = filter.process(note_on(0, 72, 96, 1327000));
    CHECK(is_note_on(result, 0, 0, 72, 96));

    // Hammer-On auf den 9. Bund: angekündigt, verworfen, und das Note Off der ersetzten
    // Note verschwindet mit — sonst verstummt der Synth beim Griffwechsel.
    result = filter.process(control(0, 84, 72, 1935000));
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
    CHECK(result.count == 0);
    result = filter.process(note_on(0, 73, 96, 1936000));
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
    result = filter.process(note_off(0, 72, 1936600));
    CHECK(result.verdict == Verdict::SwallowedNoteOff);
    CHECK(result.count == 0);

    // Pull-Off zurück auf die Note, die wirklich klingt.
    result = filter.process(control(0, 84, 73, 2143000));
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
    result = filter.process(note_on(0, 72, 96, 2144000));
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
    result = filter.process(note_off(0, 73, 2145000));
    CHECK(result.verdict == Verdict::SwallowedNoteOff);

    // Das endgültige Loslassen muss durchkommen — sonst hängt die Note für immer.
    result = filter.process(note_off(0, 72, 3291000));
    CHECK(result.count == 1 && is_note_off(result, 0, 0, 72));
}

void test_hammer_on_pass_through() {
    begin("Durchgelassene Hammer-Ons nehmen das CC 84 mit");
    Config config;
    config.suppress_hammer_ons = false;
    config.legato = false;
    Filter filter(config);
    filter.process(note_on(0, 72, 96, 1327000));

    Result result = filter.process(control(0, 84, 72, 1935000));
    CHECK(result.count == 1 && result.events[0].kind == MessageKind::ControlChange);
    CHECK(result.events[0].data1 == 84 && result.events[0].data2 == 72);

    result = filter.process(note_on(0, 73, 96, 1936000));
    CHECK(result.count == 2 && is_note_off(result, 0, 0, 72) && is_note_on(result, 1, 0, 73, 96));
}

// MARK: - Zeitüberlauf

void test_timer_wraparound() {
    begin("Der Überlauf von micros() nach 71 Minuten stört nicht");
    Config config;
    config.suppress_hammer_ons = true;
    Filter filter(config);

    // Kurz vor dem Überlauf ankündigen, kurz danach die Note.
    const uint32_t before = 0xFFFFF000u;
    filter.process(note_on(0, 72, 96, before - 1000000));
    Result result = filter.process(control(0, 84, 72, before));
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
    result = filter.process(note_on(0, 73, 96, before + 5000));  // läuft über
    CHECK(result.verdict == Verdict::SuppressedHammerOn);
}

// MARK: - Byte-Parser

void test_parser_running_status() {
    begin("Running Status: Statusbyte gilt für folgende Nachrichten weiter");
    Parser parser;
    Event event;
    const uint8_t stream[] = {0x95, 40, 100, 42, 110};  // Note On ch6, dann ohne Status

    CHECK(!parser.feed(stream[0], 0, event));
    CHECK(!parser.feed(stream[1], 0, event));
    CHECK(parser.feed(stream[2], 0, event));
    CHECK(event.channel == 5 && event.kind == MessageKind::NoteOn && event.data1 == 40);

    CHECK(!parser.feed(stream[3], 0, event));
    CHECK(parser.feed(stream[4], 0, event));
    CHECK(event.channel == 5 && event.kind == MessageKind::NoteOn && event.data1 == 42 &&
          event.data2 == 110);
}

void test_parser_realtime_interleaved() {
    begin("Echtzeit-Bytes mitten in einer Nachricht stören den Parser nicht");
    Parser parser;
    Event event;
    CHECK(!parser.feed(0x90, 0, event));
    CHECK(!parser.feed(0xF8, 0, event));  // MIDI Clock mittendrin
    CHECK(!parser.feed(60, 0, event));
    CHECK(!parser.feed(0xFE, 0, event));  // Active Sensing
    CHECK(parser.feed(64, 0, event));
    CHECK(event.kind == MessageKind::NoteOn && event.data1 == 60 && event.data2 == 64);
}

void test_parser_note_on_zero_velocity() {
    begin("Note On mit Velocity 0 ist ein Note Off");
    Parser parser;
    Event event;
    parser.feed(0x90, 0, event);
    parser.feed(60, 0, event);
    CHECK(parser.feed(0, 0, event));
    CHECK(event.kind == MessageKind::NoteOff);
}

void test_parser_sysex_and_common() {
    begin("SysEx und System Common werden übersprungen");
    Parser parser;
    Event event;
    // SysEx mit Daten, die wie Noten aussehen
    parser.feed(0xF0, 0, event);
    CHECK(!parser.feed(0x7E, 0, event));
    CHECK(!parser.feed(60, 0, event));
    CHECK(!parser.feed(0xF7, 0, event));

    // Song Position Pointer schluckt zwei Datenbytes
    CHECK(!parser.feed(0xF2, 0, event));
    CHECK(!parser.feed(10, 0, event));
    CHECK(!parser.feed(20, 0, event));

    // Danach muss eine normale Nachricht wieder ankommen
    parser.feed(0x91, 0, event);
    parser.feed(55, 0, event);
    CHECK(parser.feed(80, 0, event));
    CHECK(event.channel == 1 && event.data1 == 55 && event.data2 == 80);
}

void test_to_bytes_roundtrip() {
    begin("Serialisieren und wieder zerlegen ergibt dasselbe");
    Parser parser;
    Event source = note_on(5, 60, 100, 0);
    uint8_t bytes[3];
    const uint8_t length = to_bytes(source, bytes);
    CHECK(length == 3);

    Event parsed;
    bool complete = false;
    for (uint8_t i = 0; i < length; ++i) {
        complete = parser.feed(bytes[i], 0, parsed);
    }
    CHECK(complete);
    CHECK(parsed.channel == 5 && parsed.kind == MessageKind::NoteOn && parsed.data1 == 60 &&
          parsed.data2 == 100);

    begin("Program Change ist zwei Bytes lang");
    Event program;
    program.channel = 3;
    program.kind = MessageKind::ProgramChange;
    program.data1 = 12;
    CHECK(to_bytes(program, bytes) == 2);
}

}  // namespace

int main() {
    std::printf("\nMidiFilter Firmware — Tests\n\n");

    test_channel_merge();
    test_monophonic();
    test_measured_fret_and_pluck();
    test_measured_hammer_on();
    test_quiet_pluck_survives_default_threshold();
    test_fret_slide();
    test_fret_slide_release_and_range();
    test_fret_slide_no_follower_and_pluck();
    test_fret_slide_off_keeps_old_behaviour();
    test_panic_centres_the_bend();
    test_hammer_on_pass_through();
    test_timer_wraparound();
    test_parser_running_status();
    test_parser_realtime_interleaved();
    test_parser_note_on_zero_velocity();
    test_parser_sysex_and_common();
    test_to_bytes_roundtrip();

    std::printf("\n%d Prüfungen, %d fehlgeschlagen\n\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
