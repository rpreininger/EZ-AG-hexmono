import Foundation

public enum LogDirection: String, Sendable {
    case input, output
}

public struct LogEntry: Identifiable, Sendable {
    public let id: UInt64
    public let direction: LogDirection
    public let event: MIDIEvent
    /// Only set for incoming events: what the filter decided to do with it.
    public let verdict: Verdict?

    public var time: Double { event.time }
}

/// Ring buffer the MIDI thread writes into and the UI reads from.
public final class EventLog: @unchecked Sendable {
    private let lock = NSLock()
    private var buffer: [LogEntry] = []
    private var nextID: UInt64 = 0
    private var paused = false

    public let capacity: Int

    public init(capacity: Int = 3000) {
        self.capacity = capacity
        buffer.reserveCapacity(capacity)
    }

    public var isPaused: Bool {
        get { lock.lock(); defer { lock.unlock() }; return paused }
        set { lock.lock(); paused = newValue; lock.unlock() }
    }

    public func append(_ direction: LogDirection, _ event: MIDIEvent, verdict: Verdict? = nil) {
        lock.lock()
        defer { lock.unlock() }
        guard !paused else { return }
        buffer.append(LogEntry(id: nextID, direction: direction, event: event, verdict: verdict))
        nextID += 1
        if buffer.count > capacity {
            buffer.removeFirst(buffer.count - capacity)
        }
    }

    public func snapshot() -> [LogEntry] {
        lock.lock()
        defer { lock.unlock() }
        return buffer
    }

    public func clear() {
        lock.lock()
        buffer.removeAll(keepingCapacity: true)
        lock.unlock()
    }

    /// Tab separated dump with the delta time between events, which is what makes the
    /// instrument's behaviour readable.
    public func exportText() -> String {
        let entries = snapshot()
        var lines = ["Zeit(s)\tΔ(ms)\tRichtung\tKanal\tNachricht\tFilter"]
        var previous: Double?
        let start = entries.first?.time ?? 0
        for entry in entries {
            let delta = previous.map { (entry.time - $0) * 1000 } ?? 0
            previous = entry.time
            let columns = [
                String(format: "%8.3f", entry.time - start),
                String(format: "%7.1f", delta),
                entry.direction == .input ? "IN" : "OUT",
                "\(entry.event.channel + 1)",
                entry.event.message.displayName,
                entry.verdict.map { $0 == .passed ? "" : $0.rawValue } ?? "",
            ]
            lines.append(columns.joined(separator: "\t"))
        }
        return lines.joined(separator: "\n")
    }
}
