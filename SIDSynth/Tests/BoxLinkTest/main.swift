// Prüft das Übertragen auf die Box (BoxLink.swift) gegen eine nachgebaute Box.
//
//   make synth-test   (main.swift, weil mehrere Dateien zusammen übersetzt werden)
//
// Die nachgebaute Box spricht das WebSocket-Protokoll aus web.cpp und synth_main.cpp:
// Zustand beim Verbinden, Echo für jedes `set`, `state` nach `name`, `state` und `msg`
// nach `save`, eine Statuszeile je Sekunde — und eine Warteschlange mit 32 Plätzen, die
// wie auf der Box verwirft, was nicht mehr hineinpasst.

import Foundation
import Network

final class FakeBox {
    var name = ""
    var values = [String: Int]()
    var saved = [Int: (name: String, values: [String: Int])]()
    var loaded: Int?
    var dropped = 0
    let listener: NWListener
    private var inbox: [String] = []
    private let queue = DispatchQueue(label: "fakebox")

    init() throws {
        let parameters = NWParameters.tcp
        let ws = NWProtocolWebSocket.Options()
        ws.autoReplyPing = true
        parameters.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
        listener = try NWListener(using: parameters, on: 0)
        listener.newConnectionHandler = { [weak self] c in self?.accept(c) }
    }

    func start() -> UInt16 {
        let ready = DispatchSemaphore(value: 0)
        listener.stateUpdateHandler = { if case .ready = $0 { ready.signal() } }
        listener.start(queue: queue)
        ready.wait()
        return listener.port!.rawValue
    }

    private func accept(_ c: NWConnection) {
        c.start(queue: queue)
        send(c, "state {\"slot\":2}")
        receive(c)
        // loop() der Box: arbeitet die Warteschlange jede Millisekunde ab.
        func loop() {
            queue.asyncAfter(deadline: .now() + .milliseconds(3)) { [weak self] in
                guard let self else { return }
                let lines = self.inbox
                self.inbox.removeAll()
                for line in lines { self.handle(line, c) }
                loop()
            }
        }
        loop()
        func stat() {
            queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.send(c, "stat {}"); stat() }
        }
        stat()
    }

    private func receive(_ c: NWConnection) {
        c.receiveMessage { [weak self] data, _, _, error in
            guard let self, error == nil else { return }
            if let data, let text = String(data: data, encoding: .utf8) {
                if self.inbox.count >= 32 { self.dropped += 1 } else { self.inbox.append(text) }
            }
            self.receive(c)
        }
    }

    private func handle(_ line: String, _ c: NWConnection) {
        let parts = line.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false)
        let args = parts.count > 1 ? String(parts[1]) : ""
        switch parts[0] {
        case "name":
            name = args
            send(c, "state {\"slot\":2}")
        case "set":
            let kv = args.split(separator: " ")
            values[String(kv[0])] = Int(kv[1])!
            send(c, "set \(kv[0]) \(kv[1]) 1")
        case "save":
            let slot = Int(args)!
            saved[slot] = (name, values)
            send(c, "state {\"slot\":\(slot)}")
            send(c, "msg Gespeichert auf Platz \(slot + 1) (80 ms)")
        case "load":
            loaded = Int(args)
        default:
            break
        }
    }

    private func send(_ c: NWConnection, _ text: String) {
        let meta = NWProtocolWebSocket.Metadata(opcode: .text)
        let context = NWConnection.ContentContext(identifier: "t", metadata: [meta])
        c.send(content: Data(text.utf8), contentContext: context, completion: .idempotent)
    }
}

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { failures += 1; print("FEHLER: \(what)") }
}

let box = try FakeBox()
let port = box.start()
var items: [(slot: Int, patch: Patch)] = []
for (target, source) in [(9, 0), (10, 2), (31, 6)] {
    var patch = Patch.factory(slot: source)!
    patch.name = "Test \(target + 1) ü"
    items.append((target, patch))
}

let done = DispatchSemaphore(value: 0)
Task {
    do {
        try await BoxLink(host: "127.0.0.1:\(port)").upload(items) { print("  " + $0) }
    } catch {
        check(false, "Übertragen: \(error.localizedDescription)")
    }
    done.signal()
}
while done.wait(timeout: .now()) == .timedOut { RunLoop.main.run(until: Date() + 0.01) }
Thread.sleep(forTimeInterval: 0.1)

for (slot, patch) in items {
    guard let got = box.saved[slot] else { check(false, "Platz \(slot + 1) nicht gespeichert"); continue }
    check(got.name == patch.name, "Name auf Platz \(slot + 1): \(got.name)")
    for (index, key) in Patch.keys.enumerated() {
        check(got.values[key] == Int(patch.values[index]), "Platz \(slot + 1) \(key)")
    }
}
check(box.dropped == 0, "\(box.dropped) Befehle verworfen")
check(box.loaded == 2, "danach nicht wieder Platz 3 geladen")
print("BoxLink: \(items.count) Klänge übertragen")

// MARK: Presets aus Datei laden, wie restore_presets() der Box

let folder = FileManager.default.temporaryDirectory.appendingPathComponent("sidsynth-test-\(getpid())")
defer { try? FileManager.default.removeItem(at: folder) }
let controller = SynthController(store: PresetStore(directory: folder))
var messages: [String] = []
controller.send = { messages.append($0) }

var first = Patch.factory(slot: 4)!
first.name = "Zuerst"
var last = Patch.factory(slot: 1)!
last.name = "Zuletzt gilt"
let backup = "{\"fmt\":1,\"presets\":["
    + [Patch.factory(slot: 0)!.json(slot: 0),  // wie der Werksklang: unverändert
       first.json(slot: 9), last.json(slot: 9),  // Platz 10 zweimal: der letzte gilt
       "{\"slot\":40,\"v\":{}}", "{\"slot\":3}"].joined(separator: ",") + "]}"
controller.restorePresets(Data(backup.utf8))
check(messages.last?.hasPrefix("msg 1 Preset geladen, 1 unverändert (") == true, "Meldung: \(messages.last ?? "-")")
check(controller.store.isOwn(9) && controller.store.get(9) == last, "Platz 10")
check(!controller.store.isOwn(0), "Platz 1 bleibt Werksklang")
check(PresetStore(directory: folder).get(9) == last, "Platz 10 auf der Platte")
controller.restorePresets(Data("{\"fmt\":2,\"presets\":[]}".utf8))
check(messages.last?.hasPrefix("msg Datei nicht lesbar") == true, "fmt 2: \(messages.last ?? "-")")
controller.restorePresets(Data("{\"presets\":[]}".utf8))
check(messages.last == "msg Keine Presets in der Datei", "leer: \(messages.last ?? "-")")
print("Presets laden: \(failures) Fehler insgesamt")
exit(failures == 0 ? 0 : 1)
