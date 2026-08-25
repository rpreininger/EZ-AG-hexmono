#include "midi_event.h"

namespace midifilter {

namespace {

uint8_t status_nibble(MessageKind kind) {
    switch (kind) {
        case MessageKind::NoteOff: return 0x80;
        case MessageKind::NoteOn: return 0x90;
        case MessageKind::PolyPressure: return 0xA0;
        case MessageKind::ControlChange: return 0xB0;
        case MessageKind::ProgramChange: return 0xC0;
        case MessageKind::ChannelPressure: return 0xD0;
        case MessageKind::PitchBend: return 0xE0;
    }
    return 0x80;
}

bool is_two_data_bytes(uint8_t status) {
    const uint8_t kind = status & 0xF0;
    return kind != 0xC0 && kind != 0xD0;
}

}  // namespace

uint8_t to_bytes(const Event& event, uint8_t out[3]) {
    out[0] = static_cast<uint8_t>(status_nibble(event.kind) | (event.channel & 0x0F));
    out[1] = event.data1;
    if (event.kind == MessageKind::ProgramChange || event.kind == MessageKind::ChannelPressure) {
        return 2;
    }
    out[2] = event.data2;
    return 3;
}

void Parser::reset() {
    status_ = 0;
    data_count_ = 0;
    system_data_remaining_ = 0;
    in_sysex_ = false;
}

bool Parser::feed(uint8_t byte, uint32_t time_us, Event& out) {
    // Echtzeit-Bytes dürfen mitten in einer Nachricht stehen und lassen den Running
    // Status unangetastet. Uhr und Active Sensing interessieren uns nicht.
    if (byte >= 0xF8) {
        return false;
    }

    if (byte >= 0x80) {
        if (byte == 0xF0) {
            in_sysex_ = true;
            status_ = 0;  // System-Nachrichten löschen den Running Status
            data_count_ = 0;
            return false;
        }
        if (byte == 0xF7) {
            in_sysex_ = false;
            return false;
        }
        if (byte >= 0xF0) {
            // System Common: Running Status verfällt, etwaige Datenbytes überspringen.
            in_sysex_ = false;
            status_ = 0;
            data_count_ = 0;
            switch (byte) {
                case 0xF1: system_data_remaining_ = 1; break;  // MTC Quarter Frame
                case 0xF2: system_data_remaining_ = 2; break;  // Song Position
                case 0xF3: system_data_remaining_ = 1; break;  // Song Select
                default: system_data_remaining_ = 0; break;
            }
            return false;
        }

        status_ = byte;
        data_count_ = 0;
        return false;
    }

    // Datenbyte
    if (in_sysex_) {
        return false;
    }
    if (system_data_remaining_ > 0) {
        --system_data_remaining_;
        return false;
    }
    if (status_ == 0) {
        return false;  // Datenbyte ohne Status — verwerfen
    }

    data_[data_count_++] = byte;
    const uint8_t required = is_two_data_bytes(status_) ? 2 : 1;
    if (data_count_ < required) {
        return false;
    }
    data_count_ = 0;  // Status bleibt stehen: das ist Running Status

    out.channel = static_cast<uint8_t>(status_ & 0x0F);
    out.time_us = time_us;
    out.data1 = data_[0];
    out.data2 = required == 2 ? data_[1] : 0;

    switch (status_ & 0xF0) {
        case 0x80:
            out.kind = MessageKind::NoteOff;
            break;
        case 0x90:
            // Note On mit Velocity 0 ist ein Note Off — die EZ-AG schickt zwar echte
            // Note Offs, aber viele Instrumente sparen sich das.
            out.kind = out.data2 == 0 ? MessageKind::NoteOff : MessageKind::NoteOn;
            break;
        case 0xA0: out.kind = MessageKind::PolyPressure; break;
        case 0xB0: out.kind = MessageKind::ControlChange; break;
        case 0xC0: out.kind = MessageKind::ProgramChange; break;
        case 0xD0: out.kind = MessageKind::ChannelPressure; break;
        case 0xE0: out.kind = MessageKind::PitchBend; break;
        default: return false;
    }
    return true;
}

}  // namespace midifilter
