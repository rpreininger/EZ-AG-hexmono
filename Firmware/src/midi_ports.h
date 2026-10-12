// Ein- und Ausgabe der MIDI-Daten, gemeinsam für die Filter-Box und den Synth.
//
// Alles über `MidiPort` ist geprüfte, portable Logik; hier unten steckt die Hardware.

#pragma once

#include <Arduino.h>

#include "USBConnection.h"
#include "midi_event.h"

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
    static constexpr uint32_t kBaud = 31250;

    UartMidiPort(HardwareSerial& serial, int rx_pin, int tx_pin)
        : serial_(serial), rx_pin_(rx_pin), tx_pin_(tx_pin) {}

    void begin() override {
        serial_.begin(kBaud, SERIAL_8N1, rx_pin_, tx_pin_);
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
    /// `trace` legt Anschluss und rohe Pakete auf die serielle Konsole — nur für die
    /// Fehlersuche, das Schreiben dauert länger als die MIDI-Nachricht selbst.
    explicit UsbHostMidiPort(bool trace = false) : trace_(trace) {}

    void begin() override {
        usb_.setMidiCallback(&UsbHostMidiPort::on_midi, this);
        usb_.setConnectionCallbacks(&on_connect, &on_disconnect, this);
        const bool ok = usb_.begin();
        if (trace_) {
            Serial.printf("USB: begin %s (%s)\n", ok ? "ok" : "FEHLGESCHLAGEN",
                          usb_.getLastError().c_str());
        }
    }

    /// Hat der Host das Kabel gefunden und beansprucht?
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
        auto* self = static_cast<UsbHostMidiPort*>(context);
        if (!self->trace_) return;
        Serial.printf("USB: Gerät gefunden, Endpunkte %s\n",
                      self->usb_.isConnected() ? "beansprucht" : "NICHT beansprucht");
    }

    static void on_disconnect(void* context) {
        auto* self = static_cast<UsbHostMidiPort*>(context);
        if (self->trace_) Serial.println("USB: Gerät weg");
    }

    /// `data` ist das rohe 4-Byte-Paket: Kopfbyte, dann bis zu drei MIDI-Bytes.
    static void on_midi(void* context, const uint8_t* data, size_t length) {
        if (length < 2) return;
        auto* self = static_cast<UsbHostMidiPort*>(context);

        if (self->trace_) {
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

    bool trace_;
    USBConnection usb_;
    midifilter::Parser parser_;
    midifilter::Event queue_[kQueueSize];
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
};
