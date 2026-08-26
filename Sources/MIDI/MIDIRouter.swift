import CoreMIDI
import Foundation
import MidiFilterCore

/// Sits between the real time MIDI thread and the rest of the app: owns the filter,
/// serialises access to it and records everything for the monitor.
final class MIDIRouter: @unchecked Sendable {
    let log = EventLog()

    private let lock = NSLock()
    private let filter = EZAGFilter()
    private let sender: MIDISender

    init(sender: MIDISender) {
        self.sender = sender
    }

    /// Entry point from the CoreMIDI read block.
    func receive(_ listPtr: UnsafePointer<MIDIEventList>) {
        for event in MIDISender.parse(listPtr) {
            handle(event)
        }
    }

    func handle(_ event: MIDIEvent) {
        lock.lock()
        let result = filter.process(event)
        lock.unlock()
        log.append(.input, event, verdict: result.verdict)
        emit(result.out)
    }

    /// Drives what waits on the clock: the delayed note-ons of blip mode, and the held
    /// releases of slide mode.
    func tick() {
        lock.lock()
        let out = filter.tick(now: Timebase.now())
        lock.unlock()
        emit(out)
    }

    func setConfig(_ config: FilterConfig) {
        lock.lock()
        let flush = filter.setConfig(config)
        lock.unlock()
        emit(flush)
    }

    func panic() {
        lock.lock()
        let out = filter.panic()
        lock.unlock()
        emit(out)
    }

    private func emit(_ events: [MIDIEvent]) {
        guard !events.isEmpty else { return }
        let now = Timebase.now()
        let stamped = events.map { event -> MIDIEvent in
            var copy = event
            copy.time = now
            return copy
        }
        for event in stamped {
            log.append(.output, event)
        }
        sender.send(stamped)
    }
}
