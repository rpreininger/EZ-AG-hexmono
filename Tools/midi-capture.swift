// Nimmt rohes MIDI von allen angeschlossenen Eingängen auf und protokolliert es mit
// Zeitabständen — das Werkzeug, mit dem sich das Verhalten eines Instruments klären
// lässt, bevor man den Filter darauf einstellt.
//
//   swift Tools/midi-capture.swift [Sekunden]        Voreinstellung: 30
//
// Ausgabe pro Zeile: Zeit, Abstand zur vorherigen Nachricht, Quelle, Kanal, Nachricht.

import CoreMIDI
import Foundation

let duration = Double(CommandLine.arguments.dropFirst().first ?? "") ?? 30

func displayName(_ endpoint: MIDIEndpointRef) -> String {
    var name: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name)
    guard let name else { return "?" }
    return name.takeRetainedValue() as String
}

let lock = NSLock()
var lines: [String] = []
var counts: [String: Int] = [:]
var previousTime: Double?
let start = Date()

func noteName(_ note: UInt8) -> String {
    let names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    return "\(names[Int(note) % 12])\(Int(note) / 12 - 1)"
}

var client = MIDIClientRef()
MIDIClientCreateWithBlock("midi-capture" as CFString, &client, nil)

var port = MIDIPortRef()
MIDIInputPortCreateWithProtocol(client, "capture" as CFString, ._1_0, &port) { listPtr, context in
    let source = context.map { Unmanaged<NSString>.fromOpaque($0).takeUnretainedValue() as String } ?? "?"
    for packet in listPtr.unsafeSequence() {
        for word in packet.words() {
            guard (word >> 28) & 0xF == 0x2 else { continue }
            let status = UInt8((word >> 16) & 0xFF)
            let data1 = UInt8((word >> 8) & 0x7F)
            let data2 = UInt8(word & 0x7F)
            guard status >= 0x80, status < 0xF0 else { continue }

            let now = Date().timeIntervalSince(start)
            let description: String
            switch status & 0xF0 {
            case 0x80:
                description = String(format: "Note Off  %-4@ (%3d)  v%3d", noteName(data1) as NSString, data1, data2)
            case 0x90:
                description = data2 == 0
                    ? String(format: "Note Off  %-4@ (%3d)  v  0  [On/v0]", noteName(data1) as NSString, data1)
                    : String(format: "Note On   %-4@ (%3d)  v%3d", noteName(data1) as NSString, data1, data2)
            case 0xA0: description = String(format: "Poly AT   %-4@        %3d", noteName(data1) as NSString, data2)
            case 0xB0: description = String(format: "CC %3d                %3d", data1, data2)
            case 0xC0: description = String(format: "Program               %3d", data1)
            case 0xD0: description = String(format: "Chan AT               %3d", data1)
            case 0xE0: description = String(format: "Pitch Bend         %6d", Int(data1) | (Int(data2) << 7) - 8192)
            default: continue
            }

            lock.lock()
            let delta = previousTime.map { (now - $0) * 1000 }
            previousTime = now
            let key = String(format: "%02X", status & 0xF0)
            counts[key, default: 0] += 1
            lines.append(String(format: "%8.3f  %8@  %-28@  Kanal %2d   %@",
                                now,
                                (delta.map { String(format: "%.1f ms", $0) } ?? "—") as NSString,
                                source as NSString,
                                Int(status & 0x0F) + 1,
                                description))
            lock.unlock()
        }
    }
}

var connected: [String] = []
for index in 0..<MIDIGetNumberOfSources() {
    let endpoint = MIDIGetSource(index)
    let name = displayName(endpoint)
    guard !name.hasPrefix("MidiFilter") else { continue } // nicht die eigene Ausgabe mitschneiden
    let context = Unmanaged.passRetained(name as NSString).toOpaque()
    MIDIPortConnectSource(port, endpoint, context)
    connected.append(name)
}

FileHandle.standardError.write("Aufnahme läuft (\(Int(duration))s) — \(connected.joined(separator: ", "))\n".data(using: .utf8)!)

DispatchQueue.global().asyncAfter(deadline: .now() + duration) {
    lock.lock()
    let captured = lines
    let summary = counts
    lock.unlock()

    print("     Zeit     Abstand  Quelle                        Kanal      Nachricht")
    print(String(repeating: "─", count: 100))
    for line in captured { print(line) }
    print(String(repeating: "─", count: 100))
    if captured.isEmpty {
        print("Nichts empfangen.")
    } else {
        let kinds = summary.sorted { $0.key < $1.key }
            .map { "\($0.key): \($0.value)" }
            .joined(separator: "   ")
        print("\(captured.count) Nachrichten   [Status-Nibble] \(kinds)")
    }
    exit(0)
}

CFRunLoopRun()
