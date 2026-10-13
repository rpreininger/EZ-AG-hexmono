import SwiftUI
#if os(macOS)
import AppKit
#endif

/// SID-Synth auf Mac, iPad und iPhone: Klänge bauen, so hören wie aus der Box und dann
/// auf die Box bringen.
///
/// Klangerzeugung, Werksklänge, Parametertabelle und Editor sind dieselben Dateien wie in
/// der Firmware (Firmware/src/synth, Firmware/web/index.html). Siehe PORTIERUNG.md.
@main
struct SIDSynthApp: App {
    #if os(macOS)
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate
    #endif

    private var model: AppModel { AppModel.shared }

    var body: some Scene {
        #if os(macOS)
        Window("SID-Synth", id: "editor") {
            EditorView(app: model)
                .frame(minWidth: 380, idealWidth: 1000, minHeight: 500, idealHeight: 780)
        }
        .commands {
            CommandMenu("Box") { BoxMenuItems(model: model) }
            CommandGroup(after: .importExport) { FileMenuItems(model: model) }
        }
        #else
        WindowGroup {
            MobileRoot(model: model)
        }
        #endif
    }
}

/// Holen und Übertragen — auf dem Mac im Menü „Box“, auf iOS im Menü oben rechts.
struct BoxMenuItems: View {
    let model: AppModel

    var body: some View {
        Button("Presets von der Box holen …") { model.fetchFromBox() }
        Button("Diesen Klang auf die Box übertragen …") { model.uploadCurrent() }
        Button("Alle eigenen Klänge auf die Box übertragen …") { model.uploadAllOwn() }
        Divider()
        Button("Adresse der Box …") { model.askForHost() }
    }
}

struct FileMenuItems: View {
    let model: AppModel

    var body: some View {
        Button("Presets aus Datei laden …") { model.importBackup() }
            .keyboardShortcut("o")
        Button("Presets als Datei sichern …") { model.exportBackup() }
            .keyboardShortcut("e", modifiers: [.command, .shift])
    }
}

#if os(macOS)
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        MainActor.assumeIsolated { AppModel.shared.start() }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
#else
/// iPad und iPhone: der Editor über den ganzen Bildschirm, die Befehle der Mac-Menüs
/// in einem Menü oben rechts.
struct MobileRoot: View {
    let model: AppModel

    var body: some View {
        NavigationStack {
            EditorView(app: model)
                .ignoresSafeArea(edges: .bottom)
                .navigationTitle("SID-Synth")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .topBarTrailing) {
                        Menu {
                            BoxMenuItems(model: model)
                            Divider()
                            FileMenuItems(model: model)
                        } label: {
                            Label("Presets", systemImage: "arrow.up.arrow.down.circle")
                        }
                    }
                }
        }
        .onAppear { model.start() }
    }
}
#endif
