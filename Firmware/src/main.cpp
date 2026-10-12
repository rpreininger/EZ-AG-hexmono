// MidiFilter als eigenständige Hardware auf einem ESP32-S3.
//
//   Gitarre → MIDI IN → ESP32-S3 → MIDI OUT → SidStation
//
// Die Ein- und Ausgabe steckt hinter `MidiPort` (midi_ports.h): eine UART-Umsetzung für
// DIN-Buchsen und ein USB-Host für ein klassenkonformes Interface wie den CME C2MIDI Pro.
//
// Die Box mit eingebautem SID-Synth ist eine eigene Firmware, siehe synth/synth_main.cpp.

#include <Arduino.h>

#include "ezag_config.h"
#include "midi_event.h"
#include "midi_filter.h"
#include "midi_ports.h"

namespace {

// MARK: - Verdrahtung

// Beliebige freie GPIOs. RX kommt vom Optokoppler, TX geht über zwei Widerstände auf die
// DIN-Buchse. Die Pins 19/20 des S3 liegen am USB und sind tabu.
constexpr int kMidiRxPin = 17;
constexpr int kMidiTxPin = 18;
constexpr int kActivityLedPin = LED_BUILTIN;

/// Helligkeit der Aktivitäts-LED, 0 bis 255.
///
/// Die eingebaute LED des Boards ist keine einfache Leuchtdiode, sondern eine RGB-Zelle
/// (`RGB_BUILTIN`). `digitalWrite(..., HIGH)` schaltet sie weiss auf `RGB_BRIGHTNESS`,
/// also 64 — das blitzt bei jeder Note grell auf. Über `neopixelWrite` lässt sich die
/// Helligkeit frei wählen; ein gedämpftes Grün reicht völlig, um Betrieb anzuzeigen.
constexpr uint8_t kActivityLedBrightness = 6;

constexpr uint32_t kConsoleBaud = 115200;

/// Welcher Weg die MIDI-Daten nehmen.
///
/// true — der CME C2MIDI Pro am nativen USB-Port (Port 2). Er bringt beide DIN-Buchsen
/// mit: der weisse Stecker geht an MIDI OUT der Gitarre, der schwarze an MIDI IN der
/// SidStation. Das ist die Standalone-Box, kein Optokoppler nötig.
///
/// false — DIN-MIDI über die UART an GPIO 17/18. Braucht die Optokoppler-Platine aus
/// dem README.
constexpr bool kUseUsbHost = true;

/// Legt Nachrichten auf die serielle Konsole. Im Betrieb aus — das Schreiben dauert
/// länger als die MIDI-Nachricht selbst und würde die Latenz verderben.
constexpr bool kTrace = false;

// MARK: - Zustand

UartMidiPort g_uart_port(Serial1, kMidiRxPin, kMidiTxPin);
UsbHostMidiPort g_usb_port(kTrace);
MidiPort& g_port = kUseUsbHost ? static_cast<MidiPort&>(g_usb_port)
                               : static_cast<MidiPort&>(g_uart_port);
midifilter::Filter g_filter(ezag_config());
uint32_t g_led_off_at_ms = 0;

void trace(const char* direction, const midifilter::Event& event) {
    if (!kTrace) return;
    Serial.printf("%s ch%-2u kind=%u %3u %3u\n", direction,
                  static_cast<unsigned>(event.channel) + 1,
                  static_cast<unsigned>(event.kind),
                  static_cast<unsigned>(event.data1),
                  static_cast<unsigned>(event.data2));
}

void send(const midifilter::Result& result) {
    for (uint8_t i = 0; i < result.count; ++i) {
        g_port.write(result.events[i]);
        trace("OUT", result.events[i]);
    }
    if (result.count > 0) {
        neopixelWrite(kActivityLedPin, 0, kActivityLedBrightness, 0);
        g_led_off_at_ms = millis() + 30;
    }
}

}  // namespace

void setup() {
    pinMode(kActivityLedPin, OUTPUT);
    neopixelWrite(kActivityLedPin, 0, 0, 0);

    if (kTrace) {
        Serial.begin(kConsoleBaud);
    }
    g_port.begin();

    // Falls der Synth von einem vorherigen Lauf noch eine Note hält.
    send(g_filter.panic());
}

void loop() {
    if (kTrace && kUseUsbHost) {
        static uint32_t next_status_ms = 0;
        if (static_cast<int32_t>(millis() - next_status_ms) >= 0) {
            next_status_ms = millis() + 2000;
            Serial.printf("USB: %s\n", g_usb_port.is_connected() ? "verbunden" : "wartet");
        }
    }

    midifilter::Event event;
    while (g_port.read(event)) {
        trace("IN ", event);
        send(g_filter.process(event));
    }

    if (g_led_off_at_ms != 0 && static_cast<int32_t>(millis() - g_led_off_at_ms) >= 0) {
        neopixelWrite(kActivityLedPin, 0, 0, 0);
        g_led_off_at_ms = 0;
    }
}
