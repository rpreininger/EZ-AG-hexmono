import CoreMIDI
import Foundation

/// Hört auf alle MIDI-Eingänge — wie die Box, die alles nimmt, was am C2MIDI ankommt.
///
/// Die Nachrichten gehen direkt aus dem MIDI-Thread in sid_midi() und damit in dieselbe
/// Warteschlange wie auf der Box. Nur ein Program Change muss auf den Hauptthread, weil er
/// einen Platz lädt.
final class MIDIInput {
    /// Program Change, auf dem Hauptthread.
    var onProgramChange: ((Int) -> Void)?
    /// Anzahl der verbundenen Eingänge, auf dem Hauptthread.
    var onSourcesChanged: ((Int) -> Void)?

    /// Wie `miditrace 1` an der Konsole der Box: jede Nachricht auf die Konsole. Mit dem
    /// Startargument `-miditrace` an, z. B. `xcrun devicectl device process launch
    /// --console … de.ralfpreininger.SIDSynth -miditrace`.
    private let trace = ProcessInfo.processInfo.arguments.contains("-miditrace")

    private var client = MIDIClientRef()
    private var port = MIDIPortRef()
    private var connected: Set<MIDIUniqueID> = []

    func start() {
        guard client == 0 else { return }
        MIDIClientCreateWithBlock("SIDSynth" as CFString, &client) { [weak self] notification in
            guard notification.pointee.messageID == .msgSetupChanged else { return }
            DispatchQueue.main.async { self?.connectAll() }
        }
        MIDIInputPortCreateWithProtocol(client, "SIDSynth In" as CFString, ._1_0, &port) { [weak self] list, _ in
            self?.receive(list)
        }
        connectAll()
    }

    private func connectAll() {
        if trace { print("MIDI  \(MIDIGetNumberOfSources()) Eingänge gemeldet") }
        var present: Set<MIDIUniqueID> = []
        for i in 0..<MIDIGetNumberOfSources() {
            let source = MIDIGetSource(i)
            var id: MIDIUniqueID = 0
            MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &id)
            present.insert(id)
            if !connected.contains(id) {
                let status = MIDIPortConnectSource(port, source, nil)
                if status == noErr { connected.insert(id) }
                if trace { print("MIDI  Eingang „\(Self.name(of: source))“ verbunden: \(status == noErr ? "ja" : "Fehler \(status)")") }
            }
        }
        connected.formIntersection(present)
        onSourcesChanged?(connected.count)
    }

    /// MIDI-Thread. Nur MIDI-1.0-Kanalnachrichten (UMP-Typ 2), wie sie CoreMIDI für
    /// Geräte mit MIDI 1.0 liefert.
    private func receive(_ list: UnsafePointer<MIDIEventList>) {
        for packet in list.unsafeSequence() {
            for word in packet.words() {
                if trace { print(String(format: "MIDI  UMP %08X", word)) }
                guard (word >> 28) & 0xF == 0x2 else { continue }
                let program = sid_midi(UInt8((word >> 16) & 0xFF), UInt8((word >> 8) & 0x7F), UInt8(word & 0x7F))
                if program >= 0 {
                    DispatchQueue.main.async { [weak self] in self?.onProgramChange?(Int(program)) }
                }
            }
        }
    }

    static func name(of endpoint: MIDIEndpointRef) -> String {
        var name: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name)
        return (name?.takeRetainedValue() as String?) ?? "?"
    }
}
