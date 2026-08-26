import XCTest
@testable import MidiFilterCore

/// Convenience builders — channels are 0 based here, i.e. `ch: 5` is MIDI channel 6.
private func on(_ ch: UInt8, _ note: UInt8, _ vel: UInt8 = 100, at t: Double = 0) -> MIDIEvent {
    MIDIEvent(channel: ch, message: .noteOn(note: note, velocity: vel), time: t)
}

private func off(_ ch: UInt8, _ note: UInt8, at t: Double = 0) -> MIDIEvent {
    MIDIEvent(channel: ch, message: .noteOff(note: note, velocity: 0), time: t)
}

final class ChannelMergeTests: XCTestCase {

    func testStringChannelsAreMergedOntoTheOutputChannel() {
        let filter = EZAGFilter()
        for channel in UInt8(0)...5 {
            let result = filter.process(on(channel, 60))
            XCTAssertEqual(result.verdict, .passed)
            XCTAssertEqual(result.out.first?.channel, 0)
            _ = filter.process(off(channel, 60))
        }
    }

    func testChannelsOutsideTheRangeAreDropped() {
        let filter = EZAGFilter()
        let result = filter.process(on(9, 60))
        XCTAssertEqual(result.verdict, .filteredChannel)
        XCTAssertTrue(result.out.isEmpty)
    }

    func testChannelsOutsideTheRangeCanPassThroughUnchanged() {
        var config = FilterConfig()
        config.otherChannels = .passThrough
        let filter = EZAGFilter(config: config)
        let result = filter.process(on(9, 60))
        XCTAssertEqual(result.out, [on(9, 60)])
    }

    func testControlChangeIsRemappedToTheOutputChannel() {
        let filter = EZAGFilter()
        let cc = MIDIEvent(channel: 3, message: .controlChange(controller: 64, value: 127))
        let result = filter.process(cc)
        XCTAssertEqual(result.out, [MIDIEvent(channel: 0, message: .controlChange(controller: 64, value: 127))])
    }
}

final class MonophonicTests: XCTestCase {

    func testNewNoteStealsTheVoiceAndReleasesTheOldOne() {
        let filter = EZAGFilter()
        XCTAssertEqual(filter.process(on(0, 60, 90)).out, [on(0, 60, 90)])

        // Second string, still holding the first: old note off, new note on.
        XCTAssertEqual(filter.process(on(1, 64, 80)).out, [off(0, 60), on(0, 64, 80)])
    }

    func testReleasingTheStolenNoteChangesNothing() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        _ = filter.process(on(1, 64))
        // Releasing the note that is not sounding must not disturb the voice.
        XCTAssertEqual(filter.process(off(0, 60)).out, [])
    }

    /// The default: only the note the player struck last sounds. Letting it go ends the
    /// note, even while other strings are still held down.
    func testReleasingTheSoundingNoteEndsTheVoice() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        _ = filter.process(on(1, 64))
        XCTAssertEqual(filter.process(off(1, 64)).out, [off(0, 64)])
        // The still held note stays silent until it is struck again.
        XCTAssertEqual(filter.process(off(0, 60)).out, [])
    }

    func testReleasingTheSoundingNoteFallsBackToTheHeldNoteWhenEnabled() {
        var config = FilterConfig()
        config.fallbackToHeldNote = true
        let filter = EZAGFilter(config: config)
        _ = filter.process(on(0, 60))
        _ = filter.process(on(1, 64))
        XCTAssertEqual(filter.process(off(1, 64)).out, [off(0, 64), on(0, 60, 100)])
    }

    func testLastHeldNoteEndsTheVoice() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        XCTAssertEqual(filter.process(off(0, 60)).out, [off(0, 60)])
    }

    func testHighestNotePriorityKeepsTheTopNote() {
        var config = FilterConfig()
        config.priority = .highest
        config.fallbackToHeldNote = true
        let filter = EZAGFilter(config: config)
        _ = filter.process(on(0, 67))
        // Lower note arrives later but must not steal the voice.
        XCTAssertEqual(filter.process(on(1, 60)).out, [])
    }

    /// Without fallback the priority must not block a new attack — otherwise a held high
    /// note would silence everything played after it.
    func testWithoutFallbackEveryAttackTakesTheVoice() {
        var config = FilterConfig()
        config.priority = .highest
        config.fallbackToHeldNote = false
        let filter = EZAGFilter(config: config)
        _ = filter.process(on(0, 67))
        XCTAssertEqual(filter.process(on(1, 60, 90)).out, [off(0, 67), on(0, 60, 90)])
    }

    func testStruckAgainRetriggersOnlyTheNoteThatWasStruck() {
        var config = FilterConfig()
        config.priority = .highest
        config.fallbackToHeldNote = true
        let filter = EZAGFilter(config: config)
        _ = filter.process(on(0, 67))
        _ = filter.process(on(1, 60))
        // Hitting the sounding note itself again does re-trigger it.
        XCTAssertEqual(filter.process(on(0, 67, 90)).out, [off(0, 67), on(0, 67, 90)])
    }

    func testLegatoSendsTheNewNoteBeforeReleasingTheOldOne() {
        var config = FilterConfig()
        config.legato = true
        let filter = EZAGFilter(config: config)
        _ = filter.process(on(0, 60))
        XCTAssertEqual(filter.process(on(1, 64, 100)).out, [on(0, 64, 100), off(0, 60)])
    }

    func testSameNoteIsRetriggeredWhenStruckAgain() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        _ = filter.process(off(0, 60))
        XCTAssertEqual(filter.process(on(0, 60, 70)).out, [on(0, 60, 70)])
    }

    func testPolyphonicModePassesEveryNote() {
        var config = FilterConfig()
        config.monophonic = false
        let filter = EZAGFilter(config: config)
        XCTAssertEqual(filter.process(on(0, 60)).out, [on(0, 60)])
        XCTAssertEqual(filter.process(on(1, 64)).out, [on(0, 64)])
    }

    func testTransposeAppliesToNoteOnAndNoteOff() {
        var config = FilterConfig()
        config.transpose = -12
        let filter = EZAGFilter(config: config)
        XCTAssertEqual(filter.process(on(0, 60)).out, [on(0, 48)])
        XCTAssertEqual(filter.process(off(0, 60)).out, [off(0, 48)])
    }
}

final class FretSuppressionTests: XCTestCase {

    func testVelocityModeDropsTheFretNoteAndItsNoteOff() {
        var config = FilterConfig()
        config.fretMode = .velocity
        config.velocityThreshold = 64
        config.velocityExactMatch = true
        let filter = EZAGFilter(config: config)

        let fret = filter.process(on(0, 60, 64))
        XCTAssertEqual(fret.verdict, .suppressedFret)
        XCTAssertTrue(fret.out.isEmpty)

        let release = filter.process(off(0, 60))
        XCTAssertEqual(release.verdict, .swallowedNoteOff)
        XCTAssertTrue(release.out.isEmpty)

        // A real pluck at another velocity still gets through.
        XCTAssertEqual(filter.process(on(0, 60, 110)).out, [on(0, 60, 110)])
    }

    /// The shipping threshold sits directly on the fret constant: 16 goes, everything
    /// above it counts as a pluck. Quiet plucks were measured down to 23 on 2026-08-25,
    /// but nothing stops the player from striking softer than that.
    func testQuietPluckSurvivesTheDefaultThreshold() {
        var config = FilterConfig()
        config.fretMode = .velocity
        let filter = EZAGFilter(config: config)

        XCTAssertEqual(filter.process(on(0, 47, 16)).verdict, .suppressedFret)
        XCTAssertEqual(filter.process(off(0, 47, at: 1.0)).verdict, .swallowedNoteOff)

        // One step above the fret constant — the tightest a pluck can be and still count.
        let faintest = filter.process(on(0, 47, 17, at: 1.001))
        XCTAssertEqual(faintest.verdict, .passed)
        XCTAssertEqual(faintest.out, [on(0, 47, 17)])
        XCTAssertEqual(filter.process(off(0, 47, at: 2.0)).verdict, .passed)

        // The quietest pluck actually measured.
        let quiet = filter.process(on(0, 47, 23, at: 2.001))
        XCTAssertEqual(quiet.verdict, .passed)
        XCTAssertEqual(quiet.out, [on(0, 47, 23)])
    }

    /// The sequence the EZ-AG produces: press the fret, then pluck the string.
    func testRetriggerModeDropsTheFretNoteAndKeepsThePluck() {
        var config = FilterConfig()
        config.fretMode = .retrigger
        config.allowFirstNoteOnIdleString = false
        let filter = EZAGFilter(config: config)

        // Fret button pressed: lone Note On.
        XCTAssertEqual(filter.process(on(5, 55, 100, at: 0.0)).verdict, .suppressedFret)
        // Pluck on the now sounding string: Note Off → Note On.
        XCTAssertEqual(filter.process(off(5, 55, at: 1.0)).verdict, .swallowedNoteOff)
        let pluck = filter.process(on(5, 55, 120, at: 1.001))
        XCTAssertEqual(pluck.verdict, .passed)
        XCTAssertEqual(pluck.out, [on(0, 55, 120)])
        // Releasing the fret ends the note.
        XCTAssertEqual(filter.process(off(5, 55, at: 2.0)).out, [off(0, 55)])
    }

    func testRetriggerModeIgnoresNoteOffsThatAreTooOld() {
        var config = FilterConfig()
        config.fretMode = .retrigger
        config.retriggerWindowMs = 60
        config.allowFirstNoteOnIdleString = false
        let filter = EZAGFilter(config: config)

        _ = filter.process(on(5, 55, 100, at: 0.0))
        _ = filter.process(off(5, 55, at: 1.0))
        // Well outside the window: a fresh fret press, not a pluck.
        XCTAssertEqual(filter.process(on(5, 55, 100, at: 5.0)).verdict, .suppressedFret)
    }

    func testRetriggerModeLetsTheFirstPluckOfASilentStringThrough() {
        var config = FilterConfig()
        config.fretMode = .retrigger
        config.allowFirstNoteOnIdleString = true
        let filter = EZAGFilter(config: config)

        XCTAssertEqual(filter.process(on(5, 40, 100, at: 0.0)).verdict, .passed)
        // Second lone Note On while the string already sounds: fret press.
        XCTAssertEqual(filter.process(on(5, 47, 100, at: 0.5)).verdict, .suppressedFret)
    }

    /// Recorded from a real Yamaha EZ-AG on 2026-08-20: the low E string is ringing, the
    /// player frets the 7th fret and then plucks.
    ///
    /// Note how a fret press and a pluck are structurally indistinguishable — both are a
    /// `Note Off` followed by a `Note On` roughly half a millisecond later. Only the
    /// velocity separates them, which is why this is the strategy that ships.
    func testMeasuredFretAndPluckSequence() {
        var config = FilterConfig()
        config.fretMode = .velocity
        config.velocityThreshold = 32
        let filter = EZAGFilter(config: config)

        // Open low E, plucked earlier and still ringing.
        XCTAssertEqual(filter.process(on(5, 40, 48, at: 0)).out, [on(0, 40, 48)])

        // Fretting kills the open note and fires the fret note at velocity 16.
        XCTAssertEqual(filter.process(off(5, 40, at: 10.0)).out, [off(0, 40)])
        XCTAssertEqual(filter.process(on(5, 47, 16, at: 10.0004)).verdict, .suppressedFret)

        // The pluck: the fret note's Note Off is swallowed with it, the pluck gets through.
        XCTAssertEqual(filter.process(off(5, 47, at: 11.0)).verdict, .swallowedNoteOff)
        XCTAssertEqual(filter.process(on(5, 47, 92, at: 11.0005)).out, [on(0, 47, 92)])

        // Releasing the fret ends the note — the EZ-AG does not fall back to the open string.
        XCTAssertEqual(filter.process(off(5, 47, at: 11.6)).out, [off(0, 47)])
    }

    /// Also recorded on 2026-08-20: pluck the 8th fret, then hammer on and pull off
    /// between the 8th and 9th fret without striking the string again.
    ///
    /// Every transition arrives at velocity 96 — same as a real pluck — but is announced
    /// by `CC 84` about a millisecond earlier. That marker is the only thing that
    /// separates them.
    func testMeasuredHammerOnSequence() {
        var config = FilterConfig()
        config.fretMode = .velocity
        config.velocityThreshold = 32
        config.suppressHammerOns = true
        let filter = EZAGFilter(config: config)

        func portamento(from note: UInt8, at time: Double) -> Verdict {
            filter.process(MIDIEvent(channel: 0,
                                     message: .controlChange(controller: 84, value: note),
                                     time: time)).verdict
        }

        // The struck note.
        XCTAssertEqual(filter.process(on(0, 72, 96, at: 1.327)).out, [on(0, 72, 96)])

        // Hammer on to the 9th fret: announced, suppressed, and the release of the note
        // it replaces is swallowed with it.
        XCTAssertEqual(portamento(from: 72, at: 1.935), .suppressedHammerOn)
        XCTAssertEqual(filter.process(on(0, 73, 96, at: 1.936)).verdict, .suppressedHammerOn)
        XCTAssertEqual(filter.process(off(0, 72, at: 1.9366)).verdict, .swallowedNoteOff)

        // Pull off back to the 8th fret — the note that is really sounding.
        XCTAssertEqual(portamento(from: 73, at: 2.143), .suppressedHammerOn)
        XCTAssertEqual(filter.process(on(0, 72, 96, at: 2.144)).verdict, .suppressedHammerOn)
        XCTAssertEqual(filter.process(off(0, 73, at: 2.145)).verdict, .swallowedNoteOff)

        // Letting go at the end must still stop the note — no hanging voice.
        XCTAssertEqual(filter.process(off(0, 72, at: 3.291)).out, [off(0, 72)])
    }

    func testHammerOnsPassThroughWhenNotSuppressed() {
        var config = FilterConfig()
        config.fretMode = .velocity
        config.suppressHammerOns = false
        let filter = EZAGFilter(config: config)

        _ = filter.process(on(0, 72, 96, at: 1.327))
        let cc = MIDIEvent(channel: 0, message: .controlChange(controller: 84, value: 72), time: 1.935)
        // Forwarded, so a synth with portamento control glides the way the guitar intends.
        XCTAssertEqual(filter.process(cc).out, [cc])
        XCTAssertEqual(filter.process(on(0, 73, 96, at: 1.936)).out, [off(0, 72), on(0, 73, 96)])
    }

    func testBlipModeDropsShortNotePairs() {
        var config = FilterConfig()
        config.fretMode = .blip
        config.blipMaxMs = 40
        let filter = EZAGFilter(config: config)

        XCTAssertEqual(filter.process(on(0, 60, 100, at: 0.0)).verdict, .heldForBlip)
        XCTAssertTrue(filter.hasPendingNotes)
        XCTAssertEqual(filter.process(off(0, 60, at: 0.02)).verdict, .droppedBlipPair)
        XCTAssertFalse(filter.hasPendingNotes)
        XCTAssertTrue(filter.tick(now: 1.0).isEmpty)
    }

    func testBlipModeEmitsNotesThatOutlastTheWindow() {
        var config = FilterConfig()
        config.fretMode = .blip
        config.blipMaxMs = 40
        let filter = EZAGFilter(config: config)

        XCTAssertEqual(filter.process(on(0, 60, 100, at: 0.0)).verdict, .heldForBlip)
        XCTAssertTrue(filter.tick(now: 0.03).isEmpty)
        XCTAssertEqual(filter.tick(now: 0.05), [on(0, 60, 100)])
        XCTAssertEqual(filter.process(off(0, 60, at: 0.5)).out, [off(0, 60)])
    }
}

final class HousekeepingTests: XCTestCase {

    func testPanicReleasesTheSoundingNote() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        // The bend goes back to the centre unconditionally: panic is the button for
        // getting out of trouble, and the synth may be left bent by an earlier run.
        XCTAssertEqual(filter.panic(), [
            off(0, 60),
            MIDIEvent(channel: 0, message: .pitchBend(value: 8192)),
            MIDIEvent(channel: 0, message: .controlChange(controller: 123, value: 0)),
        ])
    }

    func testChangingTheOutputChannelReleasesTheOldNote() {
        let filter = EZAGFilter()
        _ = filter.process(on(0, 60))
        var config = FilterConfig()
        config.outputChannel = 3
        XCTAssertEqual(filter.setConfig(config), [off(0, 60)])
        XCTAssertEqual(filter.process(on(0, 64)).out, [on(3, 64)])
    }
}

final class MIDIEventTests: XCTestCase {

    func testNoteOnWithVelocityZeroIsANoteOff() {
        let event = MIDIEvent(status: 0x95, data1: 60, data2: 0, time: 0)
        XCTAssertEqual(event, off(5, 60))
    }

    func testRoundTripThroughBytes() {
        let events: [MIDIEvent] = [
            on(5, 60, 100),
            off(2, 40),
            MIDIEvent(channel: 0, message: .controlChange(controller: 64, value: 127)),
            MIDIEvent(channel: 7, message: .pitchBend(value: 8192)),
            MIDIEvent(channel: 1, message: .channelPressure(value: 55)),
            MIDIEvent(channel: 1, message: .programChange(program: 12)),
        ]
        for event in events {
            let (status, d1, d2) = event.bytes
            XCTAssertEqual(MIDIEvent(status: status, data1: d1, data2: d2, time: 0), event)
        }
    }

    func testNoteNames() {
        XCTAssertEqual(NoteName.name(for: 60), "C4")
        XCTAssertEqual(NoteName.name(for: 40), "E2")
    }
}

/// Sliding the fingering on a string that is still ringing has to move the pitch of the
/// note that is already sounding — which makes it a pitch bend, not a new note.
final class FretSlideTests: XCTestCase {

    private func slideConfig(bendRange: UInt8 = 2) -> FilterConfig {
        var config = FilterConfig()
        config.fretMode = .velocity
        config.fretSlide = true
        config.slideWindowMs = 6
        config.pitchBendRange = bendRange
        return config
    }

    private func bend(_ value: UInt16) -> MIDIEvent {
        MIDIEvent(channel: 0, message: .pitchBend(value: value))
    }

    func testFretSlideBendsTheRingingNote() {
        let filter = EZAGFilter(config: slideConfig())

        // The open low E, struck.
        XCTAssertEqual(filter.process(on(5, 40, 48)).out, [on(0, 40, 48)])

        // A fingering change starts with the release of the ringing note …
        let release = filter.process(off(5, 40, at: 1.0))
        XCTAssertEqual(release.verdict, .heldForSlide)
        XCTAssertTrue(release.out.isEmpty)

        // … and the fret note half a millisecond later. One semitone up, and with a bend
        // range of two that is half of the way up: nothing is retriggered, the note that
        // is already sounding simply moves.
        let slide = filter.process(on(5, 41, 16, at: 1.0005))
        XCTAssertEqual(slide.verdict, .slid)
        XCTAssertEqual(slide.out, [bend(12288)])

        // Sliding back down lands exactly on the centre again.
        XCTAssertEqual(filter.process(off(5, 41, at: 2.0)).verdict, .heldForSlide)
        XCTAssertEqual(filter.process(on(5, 40, 16, at: 2.0005)).out, [bend(8192)])
    }

    func testTheReleaseEndsTheNoteThatIsActuallySounding() {
        let filter = EZAGFilter(config: slideConfig())
        _ = filter.process(on(5, 40, 48))
        _ = filter.process(off(5, 40, at: 1.0))
        _ = filter.process(on(5, 42, 16, at: 1.0005))

        // Letting go of the *new* fingering ends the note — and what has to be switched
        // off is the note that went out on the wire, not the fingering.
        XCTAssertEqual(filter.process(off(5, 42, at: 2.0)).verdict, .heldForSlide)
        XCTAssertEqual(filter.tick(now: 2.01), [off(0, 40)])
        XCTAssertFalse(filter.hasPendingNotes)
    }

    func testSlideBeyondTheBendRangeFallsBackToANoteChange() {
        let filter = EZAGFilter(config: slideConfig(bendRange: 2))
        _ = filter.process(on(5, 40, 48))
        _ = filter.process(off(5, 40, at: 1.0))

        // Seven semitones is further than the synth can bend. Better a retriggered note
        // than a wrong pitch — new note first, so a mono synth still glides.
        let slide = filter.process(on(5, 47, 16, at: 1.0005))
        XCTAssertEqual(slide.verdict, .slid)
        XCTAssertEqual(slide.out, [on(0, 47, 48), off(0, 40)])
    }

    func testAWiderBendRangeCarriesTheWholeSlide() {
        let filter = EZAGFilter(config: slideConfig(bendRange: 12))
        _ = filter.process(on(5, 40, 48))
        _ = filter.process(off(5, 40, at: 1.0))
        // Same seven semitones, but now they fit: 7 of 12 above the centre.
        XCTAssertEqual(filter.process(on(5, 47, 16, at: 1.0005)).out, [bend(12970)])
    }

    func testTheBendIsCentredBeforeTheNextStrike() {
        let filter = EZAGFilter(config: slideConfig())
        _ = filter.process(on(5, 40, 48))
        _ = filter.process(off(5, 40, at: 1.0))
        XCTAssertEqual(filter.process(on(5, 41, 16, at: 1.0005)).out, [bend(12288)])

        // Another string is struck: the new note must not inherit the old one's detune.
        XCTAssertEqual(filter.process(on(4, 50, 90, at: 2.0)).out,
                       [off(0, 40), bend(8192), on(0, 50, 90)])
    }

    func testHeldReleaseIsHandedOnWhenAPluckFollowsInstead() {
        let filter = EZAGFilter(config: slideConfig())
        _ = filter.process(on(5, 47, 92))

        XCTAssertEqual(filter.process(off(5, 47, at: 1.0)).verdict, .heldForSlide)
        // Full velocity: the string was struck again, not fingered. The held release goes
        // out first, then the new strike — exactly what it looked like before slide mode.
        let pluck = filter.process(on(5, 47, 96, at: 1.0005))
        XCTAssertEqual(pluck.verdict, .passed)
        XCTAssertEqual(pluck.out, [off(0, 47), on(0, 47, 96)])
    }

    func testNothingFollowsTheHeldRelease() {
        let filter = EZAGFilter(config: slideConfig())
        _ = filter.process(on(5, 40, 48))
        XCTAssertEqual(filter.process(off(5, 40, at: 1.0)).verdict, .heldForSlide)

        // Inside the window nothing happens, after it the release is handed on late.
        XCTAssertEqual(filter.tick(now: 1.003), [])
        XCTAssertEqual(filter.tick(now: 1.01), [off(0, 40)])
        XCTAssertEqual(filter.tick(now: 1.02), [])
    }

    func testAFretPressOnASilentStringIsStillJustAFretPress() {
        let filter = EZAGFilter(config: slideConfig())
        // Nothing ringing anywhere: no release to hold, so the fret note is dropped as
        // before and its own release goes with it.
        XCTAssertEqual(filter.process(on(5, 47, 16)).verdict, .suppressedFret)
        XCTAssertEqual(filter.process(off(5, 47, at: 1.0)).verdict, .swallowedNoteOff)
    }

    func testSlideModeOffKeepsTheOldBehaviour() {
        var config = slideConfig()
        config.fretSlide = false
        let filter = EZAGFilter(config: config)

        XCTAssertEqual(filter.process(on(5, 40, 48)).out, [on(0, 40, 48)])
        // The release goes straight out, the fret note is dropped: the sound stops.
        XCTAssertEqual(filter.process(off(5, 40, at: 1.0)).out, [off(0, 40)])
        XCTAssertEqual(filter.process(on(5, 47, 16, at: 1.0005)).verdict, .suppressedFret)
    }
}
