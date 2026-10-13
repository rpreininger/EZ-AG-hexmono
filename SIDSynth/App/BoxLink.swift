import Foundation

/// Verbindung zur Box über ihr WLAN „SID-Synth" — mit den Wegen, die die Firmware schon
/// hat, ohne neuen Endpunkt:
///
/// - Holen: `GET /api/backup`, alle belegten Plätze in einem Dokument.
/// - Übertragen: über den WebSocket des Editors, je Klang `name <text>`, dann
///   `set <schlüssel> <wert>` für jeden Parameter, dann `save <platz>`.
///
/// Die Box nimmt Befehle in eine Warteschlange mit 32 Plätzen und verwirft, was nicht
/// mehr hineinpasst (web.cpp). Deshalb sind höchstens `window` Befehle unterwegs; jedes
/// `set` bestätigt die Box mit ihrem Echo `set <schlüssel> <wert> <geändert>`.
final class BoxLink {
    enum Failure: LocalizedError {
        case unreachable(String)
        case timeout(String)
        case saveFailed(Int)

        var errorDescription: String? {
            switch self {
            case .unreachable(let host):
                return "Box unter \(host) nicht erreichbar. Ist der Mac im WLAN „SID-Synth“?"
            case .timeout(let step): return "Die Box antwortet nicht (\(step))."
            case .saveFailed(let slot): return "Die Box konnte Platz \(slot + 1) nicht speichern."
            }
        }
    }

    let host: String
    private let window = 8
    private let session: URLSession

    init(host: String) {
        self.host = host
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 5
        session = URLSession(configuration: configuration)
    }

    // MARK: Holen

    func fetchBackup() async throws -> Data {
        guard let url = URL(string: "http://\(host)/api/backup") else { throw Failure.unreachable(host) }
        do {
            let (data, _) = try await session.data(from: url)
            return data
        } catch {
            throw Failure.unreachable(host)
        }
    }

    // MARK: Übertragen

    /// Schreibt die Klänge auf ihre Plätze der Box. Der Klang, der dort gerade im Editor
    /// spielt, geht dabei verloren; danach lädt die Box wieder den Platz von vorher.
    func upload(_ items: [(slot: Int, patch: Patch)], progress: @escaping (String) -> Void) async throws {
        guard let url = URL(string: "ws://\(host)/ws") else { throw Failure.unreachable(host) }
        let socket = session.webSocketTask(with: url)
        socket.resume()
        defer { socket.cancel(with: .goingAway, reason: nil) }

        // Beim Verbinden schickt die Box ihren Zustand; daran sieht man, dass sie da ist.
        let state: String
        do {
            state = try await receive(socket, until: { $0.hasPrefix("state ") }, step: "Verbinden")
        } catch {
            throw Failure.unreachable(host)
        }
        let previousSlot = Self.slot(inState: state)

        for (number, item) in items.enumerated() {
            progress("Übertrage \(number + 1) von \(items.count): Platz \(item.slot + 1) „\(item.patch.name)“")
            try await send(socket, "name \(item.patch.name)")
            _ = try await receive(socket, until: { $0.hasPrefix("state ") }, step: "Name")

            var outstanding = 0
            for (index, key) in Patch.keys.enumerated() {
                try await send(socket, "set \(key) \(item.patch.values[index])")
                outstanding += 1
                if outstanding >= window {
                    _ = try await receive(socket, until: { $0.hasPrefix("set ") }, step: "Werte")
                    outstanding -= 1
                }
            }
            while outstanding > 0 {
                _ = try await receive(socket, until: { $0.hasPrefix("set ") }, step: "Werte")
                outstanding -= 1
            }

            try await send(socket, "save \(item.slot)")
            let answer = try await receive(socket, until: {
                $0.hasPrefix("msg Gespeichert") || $0.hasPrefix("msg Speichern fehlgeschlagen")
            }, step: "Speichern")
            if !answer.hasPrefix("msg Gespeichert") { throw Failure.saveFailed(item.slot) }
        }

        if let previousSlot { try await send(socket, "load \(previousSlot)") }
    }

    private func send(_ socket: URLSessionWebSocketTask, _ line: String) async throws {
        try await socket.send(.string(line))
    }

    /// Wartet auf die nächste Zeile, für die `matches` gilt; alles andere (Statuszeilen,
    /// Echos anderer Browser) wird übergangen.
    private func receive(_ socket: URLSessionWebSocketTask, until matches: @escaping (String) -> Bool,
                         step: String) async throws -> String {
        try await withThrowingTaskGroup(of: String.self) { group in
            group.addTask {
                while true {
                    if case .string(let text) = try await socket.receive(), matches(text) { return text }
                }
            }
            group.addTask {
                try await Task.sleep(nanoseconds: 5_000_000_000)
                // receive() hört nicht auf Abbruch; erst das Schließen beendet es.
                socket.cancel(with: .goingAway, reason: nil)
                throw Failure.timeout(step)
            }
            let result = try await group.next()!
            group.cancelAll()
            return result
        }
    }

    private static func slot(inState state: String) -> Int? {
        guard let data = state.dropFirst("state ".count).data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return (json["slot"] as? NSNumber)?.intValue
    }
}
