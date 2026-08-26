// MidiFilter als eigenständige Hardware auf einem ESP32-S3.
//
//   Gitarre → MIDI IN → ESP32-S3 → MIDI OUT → SidStation
//
// Die Ein- und Ausgabe steckt hinter `MidiPort`. Zurzeit gibt es eine UART-Umsetzung für
// DIN-Buchsen; ein USB-Host für ein Klasse-konformes Interface wie den CME C2MIDI Pro
// liesse sich als zweite Umsetzung dahinterschieben, ohne die Logik anzufassen.

#include <Arduino.h>

#include "USBConnection.h"
#include "midi_event.h"
#include "midi_filter.h"

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

constexpr uint32_t kMidiBaud = 31250;
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

// MARK: - Ein- und Ausgabe

/// Trennlinie zur Hardware. Alles darüber ist geprüfte, portable Logik.
class MidiPort {
public:
    virtual ~MidiPort() = default;
    virtual void begin() = 0;
    /// Liefert true und schreibt nach `out`, sobald eine vollständige Nachricht vorliegt.
    virtual bool read(midifilter::Event& out) = 0;
    virtual void write(const midifilter::Event& event) = 0;
};

/// DIN-MIDI über eine Hardware-UART.
class UartMidiPort : public MidiPort {
public:
    UartMidiPort(HardwareSerial& serial, int rx_pin, int tx_pin)
        : serial_(serial), rx_pin_(rx_pin), tx_pin_(tx_pin) {}

    void begin() override {
        serial_.begin(kMidiBaud, SERIAL_8N1, rx_pin_, tx_pin_);
    }

    bool read(midifilter::Event& out) override {
        while (serial_.available() > 0) {
            const uint8_t byte = static_cast<uint8_t>(serial_.read());
            if (parser_.feed(byte, micros(), out)) {
                return true;
            }
        }
        return false;
    }

    void write(const midifilter::Event& event) override {
        uint8_t bytes[3];
        const uint8_t length = midifilter::to_bytes(event, bytes);
        serial_.write(bytes, length);
    }

private:
    HardwareSerial& serial_;
    int rx_pin_;
    int tx_pin_;
    midifilter::Parser parser_;
};

/// USB-Host-MIDI über ein klassenkonformes Interface am nativen USB-Port.
///
/// Der S3 ist hier der Host und versorgt das Kabel mit den 5 V, die es braucht — beim
/// C2MIDI Pro sind das 116 mW. Die USB-Ereignisse laufen in einer eigenen Aufgabe auf
/// Kern 0, `read()` holt nur ab, was dort schon eingesammelt wurde.
class UsbHostMidiPort : public MidiPort {
public:
    void begin() override {
        usb_.setMidiCallback(&UsbHostMidiPort::on_midi, this);
        usb_.setConnectionCallbacks(&on_connect, &on_disconnect, this);
        const bool ok = usb_.begin();
        if (kTrace) {
            Serial.printf("USB: begin %s (%s)\n", ok ? "ok" : "FEHLGESCHLAGEN",
                          usb_.getLastError().c_str());
        }
    }

    /// Nur für die Fehlersuche: hat der Host das Kabel gefunden und beansprucht?
    bool is_connected() const { return usb_.isConnected(); }

    bool read(midifilter::Event& out) override {
        usb_.task();  // leert den Ringpuffer der USB-Aufgabe, ruft dabei on_midi auf
        if (head_ == tail_) return false;
        out = queue_[tail_];
        tail_ = static_cast<uint8_t>((tail_ + 1) % kQueueSize);
        return true;
    }

    void write(const midifilter::Event& event) override {
        uint8_t bytes[3];
        const uint8_t length = midifilter::to_bytes(event, bytes);
        usb_.sendMidiMessage(bytes, length);
    }

private:
    static constexpr uint8_t kQueueSize = 32;

    /// Wie viele MIDI-Bytes hinter dem Kopfbyte eines USB-MIDI-Pakets stehen.
    ///
    /// Ein USB-MIDI-Ereignis ist immer 4 Byte lang, egal wie lang die Nachricht darin
    /// ist. Die überzähligen Bytes sind Füllung und dürfen nicht in den Parser — sonst
    /// gälten sie ihm als Anfang der nächsten Nachricht.
    static uint8_t midi_length_for_cin(uint8_t cin) {
        switch (cin) {
            case 0x5: case 0xF:
                return 1;
            case 0x2: case 0x6: case 0xC: case 0xD:
                return 2;
            case 0x3: case 0x4: case 0x7:
            case 0x8: case 0x9: case 0xA: case 0xB: case 0xE:
                return 3;
            default:
                return 0;
        }
    }

    static void on_connect(void* context) {
        if (!kTrace) return;
        auto* self = static_cast<UsbHostMidiPort*>(context);
        Serial.printf("USB: Gerät gefunden, Endpunkte %s\n",
                      self->usb_.isConnected() ? "beansprucht" : "NICHT beansprucht");
    }

    static void on_disconnect(void*) {
        if (kTrace) Serial.println("USB: Gerät weg");
    }

    /// `data` ist das rohe 4-Byte-Paket: Kopfbyte, dann bis zu drei MIDI-Bytes.
    static void on_midi(void* context, const uint8_t* data, size_t length) {
        if (length < 2) return;
        auto* self = static_cast<UsbHostMidiPort*>(context);

        if (kTrace) {
            // Roh, vor dem Parser — sonst lässt sich „nichts kam an" nicht von
            // „kam an, liess sich aber nicht zerlegen" unterscheiden.
            Serial.printf("USB rohes Paket: %02X %02X %02X %02X\n", data[0],
                          length > 1 ? data[1] : 0, length > 2 ? data[2] : 0,
                          length > 3 ? data[3] : 0);
        }

        const uint8_t cin = data[0] & 0x0F;
        uint8_t midi_length = midi_length_for_cin(cin);
        if (midi_length == 0) return;
        if (midi_length > length - 1) midi_length = static_cast<uint8_t>(length - 1);

        const uint32_t now = micros();
        midifilter::Event event;
        for (uint8_t i = 0; i < midi_length; ++i) {
            if (self->parser_.feed(data[i + 1], now, event)) {
                self->push(event);
            }
        }
    }

    void push(const midifilter::Event& event) {
        const uint8_t next = static_cast<uint8_t>((head_ + 1) % kQueueSize);
        if (next == tail_) return;  // voll — lieber die älteste Nachricht behalten
        queue_[head_] = event;
        head_ = next;
    }

    USBConnection usb_;
    midifilter::Parser parser_;
    midifilter::Event queue_[kQueueSize];
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
};

// MARK: - Einstellungen

/// Die am 2026-08-20 an der EZ-AG gemessenen Werte. Siehe Firmware/README.md.
midifilter::Config ezag_config() {
    midifilter::Config config;
    config.lowest_input_channel = 0;   // Kanal 1: hohe E-Saite
    config.highest_input_channel = 5;  // Kanal 6: tiefe E-Saite
    config.output_channel = 0;

    config.monophonic = true;
    config.priority = midifilter::NotePriority::Last;
    config.legato = true;
    config.retrigger_same_note = true;
    config.fallback_to_held_note = false;

    // Bund-Noten kommen immer mit exakt Velocity 16 — die Tasten sind nicht
    // anschlagdynamisch, das ist eine Konstante und keine streuende Messung. Wie leise
    // ein Anschlag wird, hängt dagegen nur am Spiel: erst hiess es 48 bis 96, am
    // 2026-08-25 an der Box kamen Anschläge bis auf 23 herunter. Die Schwelle liegt
    // deshalb so tief wie möglich — alles über der Bund-Konstanten ist ein Anschlag.
    config.fret_mode = midifilter::FretMode::Velocity;
    config.velocity_threshold = 16;

    // Hammer-On und Pull-Off werden mit CC 84 angekündigt.
    config.suppress_hammer_ons = true;
    return config;
}

// MARK: - Zustand

UartMidiPort g_uart_port(Serial1, kMidiRxPin, kMidiTxPin);
UsbHostMidiPort g_usb_port;
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
