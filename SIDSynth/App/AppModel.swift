import Foundation
#if os(iOS)
import UIKit
#endif

/// Hält Klangerzeugung, MIDI und Editor-Zustand zusammen — eine Instanz für die App, auf
/// Mac und iPad/iPhone gleich.
@MainActor
final class AppModel {
    static let shared = AppModel()

    let controller = SynthController()
    private let audio = AudioOutput()
    private let midi = MIDIInput()
    private var statusTimer: Timer?
    private var started = false
    private var busy = false

    var host: String {
        get { UserDefaults.standard.string(forKey: "boxHost") ?? "sid.local" }
        set { UserDefaults.standard.set(newValue, forKey: "boxHost") }
    }

    #if os(macOS)
    private let device = "am Mac"
    #else
    private let device = UIDevice.current.userInterfaceIdiom == .pad ? "auf dem iPad" : "auf dem iPhone"
    #endif

    func start() {
        guard !started else { return }
        started = true
        do {
            try audio.start()
        } catch {
            Task { await Dialogs.show("Keine Audioausgabe", error.localizedDescription) }
        }
        midi.onProgramChange = { [weak self] program in self?.controller.loadSlot(program) }
        midi.onSourcesChanged = { [weak self] count in self?.controller.midiSources = count }
        midi.start()
        // Wie die Box: einmal je Sekunde eine Statuszeile an den Editor.
        statusTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.controller.send?("stat " + self.controller.statusLine())
            }
        }
        #if os(iOS)
        // Zum Spielen: der Bildschirm soll nicht mitten im Stück ausgehen.
        UIApplication.shared.isIdleTimerDisabled = true
        #endif
    }

    // MARK: Dateien

    func exportBackup() {
        Task {
            do {
                try await Dialogs.save(Data(controller.store.backupJSON().utf8), name: "sid-presets.json")
            } catch {
                await Dialogs.show("Sichern fehlgeschlagen", error.localizedDescription)
            }
        }
    }

    func importBackup() {
        Task {
            do {
                guard let file = try await Dialogs.open() else { return }
                await importPatches(from: file.data, source: file.name)
            } catch {
                await Dialogs.show("Laden fehlgeschlagen", error.localizedDescription)
            }
        }
    }

    /// Wie „Presets aus Datei laden" im Editor: erst fragen, dann restorePresets().
    private func importPatches(from data: Data, source: String) async {
        guard let items = PresetStore.parseBackup(data), !items.isEmpty else {
            controller.restorePresets(data)  // meldet, was mit der Datei nicht stimmt
            return
        }
        let store = controller.store
        let overwritten = items.keys.sorted().filter { store.isOwn($0) && store.get($0) != items[$0] }
        let list = overwritten.map { "\($0 + 1) „\(store.get($0).name)“" }.joined(separator: ", ")
        guard await Dialogs.confirm(
            "\(items.count == 1 ? "Ein Preset" : "\(items.count) Presets") aus \(source) laden?",
            overwritten.isEmpty ? "Eigene Klänge \(device) bleiben unverändert."
                                : "Überschreibt eigene Klänge auf Platz \(list).") else { return }
        controller.restorePresets(data)
    }

    // MARK: Box

    func askForHost() {
        Task {
            guard let value = await Dialogs.askText(
                "Adresse der Box",
                "Im WLAN „SID-Synth“ (Passwort sid12345) ist sie unter sid.local oder 192.168.4.1 erreichbar.",
                value: host) else { return }
            let trimmed = value.trimmingCharacters(in: .whitespaces)
            if !trimmed.isEmpty { host = trimmed }
        }
    }

    func fetchFromBox() {
        guard !busy else { return }
        busy = true
        let link = BoxLink(host: host)
        Task {
            defer { busy = false }
            do {
                let data = try await link.fetchBackup()
                await importPatches(from: data, source: "der Box")
            } catch {
                await Dialogs.show("Holen fehlgeschlagen", error.localizedDescription)
            }
        }
    }

    func uploadCurrent() {
        let store = controller.store
        let slot = controller.slot
        Task {
            if controller.dirty {
                guard await Dialogs.confirm("Der Klang ist noch nicht gespeichert.",
                                            "Übertragen wird der gespeicherte Stand von Platz \(slot + 1).")
                else { return }
            }
            guard store.isUsed(slot) else { return }
            await upload([(slot, store.get(slot))])
        }
    }

    func uploadAllOwn() {
        let store = controller.store
        let items = (0..<Patch.slots).filter { store.isOwn($0) }.map { ($0, store.get($0)) }
        Task {
            guard !items.isEmpty else {
                await Dialogs.show("Nichts zu übertragen", "\(device.prefix(1).uppercased() + device.dropFirst()) gibt es noch keine eigenen Klänge.")
                return
            }
            await upload(items)
        }
    }

    private func upload(_ items: [(slot: Int, patch: Patch)]) async {
        guard !busy else { return }
        let list = items.map { "\($0.slot + 1) „\($0.patch.name)“" }.joined(separator: ", ")
        guard await Dialogs.confirm(
            "\(items.count == 1 ? "Klang" : "\(items.count) Klänge") auf die Box (\(host)) übertragen?",
            "Plätze: \(list).\n\nGleichnamige Plätze auf der Box werden überschrieben, "
            + "ungespeicherte Änderungen im Editor der Box gehen verloren. Jedes Speichern "
            + "macht die Box kurz stumm.") else { return }
        busy = true
        defer { busy = false }
        do {
            try await BoxLink(host: host).upload(items) { [weak self] text in
                DispatchQueue.main.async { self?.controller.notify(text) }
            }
            controller.notify("Auf der Box: \(items.count == 1 ? "1 Klang" : "\(items.count) Klänge") gespeichert")
        } catch {
            await Dialogs.show("Übertragen fehlgeschlagen", error.localizedDescription)
        }
    }
}
