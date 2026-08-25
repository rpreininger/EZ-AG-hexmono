import CoreMIDI
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var engine: MIDIEngine

    var body: some View {
        VStack(spacing: 0) {
            if let error = engine.errorMessage {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                    .background(.red.opacity(0.15))
            }

            #if os(macOS)
            HSplitView {
                settingsColumn
                    .frame(minWidth: 340, idealWidth: 380, maxWidth: 460)
                MonitorView()
                    .frame(minWidth: 420)
            }
            #else
            TabView {
                settingsColumn
                    .tabItem { Label("Einstellungen", systemImage: "slider.horizontal.3") }
                MonitorView()
                    .tabItem { Label("Monitor", systemImage: "waveform") }
            }
            #endif
        }
        .toolbar {
            ToolbarItem {
                Button {
                    engine.panic()
                } label: {
                    Label("Panic", systemImage: "stop.circle")
                }
                .help("Alle Noten aus")
            }
        }
    }

    private var settingsColumn: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                DeviceSection()
                Divider()
                FilterSection()
            }
            .padding(16)
        }
    }
}

struct DeviceSection: View {
    @EnvironmentObject private var engine: MIDIEngine

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeader("Geräte", subtitle: "Eingänge werden zusammengeführt, an alle gewählten Ausgänge gesendet.")

            endpointList(title: "Eingang (EZ-AG)",
                         endpoints: engine.sources,
                         selection: $engine.selectedSourceIDs,
                         empty: "Keine MIDI-Eingänge gefunden.")

            endpointList(title: "Ausgang (Synth)",
                         endpoints: engine.destinations,
                         selection: $engine.selectedDestinationIDs,
                         empty: "Keine MIDI-Ausgänge gefunden.")

            Label("Zusätzlich immer aktiv: virtueller Ausgang „\(engine.virtualSourceName)“ und virtueller Eingang „\(engine.virtualDestinationName)“.",
                  systemImage: "info.circle")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func endpointList(title: String,
                              endpoints: [EndpointInfo],
                              selection: Binding<Set<MIDIUniqueID>>,
                              empty: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            if endpoints.isEmpty {
                Text(empty).font(.caption).foregroundStyle(.secondary)
            } else {
                ForEach(endpoints) { endpoint in
                    Toggle(endpoint.name, isOn: Binding(
                        get: { selection.wrappedValue.contains(endpoint.id) },
                        set: { isOn in
                            if isOn { selection.wrappedValue.insert(endpoint.id) }
                            else { selection.wrappedValue.remove(endpoint.id) }
                        }
                    ))
                }
            }
        }
    }
}

struct SectionHeader: View {
    let title: String
    let subtitle: String?

    init(_ title: String, subtitle: String? = nil) {
        self.title = title
        self.subtitle = subtitle
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.title3.weight(.semibold))
            if let subtitle {
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}
