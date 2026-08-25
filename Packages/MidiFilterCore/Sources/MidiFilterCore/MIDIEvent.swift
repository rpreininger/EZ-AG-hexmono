import Foundation

/// A parsed MIDI 1.0 channel voice message.
public enum MIDIMessage: Equatable, Hashable, Sendable {
    case noteOn(note: UInt8, velocity: UInt8)
    case noteOff(note: UInt8, velocity: UInt8)
    case polyPressure(note: UInt8, value: UInt8)
    case controlChange(controller: UInt8, value: UInt8)
    case programChange(program: UInt8)
    case channelPressure(value: UInt8)
    case pitchBend(value: UInt16) // 0...16383, centre = 8192
}

/// One channel voice message plus the channel it arrived on and a monotonic timestamp
/// in seconds. Everything downstream of the CoreMIDI layer works on these.
public struct MIDIEvent: Equatable, Hashable, Sendable {
    public var channel: UInt8 // 0...15 (displayed as 1...16)
    public var message: MIDIMessage
    public var time: Double

    public init(channel: UInt8, message: MIDIMessage, time: Double = 0) {
        self.channel = channel
        self.message = message
        self.time = time
    }
}

// MARK: - Raw byte conversion

public extension MIDIEvent {
    /// Parses a three byte channel voice message. `Note On` with velocity 0 is
    /// normalised to `Note Off`, which is what many controllers send.
    init?(status: UInt8, data1: UInt8, data2: UInt8, time: Double) {
        guard status >= 0x80, status < 0xF0 else { return nil }
        let channel = status & 0x0F
        let message: MIDIMessage
        switch status & 0xF0 {
        case 0x80: message = .noteOff(note: data1, velocity: data2)
        case 0x90: message = data2 == 0 ? .noteOff(note: data1, velocity: 0)
                                        : .noteOn(note: data1, velocity: data2)
        case 0xA0: message = .polyPressure(note: data1, value: data2)
        case 0xB0: message = .controlChange(controller: data1, value: data2)
        case 0xC0: message = .programChange(program: data1)
        case 0xD0: message = .channelPressure(value: data1)
        case 0xE0: message = .pitchBend(value: UInt16(data1) | (UInt16(data2) << 7))
        default: return nil
        }
        self.init(channel: channel, message: message, time: time)
    }

    /// Status / data1 / data2 for transmission.
    var bytes: (status: UInt8, data1: UInt8, data2: UInt8) {
        switch message {
        case let .noteOff(note, velocity):          return (0x80 | channel, note, velocity)
        case let .noteOn(note, velocity):           return (0x90 | channel, note, velocity)
        case let .polyPressure(note, value):        return (0xA0 | channel, note, value)
        case let .controlChange(controller, value): return (0xB0 | channel, controller, value)
        case let .programChange(program):           return (0xC0 | channel, program, 0)
        case let .channelPressure(value):           return (0xD0 | channel, value, 0)
        case let .pitchBend(value):                 return (0xE0 | channel, UInt8(value & 0x7F), UInt8((value >> 7) & 0x7F))
        }
    }
}

// MARK: - Convenience

public extension MIDIEvent {
    var isNoteOn: Bool { if case .noteOn = message { return true }; return false }
    var isNoteOff: Bool { if case .noteOff = message { return true }; return false }
    var isNote: Bool { isNoteOn || isNoteOff }

    var note: UInt8? {
        switch message {
        case let .noteOn(note, _), let .noteOff(note, _), let .polyPressure(note, _): return note
        default: return nil
        }
    }

    var velocity: UInt8? {
        switch message {
        case let .noteOn(_, velocity), let .noteOff(_, velocity): return velocity
        default: return nil
        }
    }
}

public enum NoteName {
    private static let names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    /// Scientific pitch notation, i.e. note 60 == C4.
    public static func name(for note: UInt8) -> String {
        let octave = Int(note) / 12 - 1
        return "\(names[Int(note) % 12])\(octave)"
    }
}

public extension MIDIMessage {
    /// Short human readable form for the monitor.
    var displayName: String {
        switch self {
        case let .noteOn(note, velocity):    return "Note On   \(NoteName.name(for: note)) (\(note))  v\(velocity)"
        case let .noteOff(note, velocity):   return "Note Off  \(NoteName.name(for: note)) (\(note))  v\(velocity)"
        case let .polyPressure(note, value): return "Poly AT   \(NoteName.name(for: note))  \(value)"
        case let .controlChange(cc, value):  return "CC \(cc)  \(value)"
        case let .programChange(program):    return "Prog \(program)"
        case let .channelPressure(value):    return "Chan AT  \(value)"
        case let .pitchBend(value):          return "Bend \(Int(value) - 8192)"
        }
    }
}
