import SwiftUI

@main
struct MidiFilterApp: App {
    @StateObject private var engine = MIDIEngine()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
                .onAppear { engine.start() }
        }
        #if os(macOS)
        .defaultSize(width: 1040, height: 720)
        .commands {
            CommandGroup(after: .appInfo) {
                Button("Alle Noten aus (Panic)") { engine.panic() }
                    .keyboardShortcut(".", modifiers: [.command])
            }
        }
        #endif
    }
}
