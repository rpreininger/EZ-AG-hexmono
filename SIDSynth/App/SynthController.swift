import Foundation

/// Der Zustand des Editors, wie in synth_main.cpp der Box: der Klang, der gerade spielt
/// (mit allen noch nicht gespeicherten Änderungen), sein Platz und die Einstellungen.
///
/// `handle(_:)` versteht dieselben Befehlszeilen wie die Box, und `send` schickt dieselben
/// Antworten (`state`, `set`, `stat`, `msg`). Deshalb läuft der Web-Editor der Box hier
/// unverändert. Nur auf dem Hauptthread benutzen.
final class SynthController {
    /// Regler 1–3, solange niemand sie umstellt — wie kDefaultKnobs auf der Box.
    static let defaultKnobs = ["f.cut", "f.res", "l1.rate"]
    static let presetKnob = 3

    let store: PresetStore
    private(set) var edit: Patch
    private(set) var slot = 0
    private(set) var dirty = false
    private(set) var thru: Bool
    private(set) var ezag: Bool
    private(set) var startSlot: Int
    private(set) var knobs: [Int]
    var midiSources = 0

    /// Eine Zeile an den Editor.
    var send: ((String) -> Void)?

    private let defaults = UserDefaults.standard
    private var paramChanges = 0

    init(store: PresetStore = PresetStore()) {
        self.store = store
        thru = defaults.object(forKey: "thru") as? Bool ?? true
        ezag = defaults.bool(forKey: "ezag")
        startSlot = defaults.integer(forKey: "start")
        knobs = (0..<3).map { k in
            let key = UserDefaults.standard.string(forKey: "knob\(k)") ?? SynthController.defaultKnobs[k]
            let index = Int(sid_find_param(key))
            return index >= 0 ? index : Int(sid_find_param(SynthController.defaultKnobs[k]))
        }
        if !store.isUsed(startSlot) { startSlot = store.step(from: -1, direction: 1) }
        slot = startSlot
        edit = store.get(slot)
        edit.values.withUnsafeBufferPointer { sid_engine_begin($0.baseAddress) }
        sid_set_ezag(ezag)
    }

    // MARK: Ausgabe

    func stateMessage() -> String {
        let state: [String: Any] = [
            "slot": slot,
            "start": startSlot,
            "thru": thru ? 1 : 0,
            "ezag": ezag ? 1 : 0,
            "dirty": dirty,
            "name": edit.name,
            "v": edit.values.map(Int.init),
            "names": (0..<Patch.slots).map { store.isUsed($0) ? store.get($0).name : "" },
            "used": (0..<Patch.slots).map { store.isUsed($0) ? 1 : 0 },
            "own": (0..<Patch.slots).map { store.isOwn($0) ? 1 : 0 },
            "knobs": knobs.map { Patch.keys[$0] },
            "enc": false,
        ]
        let data = (try? JSONSerialization.data(withJSONObject: state)) ?? Data("{}".utf8)
        return "state " + String(decoding: data, as: UTF8.self)
    }

    /// Wie status_line() der Box. `usb` heißt hier: mindestens ein MIDI-Eingang da.
    func statusLine() -> String {
        let s = sid_stats()
        return String(format: "{\"load\":%.1f,\"max\":%.1f,\"gap\":0,\"peak\":%d,\"usb\":%@,"
                      + "\"midi\":%u,\"heap\":0,\"drop\":%u}",
                      s.load_avg, s.load_max, s.peak, midiSources > 0 ? "true" : "false",
                      s.midi_in, s.dropped)
    }

    func broadcastState() { send?(stateMessage()) }

    /// Einblendung im Editor.
    func notify(_ text: String) { send?("msg " + text) }

    // MARK: Befehle

    private func updateDirty() {
        let stored = store.get(slot)
        dirty = !store.isUsed(slot) || stored != edit
    }

    /// Ein Parameterwert aus Editor, MIDI-Regler oder Drehregler — alle nehmen diesen Weg.
    func applyParam(_ index: Int, _ value: Int) {
        edit.set(index, value)
        sid_set_param(Int32(index), Int32(edit.values[index]))
        paramChanges += 1
        updateDirty()
        send?("set \(Patch.keys[index]) \(edit.values[index]) \(dirty ? 1 : 0)")
    }

    func loadSlot(_ newSlot: Int) {
        guard store.isUsed(newSlot) else { return }
        edit = store.get(newSlot)
        slot = newSlot
        dirty = false
        edit.values.withUnsafeBufferPointer { sid_load($0.baseAddress) }
        broadcastState()
    }

    func saveSlot(_ target: Int) {
        guard (0..<Patch.slots).contains(target) else { return }
        do {
            try store.save(edit, to: target)
            slot = target
            dirty = false
            broadcastState()
            notify("Gespeichert auf Platz \(target + 1)")
        } catch {
            broadcastState()
            notify("Speichern fehlgeschlagen")
        }
    }

    /// Presets aus einer Sicherung, im Format von /api/backup — wie restore_presets() in
    /// synth_main.cpp: geschrieben wird nur, was sich ändert; der Klang im Editor folgt
    /// seinem Platz, außer er hat ungespeicherte Änderungen. Meldet das Ergebnis als `msg`.
    func restorePresets(_ data: Data) {
        guard let incoming = PresetStore.parseBackup(data), !incoming.isEmpty else {
            notify(PresetStore.parseBackup(data) == nil
                   ? "Datei nicht lesbar – erwartet wird eine Sicherung aus „Alle Presets als Datei sichern“"
                   : "Keine Presets in der Datei")
            return
        }
        let changed = incoming.filter { slot, patch in !(store.isUsed(slot) && store.get(slot) == patch) }
        let edited = dirty
        let start = Date()
        var written = 0
        for (slot, patch) in changed.sorted(by: { $0.key < $1.key }) {
            if (try? store.save(patch, to: slot)) != nil { written += 1 }
        }

        if changed[slot] != nil && !edited {
            loadSlot(slot)
        } else {
            updateDirty()
            broadcastState()
        }
        var message = "\(written)" + (written == 1 ? " Preset" : " Presets") + " geladen"
        if incoming.count > changed.count { message += ", \(incoming.count - changed.count) unverändert" }
        if written < changed.count { message += ", \(changed.count - written) fehlgeschlagen" }
        if !changed.isEmpty { message += " (\(Int(Date().timeIntervalSince(start) * 1000)) ms)" }
        notify(message)
    }

    // MARK: Drehregler (wie turn_knob/press_knob der Box)

    func turnKnob(_ knob: Int, _ delta: Int) {
        if knob == SynthController.presetKnob {
            var target = slot
            for _ in 0..<abs(delta) { target = store.step(from: target, direction: delta > 0 ? 1 : -1) }
            if target != slot { loadSlot(target) }
            return
        }
        let index = knobs[knob]
        let i = Int32(index)
        if sid_param_is_toggle(i) {
            applyParam(index, delta > 0 ? 1 : 0)
            return
        }
        let step = max(1, Int(sid_param_max(i) - sid_param_min(i)) / 128)
        let speed = min(4, abs(delta))
        applyParam(index, Int(edit.values[index]) + delta * step * speed)
    }

    func pressKnob(_ knob: Int) {
        if knob == SynthController.presetKnob {
            loadSlot(slot)
            return
        }
        let index = knobs[knob]
        applyParam(index, Int(store.get(slot).values[index]))
    }

    /// Eine Befehlszeile aus dem Editor — dieselben Befehle wie handle() in synth_main.cpp.
    func handle(_ line: String) {
        let parts = line.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false)
        let command = parts.first.map(String.init) ?? ""
        let args = parts.count > 1 ? String(parts[1]) : ""

        switch command {
        case "set":
            let kv = args.split(separator: " ", maxSplits: 1)
            guard kv.count == 2 else { return }
            let index = Int(sid_find_param(String(kv[0])))
            guard index >= 0 else {
                notify("Unbekannter Parameter \(kv[0])")
                return
            }
            applyParam(index, atoi(kv[1]))
        case "on":
            let nv = args.split(separator: " ")
            let note = nv.first.map(atoi) ?? 0
            sid_note_on(Int32(note & 0xFF), Int32(nv.count > 1 ? atoi(nv[1]) & 0xFF : 100))
        case "off":
            sid_note_off(Int32(atoi(Substring(args)) & 0xFF))
        case "name":
            edit.name = Patch.boxName(args)
            updateDirty()
            broadcastState()
        case "load":
            loadSlot(atoi(Substring(args)))
        case "next":
            loadSlot(store.step(from: slot, direction: 1))
        case "prev":
            loadSlot(store.step(from: slot, direction: -1))
        case "revert":
            loadSlot(slot)
        case "save":
            saveSlot(args.isEmpty ? slot : atoi(Substring(args)))
        case "erase":
            let target = atoi(Substring(args))
            guard store.isOwn(target) else { return }
            let ok = (try? store.erase(target)) != nil
            updateDirty()  // der Klang spielt weiter, ist aber womöglich nirgends mehr gespeichert
            broadcastState()
            notify(ok ? "Platz \(target + 1) gelöscht" : "Löschen fehlgeschlagen")
        case "init":
            edit = Patch.initial(name: "Neuer Klang")
            edit.values.withUnsafeBufferPointer { sid_load($0.baseAddress) }
            updateDirty()
            broadcastState()
        case "panic":
            sid_all_notes_off()
        case "thru":
            // Auf dem Mac gibt es kein MIDI OUT; der Schalter wird nur gemerkt.
            thru = atoi(Substring(args)) != 0
            defaults.set(thru, forKey: "thru")
            broadcastState()
        case "ezag":
            ezag = atoi(Substring(args)) != 0
            sid_set_ezag(ezag)
            defaults.set(ezag, forKey: "ezag")
            broadcastState()
        case "knob":
            let kk = args.split(separator: " ", maxSplits: 1)
            guard kk.count == 2 else { return }
            let knob = atoi(kk[0]) - 1
            let index = Int(sid_find_param(String(kk[1])))
            guard (0...2).contains(knob), index >= 0 else { return }
            knobs[knob] = index
            defaults.set(Patch.keys[index], forKey: "knob\(knob)")
            broadcastState()
        case "turn":
            let kd = args.split(separator: " ")
            guard kd.count == 2 else { return }
            let knob = atoi(kd[0]) - 1
            if (0...3).contains(knob) { turnKnob(knob, atoi(kd[1])) }
        case "press":
            let knob = atoi(Substring(args)) - 1
            if (0...3).contains(knob) { pressKnob(knob) }
        case "start":
            let target = atoi(Substring(args))
            if store.isUsed(target) {
                startSlot = target
                defaults.set(target, forKey: "start")
            }
            broadcastState()
        case "state":
            broadcastState()
        case "":
            break
        default:
            notify("Unbekannter Befehl \(command)")
        }
    }
}

/// Wie atoi() auf der Box: führende Zahl lesen, Rest übergehen, sonst 0.
private func atoi(_ text: Substring) -> Int {
    var digits = Substring("")
    var rest = text.drop { $0 == " " }
    if let first = rest.first, first == "-" || first == "+" {
        digits.append(first)
        rest = rest.dropFirst()
    }
    digits += rest.prefix { $0.isASCII && $0.isNumber }
    return Int(digits) ?? 0
}
