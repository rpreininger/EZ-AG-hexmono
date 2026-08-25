import CoreMIDI
import Foundation
import MidiFilterCore

/// Converts mach host time — what CoreMIDI stamps its packets with — into seconds.
enum Timebase {
    private static let scale: Double = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return Double(info.numer) / Double(info.denom) / 1_000_000_000
    }()

    static func seconds(_ hostTime: MIDITimeStamp) -> Double { Double(hostTime) * scale }
    static func now() -> Double { seconds(mach_absolute_time()) }
}

/// Turns `MIDIEvent`s into Universal MIDI Packets and pushes them out of the virtual
/// source and every selected hardware destination.
///
/// Called from the real time MIDI thread, so the endpoint list is behind a lock and the
/// UI only ever swaps it wholesale.
final class MIDISender: @unchecked Sendable {
    private let lock = NSLock()
    private var outputPort: MIDIPortRef = 0
    private var virtualSource: MIDIEndpointRef = 0
    private var destinations: [MIDIEndpointRef] = []

    /// A UMP packet holds 64 words; stay well below that per transmission.
    private let maxEventsPerPacket = 16

    func configure(outputPort: MIDIPortRef, virtualSource: MIDIEndpointRef) {
        lock.lock()
        self.outputPort = outputPort
        self.virtualSource = virtualSource
        lock.unlock()
    }

    func setDestinations(_ endpoints: [MIDIEndpointRef]) {
        lock.lock()
        destinations = endpoints
        lock.unlock()
    }

    func send(_ events: [MIDIEvent]) {
        guard !events.isEmpty else { return }
        lock.lock()
        let port = outputPort
        let source = virtualSource
        let targets = destinations
        lock.unlock()
        guard source != 0 || !targets.isEmpty else { return }

        var index = events.startIndex
        while index < events.endIndex {
            let end = min(index + maxEventsPerPacket, events.endIndex)
            transmit(Array(events[index..<end]), port: port, source: source, targets: targets)
            index = end
        }
    }

    private func transmit(_ events: [MIDIEvent],
                          port: MIDIPortRef,
                          source: MIDIEndpointRef,
                          targets: [MIDIEndpointRef]) {
        let timestamp = mach_absolute_time()
        var list = MIDIEventList()
        withUnsafeMutablePointer(to: &list) { listPtr in
            var packet = MIDIEventListInit(listPtr, ._1_0)
            for event in events {
                var word = Self.universalPacket(for: event)
                packet = MIDIEventListAdd(listPtr,
                                          MemoryLayout<MIDIEventList>.size,
                                          packet,
                                          timestamp,
                                          1,
                                          &word)
            }
            if source != 0 {
                MIDIReceivedEventList(source, listPtr)
            }
            for target in targets {
                MIDISendEventList(port, target, listPtr)
            }
        }
    }

    /// Message type 2 = MIDI 1.0 channel voice, group 0.
    static func universalPacket(for event: MIDIEvent) -> UInt32 {
        let (status, data1, data2) = event.bytes
        return UInt32(0x2000_0000) | (UInt32(status) << 16) | (UInt32(data1) << 8) | UInt32(data2)
    }

    /// Extracts the MIDI 1.0 channel voice messages from an incoming event list.
    /// Everything else — clock, system exclusive, MIDI 2.0 voice messages — is ignored.
    static func parse(_ listPtr: UnsafePointer<MIDIEventList>) -> [MIDIEvent] {
        var events: [MIDIEvent] = []
        for packet in listPtr.unsafeSequence() {
            let stamp = packet.pointee.timeStamp
            // Some drivers stamp "now" as 0.
            let time = stamp == 0 ? Timebase.now() : Timebase.seconds(stamp)
            for word in packet.words() {
                guard (word >> 28) & 0xF == 0x2 else { continue }
                let status = UInt8((word >> 16) & 0xFF)
                let data1 = UInt8((word >> 8) & 0x7F)
                let data2 = UInt8(word & 0x7F)
                if let event = MIDIEvent(status: status, data1: data1, data2: data2, time: time) {
                    events.append(event)
                }
            }
        }
        return events
    }
}
