// Die Box mit eingebautem SID-Synth.
//
//   Gitarre → C2MIDI Pro → ESP32-S3: [EZ-AG-Filter] → SID-Engine → I2S → PCM5102A
//                                                    ↘ MIDI OUT → SidStation
//
// Gespielt wird mit der Casio DG-20, deren MIDI geht direkt in die Engine. Der Filter
// für die Yamaha EZ-AG (sechs Saitenkanäle, Bundgriffe) lässt sich im Editor
// zuschalten; die Weiterleitung an MIDI OUT ebenfalls ab.
//
// Der Klang wird im Browser bearbeitet: mit dem WLAN „SID-Synth" verbinden (Passwort
// sid12345), dann http://sid.local oder http://192.168.4.1 öffnen. Dieselben Befehle
// wie der Browser versteht die serielle Konsole — zum Prüfen ohne WLAN, etwa
// `on 60 100`, `set f.cut 400`, `off 60`, `stat`.
//
// Hier liegt der Zustand des Editors: der Klang, der gerade spielt (mit allen noch
// nicht gespeicherten Änderungen), sein Platz und die Einstellungen. Geändert wird er
// nur in loop() — Browser und Konsole reichen ihre Befehle dorthin.

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "engine.h"
#include "ezag_config.h"
#include "midi_filter.h"
#include "midi_ports.h"
#include "controls.h"
#include "params.h"
#include "presets.h"
#include "web.h"

#include <algorithm>

namespace {

using namespace synth;

constexpr int kActivityLedPin = LED_BUILTIN;
constexpr uint8_t kActivityLedBrightness = 6;  // gedämpftes Grün, siehe main.cpp

/// Regler 1–3 des Encoder-Boards: welcher Parameter, bis jemand es im Editor ändert.
constexpr ParamId kDefaultKnobs[3] = {ParamId::F_CUT, ParamId::F_RES, ParamId::L1_RATE};
/// Farben der Regler-LEDs; die Helligkeit zeigt, wo der Wert im Bereich steht.
constexpr uint32_t kKnobColors[3] = {0xFF3000, 0x20FF40, 0x3070FF};
constexpr int kPresetKnob = 3;

UsbHostMidiPort g_port;
midifilter::Filter g_filter(ezag_config());
Preferences g_settings;

Patch g_edit;
int g_slot = 0;
bool g_dirty = false;
bool g_thru = true;   // Noten auch an MIDI OUT, also an die SidStation
bool g_ezag = false;  // EZ-AG-Filter vor der Engine; aus = alles geht direkt hindurch
bool g_trace = false; // eingehendes MIDI auf die Konsole, nur zur Fehlersuche
int g_start_slot = 0;
size_t g_knob_params[3];
bool g_controls_present = false;

uint32_t g_midi_in = 0;
uint32_t g_param_changes = 0;
uint32_t g_led_off_at_ms = 0;

// MARK: - Ausgabe

void append_json_string(String& out, const char* text) {
    out += '"';
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            out += '\\';
            out += *c;
        } else if (static_cast<uint8_t>(*c) >= 0x20) {
            out += *c;
        }
    }
    out += '"';
}

String state_message() {
    String s;
    s.reserve(2800);
    s += "state {\"slot\":";
    s += g_slot;
    s += ",\"start\":";
    s += g_start_slot;
    s += ",\"thru\":";
    s += g_thru ? 1 : 0;
    s += ",\"ezag\":";
    s += g_ezag ? 1 : 0;
    s += ",\"dirty\":";
    s += g_dirty ? "true" : "false";
    s += ",\"name\":";
    append_json_string(s, g_edit.name);
    s += ",\"v\":[";
    for (size_t i = 0; i < kParamCount; ++i) {
        if (i > 0) s += ',';
        s += g_edit.values[i];
    }
    s += "],\"names\":[";
    for (int slot = 0; slot < presets::kSlots; ++slot) {
        if (slot > 0) s += ',';
        append_json_string(s, presets::used(slot) ? presets::get(slot).name : "");
    }
    s += "],\"used\":[";
    for (int slot = 0; slot < presets::kSlots; ++slot) {
        if (slot > 0) s += ',';
        s += presets::used(slot) ? 1 : 0;
    }
    s += "],\"own\":[";
    for (int slot = 0; slot < presets::kSlots; ++slot) {
        if (slot > 0) s += ',';
        s += presets::from_file(slot) ? 1 : 0;
    }
    s += "],\"knobs\":[";
    for (int k = 0; k < 3; ++k) {
        if (k > 0) s += ',';
        append_json_string(s, kParams[g_knob_params[k]].key);
    }
    s += "],\"enc\":";
    s += g_controls_present ? "true" : "false";
    s += '}';
    return s;
}

void broadcast_state() { web::broadcast(state_message()); }

/// Meldung an die Konsole und als Einblendung in jeden offenen Browser.
void notify(const String& text) {
    Serial.println(text);
    web::broadcast("msg " + text);
}

String status_line() {
    const EngineStats stats = engine::stats();
    char text[200];
    snprintf(text, sizeof(text),
             "{\"load\":%.1f,\"max\":%.1f,\"gap\":%.1f,\"peak\":%d,\"usb\":%s,\"midi\":%u,"
             "\"heap\":%u,\"drop\":%u}",
             stats.load_avg, stats.load_max, stats.worst_gap_us / 1000.0, stats.peak,
             g_port.is_connected() ? "true" : "false", static_cast<unsigned>(g_midi_in),
             static_cast<unsigned>(ESP.getFreeHeap() / 1024),
             static_cast<unsigned>(stats.dropped_commands));
    return String(text);
}

// MARK: - Flash

/// Jeder Schreibzugriff auf den Flash hält beide Kerne an, bis zu 40 ms — länger, als
/// der Ausgabepuffer reicht. Deshalb vorher ausblenden, dann ist es nur eine kurze Pause.
template <typename Work>
bool while_muted(Work work) {
    engine::set_muted(true);
    const uint32_t start = millis();
    while (!engine::silent() && millis() - start < 60) delay(1);
    const bool ok = work();
    engine::set_muted(false);
    return ok;
}

// MARK: - Befehle

/// Weicht der Klang vom gespeicherten Stand seines Platzes ab? Genau verglichen, damit
/// ein Regler, der auf seinen alten Wert zurückgedreht wird, nichts als geändert meldet.
void update_dirty() {
    const Patch& stored = presets::get(g_slot);
    g_dirty = !presets::used(g_slot) ||
              memcmp(stored.values, g_edit.values, sizeof(g_edit.values)) != 0 ||
              strcmp(stored.name, g_edit.name) != 0;
}

/// Ein Parameterwert aus Browser, Konsole oder Encoder — alle nehmen diesen Weg.
void apply_param(size_t index, long value) {
    const int16_t v = clamp_value(index, value);
    g_edit.values[index] = v;
    engine::set_param(index, v);
    ++g_param_changes;
    update_dirty();
    web::broadcast(String("set ") + kParams[index].key + ' ' + v + ' ' + (g_dirty ? 1 : 0));
}

void load_slot(int slot) {
    if (!presets::used(slot)) return;
    g_edit = presets::get(slot);
    g_slot = slot;
    g_dirty = false;
    engine::load(g_edit);
    broadcast_state();
    Serial.printf("PRESET %02d %s\n", slot + 1, g_edit.name);
}

void save_slot(int slot) {
    if (slot < 0 || slot >= presets::kSlots) return;
    const uint32_t start = millis();
    const bool ok = while_muted([slot] { return presets::save(slot, g_edit); });
    if (ok) {
        g_slot = slot;
        g_dirty = false;
    }
    broadcast_state();
    notify(ok ? String("Gespeichert auf Platz ") + (slot + 1) + " (" + (millis() - start) + " ms)"
              : String("Speichern fehlgeschlagen"));
}

// MARK: - Encoder-Board

/// Regler 1–3: den zugewiesenen Parameter verstellen. Rund 128 Rasten für den ganzen
/// Bereich; schnell gedreht geht es bis zu viermal so weit je Raste. Regler 4: Presets.
void turn_knob(int knob, int delta) {
    if (knob == kPresetKnob) {
        int slot = g_slot;
        for (int i = 0; i < abs(delta); ++i) slot = presets::step(slot, delta > 0 ? 1 : -1);
        if (slot != g_slot) load_slot(slot);
        return;
    }
    const size_t index = g_knob_params[knob];
    const Param& p = kParams[index];
    if (p.kind == Kind::Toggle) {
        apply_param(index, delta > 0 ? 1 : 0);
        return;
    }
    const int step = std::max(1, (p.max - p.min) / 128);
    const int speed = std::min(4, abs(delta));
    apply_param(index, g_edit.values[index] + static_cast<long>(delta) * step * speed);
}

/// Druck auf Regler 1–3: dieser Parameter zurück auf den gespeicherten Wert.
/// Druck auf Regler 4: alle Änderungen am Preset verwerfen.
void press_knob(int knob) {
    if (knob == kPresetKnob) {
        load_slot(g_slot);
        return;
    }
    const size_t index = g_knob_params[knob];
    apply_param(index, presets::get(g_slot).values[index]);
}

/// Helligkeit = Stellung im Wertebereich, der Preset-Regler leuchtet orange, solange
/// ungespeicherte Änderungen da sind. Schreibt nur, wenn sich eine Farbe ändert.
void update_knob_leds() {
    for (int k = 0; k < 3; ++k) {
        const size_t index = g_knob_params[k];
        const Param& p = kParams[index];
        float level = static_cast<float>(g_edit.values[index] - p.min) / (p.max - p.min);
        level = 0.06f + 0.94f * level;
        const uint32_t color = kKnobColors[k];
        controls::set_led(k, static_cast<uint8_t>(((color >> 16) & 0xFF) * level),
                          static_cast<uint8_t>(((color >> 8) & 0xFF) * level),
                          static_cast<uint8_t>((color & 0xFF) * level));
    }
    if (g_dirty) {
        controls::set_led(kPresetKnob, 255, 90, 0);
    } else {
        controls::set_led(kPresetKnob, 140, 60, 255);
    }
}

void poll_controls() {
    controls::Event event;
    while (controls::poll(event)) {
        if (event.type == controls::Event::Type::Turn) {
            turn_knob(event.knob, event.delta);
        } else {
            press_knob(event.knob);
        }
    }
    if (controls::present() != g_controls_present) {
        g_controls_present = controls::present();
        broadcast_state();
    }
    update_knob_leds();
}

void load_knob_settings() {
    for (int k = 0; k < 3; ++k) {
        char key[8];
        snprintf(key, sizeof(key), "knob%d", k);
        const int index = find_param(g_settings.getString(key, "").c_str());
        g_knob_params[k] = index >= 0 ? static_cast<size_t>(index)
                                      : static_cast<size_t>(kDefaultKnobs[k]);
    }
}

/// Kürzt auf die Länge des Namensfelds, ohne ein UTF-8-Zeichen zu zerschneiden.
void set_name(const char* text) {
    char name[kNameLength];
    size_t n = 0;
    for (const char* c = text; *c != '\0' && n < kNameLength - 1; ++c) {
        if (static_cast<uint8_t>(*c) >= 0x20) name[n++] = *c;
    }
    while (n > 0 && (static_cast<uint8_t>(name[n - 1]) & 0xC0) == 0x80) --n;  // Folgebytes
    if (n > 0 && (static_cast<uint8_t>(name[n - 1]) & 0xC0) == 0xC0) --n;     // Startbyte
    name[n] = '\0';
    memcpy(g_edit.name, name, n + 1);
}

/// Eine Befehlszeile aus Browser oder Konsole, siehe web/index.html.
void handle(char* line) {
    char* args = strchr(line, ' ');
    if (args != nullptr) {
        *args++ = '\0';
    } else {
        args = line + strlen(line);
    }
    const char* command = line;

    if (strcmp(command, "set") == 0) {
        char* value = strchr(args, ' ');
        if (value == nullptr) return;
        *value++ = '\0';
        const int index = find_param(args);
        if (index < 0) {
            notify(String("Unbekannter Parameter ") + args);
            return;
        }
        apply_param(static_cast<size_t>(index), strtol(value, nullptr, 10));
    } else if (strcmp(command, "on") == 0) {
        char* velocity = strchr(args, ' ');
        engine::note_on(static_cast<uint8_t>(atoi(args)),
                        velocity != nullptr ? static_cast<uint8_t>(atoi(velocity + 1)) : 100);
    } else if (strcmp(command, "off") == 0) {
        engine::note_off(static_cast<uint8_t>(atoi(args)));
    } else if (strcmp(command, "name") == 0) {
        set_name(args);
        update_dirty();
        broadcast_state();
    } else if (strcmp(command, "load") == 0) {
        load_slot(atoi(args));
    } else if (strcmp(command, "next") == 0) {
        load_slot(presets::step(g_slot, 1));
    } else if (strcmp(command, "prev") == 0) {
        load_slot(presets::step(g_slot, -1));
    } else if (strcmp(command, "revert") == 0) {
        load_slot(g_slot);
    } else if (strcmp(command, "save") == 0) {
        save_slot(*args != '\0' ? atoi(args) : g_slot);
    } else if (strcmp(command, "erase") == 0) {
        const int slot = atoi(args);
        if (!presets::from_file(slot)) return;
        const bool ok = while_muted([slot] { return presets::erase(slot); });
        update_dirty();  // der Klang spielt weiter, ist aber womöglich nirgends mehr gespeichert
        broadcast_state();
        notify(ok ? String("Platz ") + (slot + 1) + " gelöscht" : String("Löschen fehlgeschlagen"));
    } else if (strcmp(command, "init") == 0) {
        init_patch(g_edit, "Neuer Klang");
        engine::load(g_edit);
        update_dirty();
        broadcast_state();
    } else if (strcmp(command, "panic") == 0) {
        engine::all_notes_off();
        const midifilter::Result result = g_filter.panic();
        for (uint8_t i = 0; i < result.count && g_thru; ++i) g_port.write(result.events[i]);
    } else if (strcmp(command, "thru") == 0) {
        g_thru = atoi(args) != 0;
        while_muted([] { return g_settings.putBool("thru", g_thru) > 0; });
        broadcast_state();
    } else if (strcmp(command, "ezag") == 0) {
        // Beim Umschalten nichts hängen lassen: der Filter kennt die alten Noten nicht.
        engine::all_notes_off();
        g_filter.reset();
        g_ezag = atoi(args) != 0;
        while_muted([] { return g_settings.putBool("ezag", g_ezag) > 0; });
        broadcast_state();
    } else if (strcmp(command, "knob") == 0) {
        // knob <1..3> <key>: welchen Parameter ein Regler verstellt
        char* key = strchr(args, ' ');
        const int knob = atoi(args) - 1;
        const int index = key != nullptr ? find_param(key + 1) : -1;
        if (knob < 0 || knob > 2 || index < 0) return;
        g_knob_params[knob] = static_cast<size_t>(index);
        char name[8];
        snprintf(name, sizeof(name), "knob%d", knob);
        while_muted([name, index] { return g_settings.putString(name, kParams[index].key) > 0; });
        broadcast_state();
    } else if (strcmp(command, "turn") == 0) {
        // turn <1..4> <rasten> und press <1..4>: wie am Encoder-Board, zum Prüfen ohne Board
        char* delta = strchr(args, ' ');
        const int knob = atoi(args) - 1;
        if (knob >= 0 && knob < controls::kKnobs && delta != nullptr) turn_knob(knob, atoi(delta + 1));
    } else if (strcmp(command, "press") == 0) {
        const int knob = atoi(args) - 1;
        if (knob >= 0 && knob < controls::kKnobs) press_knob(knob);
    } else if (strcmp(command, "miditrace") == 0) {
        g_trace = atoi(args) != 0;
        Serial.printf("MIDI-Mitschnitt %s\n", g_trace ? "an" : "aus");
    } else if (strcmp(command, "start") == 0) {
        const int slot = atoi(args);
        if (presets::used(slot)) {
            g_start_slot = slot;
            while_muted([slot] { return g_settings.putInt("start", slot) > 0; });
        }
        broadcast_state();
    } else if (strcmp(command, "state") == 0) {
        broadcast_state();
    } else if (strcmp(command, "stat") == 0) {
        Serial.println(status_line());
    } else if (strcmp(command, "peak") == 0) {
        Serial.printf("PEAK %d\n", engine::take_peak());
    } else if (strcmp(command, "usb") == 0) {
        g_port.print_descriptors(Serial);
    } else if (strcmp(command, "dump") == 0) {
        presets::write_json(Serial, g_edit);
        Serial.println();
    } else if (*command != '\0') {
        notify(String("Unbekannter Befehl ") + command);
    }
}

void poll_console() {
    static char line[112];
    static size_t length = 0;
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c != '\n') {
            if (length < sizeof(line) - 1) line[length++] = c;
            continue;
        }
        line[length] = '\0';
        length = 0;
        Serial.printf("> %s\n", line);
        // Nur an der Konsole: was sonst an den Browser geht, hier ausgeben.
        if (strcmp(line, "schema") == 0) {
            web::write_schema(Serial);
            Serial.println();
        } else if (strcmp(line, "state") == 0) {
            Serial.println(state_message());
        } else {
            handle(line);
        }
    }
}

// MARK: - MIDI

void play(const midifilter::Event& event) {
    switch (event.kind) {
        case midifilter::MessageKind::NoteOn:
            if (event.velocity() == 0) {
                engine::note_off(event.note());
            } else {
                engine::note_on(event.note(), event.velocity());
            }
            break;
        case midifilter::MessageKind::NoteOff:
            engine::note_off(event.note());
            break;
        case midifilter::MessageKind::PitchBend:
            engine::pitch_bend(((event.data2 << 7) | event.data1) - 8192);
            break;
        case midifilter::MessageKind::ProgramChange:
            load_slot(event.data1);
            break;
        case midifilter::MessageKind::ControlChange:
            if (event.data1 == 120 || event.data1 == 123) engine::all_notes_off();
            break;
        default:
            break;
    }
}

void trace(const midifilter::Event& event) {
    static const char* const kKinds[] = {"Off", "On", "PolyAT", "CC", "PC", "AT", "Bend"};
    Serial.printf("MIDI  Kanal %-2u %-6s %3u %3u\n", static_cast<unsigned>(event.channel) + 1,
                  kKinds[static_cast<uint8_t>(event.kind)], event.data1, event.data2);
}

void forward(const midifilter::Event& event) {
    if (g_thru) g_port.write(event);
    play(event);
}

void poll_midi() {
    midifilter::Event event;
    while (g_port.read(event)) {
        ++g_midi_in;
        if (g_trace) trace(event);
        if (g_ezag) {
            const midifilter::Result result = g_filter.process(event);
            for (uint8_t i = 0; i < result.count; ++i) forward(result.events[i]);
            if (result.count == 0) continue;
        } else {
            forward(event);
        }
        neopixelWrite(kActivityLedPin, 0, kActivityLedBrightness, 0);
        g_led_off_at_ms = millis() + 30;
    }
    if (g_led_off_at_ms != 0 && static_cast<int32_t>(millis() - g_led_off_at_ms) >= 0) {
        neopixelWrite(kActivityLedPin, 0, 0, 0);
        g_led_off_at_ms = 0;
    }
}

void on_web_command(char* line) {
    // Seltene Befehle mitschreiben, die häufigen (Regler, Tasten) nicht — die kämen
    // beim Ziehen sechzigmal pro Sekunde.
    static const char* const kQuiet[] = {"set ", "on ", "off ", "state"};
    bool quiet = false;
    for (const char* prefix : kQuiet) quiet = quiet || strncmp(line, prefix, strlen(prefix)) == 0;
    if (!quiet) Serial.printf("WEB   %s\n", line);
    handle(line);
}

}  // namespace

void setup() {
    pinMode(kActivityLedPin, OUTPUT);
    neopixelWrite(kActivityLedPin, 0, 0, 0);

    // Mit Puffer, damit eine Statuszeile die MIDI-Verarbeitung nicht aufhält.
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SID-Synth ===");

    g_settings.begin("synth", false);
    g_thru = g_settings.getBool("thru", true);
    g_ezag = g_settings.getBool("ezag", false);
    g_start_slot = g_settings.getInt("start", 0);
    load_knob_settings();

    if (!presets::begin()) Serial.println("FLASH Dateisystem fehlt, nur Werksklänge");
    if (!presets::used(g_start_slot)) g_start_slot = presets::step(-1, 1);
    g_slot = g_start_slot;
    g_edit = presets::get(g_slot);
    engine::begin(g_edit);

    g_port.begin();
    // Falls die SidStation von einem vorherigen Lauf noch eine Note hält.
    const midifilter::Result panic = g_filter.panic();
    for (uint8_t i = 0; i < panic.count && g_thru; ++i) g_port.write(panic.events[i]);

    web::begin(&on_web_command);
    controls::begin();
    Serial.printf("WLAN  \"%s\" / %s  ->  http://%s.local/  oder  http://%s/\n", web::kSsid,
                  web::kPassword, web::kHostname, WiFi.softAPIP().toString().c_str());
    Serial.printf("PRESET %02d %s\n", g_slot + 1, g_edit.name);
}

void loop() {
    poll_midi();
    web::poll();
    poll_controls();
    poll_console();

    static uint32_t next_status_ms = 1000;
    static uint32_t next_console_ms = 5000;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - next_status_ms) >= 0) {
        next_status_ms = now + 1000;
        if (web::clients() > 0) web::broadcast("stat " + status_line());
    }
    if (static_cast<int32_t>(now - next_console_ms) >= 0) {
        next_console_ms = now + 5000;
        Serial.printf("STAT %s browser=%u reglerwerte=%u\n", status_line().c_str(),
                      static_cast<unsigned>(web::clients()),
                      static_cast<unsigned>(g_param_changes));
    }
    delay(1);
}
