import MidiFilterCore
import SwiftUI

#if os(macOS)
import AppKit
#else
import UIKit
#endif

struct MonitorView: View {
    @EnvironmentObject private var engine: MIDIEngine
    @State private var notesOnly = false
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            columnTitles
            Divider()
            list
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Text("Monitor").font(.title3.weight(.semibold))
            Spacer()
            Toggle("nur Noten", isOn: $notesOnly)
                .toggleStyle(.switch)
                .controlSize(.small)
            Button(engine.isPaused ? "Weiter" : "Pause") { engine.isPaused.toggle() }
            Button("Leeren") { engine.clearLog() }
            Button(copied ? "Kopiert" : "Log kopieren") { copyLog() }
        }
        .padding(10)
    }

    private var columnTitles: some View {
        HStack(spacing: 8) {
            Text("Δ ms").frame(width: 60, alignment: .trailing)
            Text("").frame(width: 34)
            Text("Kan").frame(width: 34, alignment: .trailing)
            Text("Nachricht").frame(maxWidth: .infinity, alignment: .leading)
            Text("Filter").frame(width: 90, alignment: .leading)
        }
        .font(.caption.weight(.semibold))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
    }

    private var list: some View {
        // `entries` is newest first, so the previous event in time is the next element.
        let entries = notesOnly ? engine.entries.filter(\.event.isNote) : engine.entries
        return ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(Array(entries.enumerated()), id: \.element.id) { index, entry in
                    let previous = index + 1 < entries.count ? entries[index + 1] : nil
                    MonitorRow(entry: entry, deltaMs: previous.map { (entry.time - $0.time) * 1000 })
                }
            }
            .padding(.horizontal, 10)
        }
        .background(.background)
    }

    private func copyLog() {
        let text = engine.exportLog()
        #if os(macOS)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        #else
        UIPasteboard.general.string = text
        #endif
        copied = true
        Task {
            try? await Task.sleep(nanoseconds: 1_200_000_000)
            copied = false
        }
    }
}

private struct MonitorRow: View {
    let entry: LogEntry
    let deltaMs: Double?

    var body: some View {
        HStack(spacing: 8) {
            Text(deltaMs.map { String(format: "%.1f", $0) } ?? "—")
                .frame(width: 60, alignment: .trailing)
                .foregroundStyle(.secondary)

            Text(entry.direction == .input ? "IN" : "OUT")
                .frame(width: 34)
                .foregroundStyle(entry.direction == .input ? Color.secondary : Color.accentColor)

            Text("\(entry.event.channel + 1)")
                .frame(width: 34, alignment: .trailing)

            Text(entry.event.message.displayName)
                .frame(maxWidth: .infinity, alignment: .leading)
                .strikethrough(isSuppressed)

            Text(verdictLabel)
                .frame(width: 90, alignment: .leading)
                .foregroundStyle(isSuppressed ? Color.orange : Color.secondary)
        }
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(isSuppressed ? Color.secondary : Color.primary)
        .padding(.vertical, 1)
    }

    private var isSuppressed: Bool { entry.verdict?.isSuppression ?? false }

    private var verdictLabel: String {
        guard let verdict = entry.verdict, verdict != .passed else { return "" }
        return verdict.displayName
    }
}
