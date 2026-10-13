import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// SID-Synth am Mac: Klänge bauen, so hören wie aus der Box und dann auf die Box bringen.
///
/// Klangerzeugung, Werksklänge, Parametertabelle und Editor sind dieselben Dateien wie in
/// der Firmware (Firmware/src/synth, Firmware/web/index.html). Siehe PORTIERUNG.md.
@main
struct SIDSynthApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        Window("SID-Synth", id: "editor") {
            EditorView(app: delegate.model)
                .frame(minWidth: 380, idealWidth: 1000, minHeight: 500, idealHeight: 780)
        }
        .commands {
            CommandMenu("Box") {
                Button("Presets von der Box holen …") { delegate.model.fetchFromBox() }
                Button("Diesen Klang auf die Box übertragen …") { delegate.model.uploadCurrent() }
                Button("Alle eigenen Klänge auf die Box übertragen …") { delegate.model.uploadAllOwn() }
                Divider()
                Button("Adresse der Box …") { delegate.model.askForHost() }
            }
            CommandGroup(after: .importExport) {
                Button("Presets aus Datei laden …") { delegate.model.importBackup() }
                    .keyboardShortcut("o")
                Button("Presets als Datei sichern …") { delegate.model.exportBackup() }
                    .keyboardShortcut("e", modifiers: [.command, .shift])
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        model.start()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// Hält Klangerzeugung, MIDI und Editor-Zustand zusammen.
final class AppModel {
    let controller = SynthController()
    private let audio = AudioOutput()
    private let midi = MIDIInput()
    private var statusTimer: Timer?
    private var busy = false

    var host: String {
        get { UserDefaults.standard.string(forKey: "boxHost") ?? "sid.local" }
        set { UserDefaults.standard.set(newValue, forKey: "boxHost") }
    }

    func start() {
        do {
            try audio.start()
        } catch {
            show("Keine Audioausgabe", error.localizedDescription)
        }
        midi.onProgramChange = { [weak self] program in self?.controller.loadSlot(program) }
        midi.onSourcesChanged = { [weak self] count in self?.controller.midiSources = count }
        midi.start()
        // Wie die Box: einmal je Sekunde eine Statuszeile an den Editor.
        statusTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.controller.send?("stat " + self.controller.statusLine())
        }
    }

    // MARK: Dateien

    func exportBackup() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "sid-presets.json"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try Data(controller.store.backupJSON().utf8).write(to: url, options: .atomic)
        } catch {
            show("Sichern fehlgeschlagen", error.localizedDescription)
        }
    }

    func importBackup() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try importPatches(from: Data(contentsOf: url), source: url.lastPathComponent)
        } catch {
            show("Laden fehlgeschlagen", error.localizedDescription)
        }
    }

    /// Wie „Presets aus Datei laden" im Editor: erst fragen, dann restorePresets().
    private func importPatches(from data: Data, source: String) throws {
        guard let items = PresetStore.parseBackup(data), !items.isEmpty else {
            controller.restorePresets(data)  // meldet, was mit der Datei nicht stimmt
            return
        }
        let store = controller.store
        let overwritten = items.keys.sorted().filter { store.isOwn($0) && store.get($0) != items[$0] }
        let list = overwritten.map { "\($0 + 1) „\(store.get($0).name)“" }.joined(separator: ", ")
        guard confirm("\(items.count == 1 ? "Ein Preset" : "\(items.count) Presets") aus \(source) laden?",
                      overwritten.isEmpty ? "Eigene Klänge am Mac bleiben unverändert."
                                          : "Überschreibt eigene Klänge auf Platz \(list).") else { return }
        controller.restorePresets(data)
    }

    // MARK: Box

    func askForHost() {
        let alert = NSAlert()
        alert.messageText = "Adresse der Box"
        alert.informativeText = "Im WLAN „SID-Synth“ (Passwort sid12345) ist sie unter sid.local oder 192.168.4.1 erreichbar."
        let field = NSTextField(string: host)
        field.frame = NSRect(x: 0, y: 0, width: 240, height: 24)
        alert.accessoryView = field
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Abbrechen")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let value = field.stringValue.trimmingCharacters(in: .whitespaces)
        if !value.isEmpty { host = value }
    }

    func fetchFromBox() {
        guard !busy else { return }
        busy = true
        let link = BoxLink(host: host)
        Task { @MainActor in
            defer { busy = false }
            do {
                let data = try await link.fetchBackup()
                try importPatches(from: data, source: "der Box")
            } catch {
                show("Holen fehlgeschlagen", error.localizedDescription)
            }
        }
    }

    func uploadCurrent() {
        let store = controller.store
        let slot = controller.slot
        if controller.dirty {
            guard confirm("Der Klang ist noch nicht gespeichert.",
                          "Übertragen wird der gespeicherte Stand von Platz \(slot + 1).") else { return }
        }
        guard store.isUsed(slot) else { return }
        upload([(slot, store.get(slot))])
    }

    func uploadAllOwn() {
        let store = controller.store
        let items = (0..<Patch.slots).filter { store.isOwn($0) }.map { ($0, store.get($0)) }
        guard !items.isEmpty else {
            show("Nichts zu übertragen", "Am Mac gibt es noch keine eigenen Klänge.")
            return
        }
        upload(items)
    }

    private func upload(_ items: [(slot: Int, patch: Patch)]) {
        guard !busy else { return }
        let list = items.map { "\($0.slot + 1) „\($0.patch.name)“" }.joined(separator: ", ")
        guard confirm("\(items.count == 1 ? "Klang" : "\(items.count) Klänge") auf die Box (\(host)) übertragen?",
                      "Plätze: \(list).\n\nGleichnamige Plätze auf der Box werden überschrieben, "
                      + "ungespeicherte Änderungen im Editor der Box gehen verloren. Jedes Speichern "
                      + "macht die Box kurz stumm.") else { return }
        busy = true
        let link = BoxLink(host: host)
        Task { @MainActor in
            defer { busy = false }
            do {
                try await link.upload(items) { [weak self] text in
                    DispatchQueue.main.async { self?.controller.notify(text) }
                }
                controller.notify("Auf der Box: \(items.count == 1 ? "1 Klang" : "\(items.count) Klänge") gespeichert")
            } catch {
                show("Übertragen fehlgeschlagen", error.localizedDescription)
            }
        }
    }

    // MARK: Dialoge

    private func show(_ title: String, _ text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.runModal()
    }

    private func confirm(_ title: String, _ text: String) -> Bool {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Abbrechen")
        return alert.runModal() == .alertFirstButtonReturn
    }
}
