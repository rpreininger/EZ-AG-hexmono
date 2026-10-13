import Foundation

/// Die 32 Plätze, aufgebaut wie presets.cpp auf der Box.
///
/// Jeder eigene Klang ist eine Datei `patch-NN.json` (NN ab 00) im selben Format wie im
/// LittleFS der Box. Ein Platz ohne Datei zeigt den Werksklang, falls es einen gibt.
/// Liegt unter ~/Library/Application Support/SIDSynth/Presets.
final class PresetStore {
    let directory: URL
    private(set) var patches: [Patch]
    private(set) var used: [Bool]
    private(set) var own: [Bool]

    init(directory: URL = PresetStore.defaultDirectory) {
        self.directory = directory
        patches = Array(repeating: Patch.initial(name: ""), count: Patch.slots)
        used = Array(repeating: false, count: Patch.slots)
        own = Array(repeating: false, count: Patch.slots)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for slot in 0..<Patch.slots { reload(slot) }
    }

    static var defaultDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("SIDSynth/Presets", isDirectory: true)
    }

    func url(for slot: Int) -> URL {
        directory.appendingPathComponent(String(format: "patch-%02d.json", slot))
    }

    func get(_ slot: Int) -> Patch { patches[(0..<Patch.slots).contains(slot) ? slot : 0] }

    func isUsed(_ slot: Int) -> Bool { (0..<Patch.slots).contains(slot) && used[slot] }

    func isOwn(_ slot: Int) -> Bool { (0..<Patch.slots).contains(slot) && own[slot] }

    /// Der nächste belegte Platz in Richtung `direction`, rundherum — wie presets::step().
    func step(from: Int, direction: Int) -> Int {
        for i in 1...Patch.slots {
            let slot = ((from + direction * i) % Patch.slots + Patch.slots) % Patch.slots
            if used[slot] { return slot }
        }
        return from
    }

    func save(_ patch: Patch, to slot: Int) throws {
        guard (0..<Patch.slots).contains(slot) else { return }
        try Data(patch.json().utf8).write(to: url(for: slot), options: .atomic)
        patches[slot] = patch
        used[slot] = true
        own[slot] = true
    }

    /// Löscht den eigenen Klang; der Platz zeigt dann wieder den Werksklang oder ist leer.
    func erase(_ slot: Int) throws {
        guard isOwn(slot) else { return }
        try FileManager.default.removeItem(at: url(for: slot))
        reload(slot)
    }

    private func reload(_ slot: Int) {
        if let data = try? Data(contentsOf: url(for: slot)),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            patches[slot] = Patch(json: json)
            own[slot] = true
            used[slot] = true
        } else if let factory = Patch.factory(slot: slot) {
            patches[slot] = factory
            own[slot] = false
            used[slot] = true
        } else {
            patches[slot] = Patch.initial(name: "")
            own[slot] = false
            used[slot] = false
        }
    }

    // MARK: Sicherung im Format von /api/backup

    /// Alle belegten Plätze, wie write_backup() auf der Box.
    func backupJSON() -> String {
        let entries = (0..<Patch.slots).filter { used[$0] }.map { patches[$0].json(slot: $0) }
        return "{\"fmt\":1,\"presets\":[" + entries.joined(separator: ",") + "]}"
    }

    /// Liest ein Backup wie read_backup() der Box: `fmt` muss 1 sein (oder fehlen), jeder
    /// Eintrag braucht einen Platz 1…32 und ein `v`. Kommt ein Platz zweimal vor, gilt der
    /// letzte. Liefert die Klänge nach Platz (ab 0), oder nil, wenn das Dokument nicht
    /// lesbar ist.
    static func parseBackup(_ data: Data) -> [Int: Patch]? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (root["fmt"] as? NSNumber)?.intValue ?? 1 == 1,
              let list = root["presets"] as? [Any] else { return nil }
        var found: [Int: Patch] = [:]
        for case let entry as [String: Any] in list {
            let slot = ((entry["slot"] as? NSNumber).flatMap { Int(exactly: $0) } ?? 0) - 1
            guard (0..<Patch.slots).contains(slot), entry["v"] is [String: Any] else { continue }
            found[slot] = Patch(json: entry)
        }
        return found
    }
}
