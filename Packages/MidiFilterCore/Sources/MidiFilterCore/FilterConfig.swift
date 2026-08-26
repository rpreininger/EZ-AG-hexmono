import Foundation

/// Which note wins when more than one is held down.
public enum NotePriority: String, Codable, CaseIterable, Sendable {
    case last, highest, lowest

    public var displayName: String {
        switch self {
        case .last: return "Zuletzt gespielt"
        case .highest: return "Höchste Note"
        case .lowest: return "Tiefste Note"
        }
    }
}

/// Strategies for telling a fret button press apart from an actual string pluck.
///
/// On the Yamaha EZ-AG both produce a `Note On` with the same note number on the same
/// channel, so the distinction has to come from context. Which of these works depends on
/// what the instrument actually sends — use the monitor to find out.
public enum FretMode: String, Codable, CaseIterable, Sendable {
    /// Pass everything, only merge and make monophonic.
    case off
    /// Suppress `Note On` at or below (or exactly at) a velocity — fret buttons are not
    /// velocity sensitive, so they tend to send one fixed value.
    case velocity
    /// Only let a `Note On` through when the same note was switched off moments before,
    /// i.e. the `Note Off` → `Note On` retrigger pair a pluck produces.
    case retrigger
    /// Delay every `Note On` briefly and drop it if its `Note Off` arrives within the
    /// window — catches fret notes that are short blips. Costs latency.
    case blip

    public var displayName: String {
        switch self {
        case .off: return "Aus (alles durchlassen)"
        case .velocity: return "Velocity"
        case .retrigger: return "Retrigger (Off→On Paar)"
        case .blip: return "Kurze Noten verwerfen"
        }
    }

    public var explanation: String {
        switch self {
        case .off:
            return "Keine Bund-Unterdrückung. Nur Kanal-Merge und Monophonie."
        case .velocity:
            return "Bund-Tasten sind nicht anschlagdynamisch und senden meist einen festen Velocity-Wert. Note On mit diesem Wert wird verworfen."
        case .retrigger:
            return "Ein Anschlag auf eine bereits klingende Saite sendet Note Off → Note On. Nur solche Paare gelten als Anschlag, ein einzelnes Note On gilt als Bund-Taste."
        case .blip:
            return "Jedes Note On wird um die eingestellte Zeit verzögert und verworfen, wenn vorher schon das Note Off kommt. Erzeugt Latenz."
        }
    }
}

/// What to do with channels outside the merge range.
public enum OtherChannelPolicy: String, Codable, CaseIterable, Sendable {
    case drop, passThrough

    public var displayName: String {
        switch self {
        case .drop: return "Verwerfen"
        case .passThrough: return "Unverändert durchlassen"
        }
    }
}

public struct FilterConfig: Codable, Equatable, Sendable {
    // Channel merge — 0 based internally, the UI shows 1...16.
    public var lowestInputChannel: UInt8 = 0
    public var highestInputChannel: UInt8 = 5
    public var outputChannel: UInt8 = 0
    public var otherChannels: OtherChannelPolicy = .drop

    // Monophonic voice handling
    public var monophonic = true
    public var priority: NotePriority = .last
    /// Send the new `Note On` before releasing the old note, so a mono synth in legato
    /// mode glides instead of retriggering its envelope.
    public var legato = false
    /// Play the same note again when it is re-attacked while already sounding.
    public var retriggerSameNote = true
    /// When the sounding note is released, return to a note that is still held down.
    /// Standard behaviour for mono synths, but on a guitar controller it makes older
    /// notes reappear that the player never struck — the fret buttons keep notes held.
    public var fallbackToHeldNote = false

    // Fret suppression
    public var fretMode: FretMode = .off
    /// Fret notes always arrive at exactly 16 — the fret buttons are not velocity
    /// sensitive, so that is a constant, not a measurement that could scatter. How soft
    /// a pluck can get, on the other hand, only depends on how gently the string is
    /// played (23 was the quietest measured on 2026-08-25), so the threshold sits as low
    /// as it can: everything above the fret constant is a pluck.
    public var velocityThreshold: UInt8 = 16
    /// Match the threshold exactly instead of "at or below".
    public var velocityExactMatch = false
    public var retriggerWindowMs: Double = 60
    /// Let a lone `Note On` through when nothing is sounding on that string yet, so the
    /// first pluck of an open string is not swallowed.
    public var allowFirstNoteOnIdleString = true
    public var blipMaxMs: Double = 40

    /// A fret change on a string whose struck note is still ringing retunes that note
    /// instead of being thrown away — sliding the fingering moves the pitch, the way it
    /// does on a real string. The velocity of the original pluck carries over, because
    /// it is still that strike sounding.
    ///
    /// Needs the fret detection to be on (`velocity` or `retrigger`), since that is what
    /// tells a fingering change apart from the next pluck.
    public var fretSlide = false
    /// A slide moves the note that is *already* sounding, so a pitch bend is the honest
    /// way to do it: the envelope is never touched and nothing is retriggered.
    ///
    /// Falls back to a real note change when the interval does not fit into the bend
    /// range — a retriggered note beats a wrong pitch.
    public var slideUsesPitchBend = true
    /// Semitones the synth's pitch bend covers in each direction. Has to match what the
    /// synth is set to, otherwise every slide comes out at the wrong interval. Two is the
    /// near universal default; raise it here *and* on the synth to slide further.
    public var pitchBendRange: UInt8 = 2
    /// How long the release of a sounding note is held back to see whether a fret note
    /// follows. The EZ-AG sends the pair about half a millisecond apart; the window only
    /// has to be wide enough to cover that, and it costs exactly this much extra ring
    /// time on a genuine release.
    public var slideWindowMs: Double = 6

    /// Hammer-ons and pull-offs arrive at full velocity, so the velocity rule cannot see
    /// them. The EZ-AG announces each one with `CC 84` (portamento control) about a
    /// millisecond ahead, which identifies them exactly.
    ///
    /// When suppressed, the struck note simply keeps ringing across the fingering change
    /// — only what the player actually strikes ever sounds.
    public var suppressHammerOns = true
    public var hammerOnWindowMs: Double = 20

    // Output shaping
    public var transpose: Int = 0
    public var passOtherMessages = true

    public init() {}

    public var inputChannelRange: ClosedRange<UInt8> {
        let lo = min(lowestInputChannel, highestInputChannel)
        let hi = max(lowestInputChannel, highestInputChannel)
        return lo...hi
    }
}

public extension FilterConfig {
    /// Decoded field by field against the defaults, so stored settings survive a new
    /// option being added here — otherwise every added field would silently reset
    /// everything the user had configured.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        var config = FilterConfig()

        func decode<T: Decodable>(_ key: CodingKeys, _ current: T) -> T {
            ((try? container.decodeIfPresent(T.self, forKey: key)) ?? nil) ?? current
        }

        config.lowestInputChannel = decode(.lowestInputChannel, config.lowestInputChannel)
        config.highestInputChannel = decode(.highestInputChannel, config.highestInputChannel)
        config.outputChannel = decode(.outputChannel, config.outputChannel)
        config.otherChannels = decode(.otherChannels, config.otherChannels)
        config.monophonic = decode(.monophonic, config.monophonic)
        config.priority = decode(.priority, config.priority)
        config.legato = decode(.legato, config.legato)
        config.retriggerSameNote = decode(.retriggerSameNote, config.retriggerSameNote)
        config.fallbackToHeldNote = decode(.fallbackToHeldNote, config.fallbackToHeldNote)
        config.fretMode = decode(.fretMode, config.fretMode)
        config.velocityThreshold = decode(.velocityThreshold, config.velocityThreshold)
        config.velocityExactMatch = decode(.velocityExactMatch, config.velocityExactMatch)
        config.retriggerWindowMs = decode(.retriggerWindowMs, config.retriggerWindowMs)
        config.allowFirstNoteOnIdleString = decode(.allowFirstNoteOnIdleString, config.allowFirstNoteOnIdleString)
        config.blipMaxMs = decode(.blipMaxMs, config.blipMaxMs)
        config.fretSlide = decode(.fretSlide, config.fretSlide)
        config.slideWindowMs = decode(.slideWindowMs, config.slideWindowMs)
        config.slideUsesPitchBend = decode(.slideUsesPitchBend, config.slideUsesPitchBend)
        config.pitchBendRange = decode(.pitchBendRange, config.pitchBendRange)
        config.suppressHammerOns = decode(.suppressHammerOns, config.suppressHammerOns)
        config.hammerOnWindowMs = decode(.hammerOnWindowMs, config.hammerOnWindowMs)
        config.transpose = decode(.transpose, config.transpose)
        config.passOtherMessages = decode(.passOtherMessages, config.passOtherMessages)

        self = config
    }
}
