// Spielt eine EZ-AG nach: sendet typische Bund-/Anschlag-Sequenzen in den virtuellen
// Eingang "MidiFilter In" der laufenden App und protokolliert, was am virtuellen Ausgang
// "MidiFilter Out" wieder herauskommt. Damit lässt sich die ganze Kette ohne Instrument
// prüfen.
//
//   swift Tools/fake-ezag.swift          einmal durchlaufen
//   swift Tools/fake-ezag.swift loop     endlos wiederholen (zum Zuschauen im Monitor)

import CoreMIDI
import Foundation

func endpoint(named name: String, count: Int, at accessor: (Int) -> MIDIEndpointRef) -> MIDIEndpointRef? {
    for index in 0..<count {
        let endpoint = accessor(index)
        var displayName: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &displayName)
        if let displayName, displayName.takeRetainedValue() as String == name { return endpoint }
    }
    return nil
}

let loopForever = CommandLine.arguments.contains("loop")
let start = Date()
let lock = NSLock()
var transcript: [String] = []

func record(_ line: String) {
    lock.lock()
    transcript.append(line)
    lock.unlock()
    print(line)
}

var client = MIDIClientRef()
MIDIClientCreateWithBlock("fake-ezag" as CFString, &client, nil)

var inputPort = MIDIPortRef()
MIDIInputPortCreateWithProtocol(client, "fake-ezag in" as CFString, ._1_0, &inputPort) { listPtr, _ in
    for packet in listPtr.unsafeSequence() {
        for word in packet.words() {
            guard (word >> 28) & 0xF == 0x2 else { continue }
            let status = UInt8((word >> 16) & 0xFF)
            let data1 = UInt8((word >> 8) & 0x7F)
            let data2 = UInt8(word & 0x7F)
            let kind: String
            switch status & 0xF0 {
            case 0x80: kind = "Note Off"
            case 0x90: kind = data2 == 0 ? "Note Off" : "Note On "
            case 0xB0: kind = "CC      "
            default: kind = String(format: "0x%02X    ", status & 0xF0)
            }
            record(String(format: "%8.1f ms   OUT  Kanal %-2d  %@ %3d  v%3d",
                          Date().timeIntervalSince(start) * 1000,
                          Int(status & 0x0F) + 1, kind, data1, data2))
        }
    }
}

var outputPort = MIDIPortRef()
MIDIOutputPortCreate(client, "fake-ezag out" as CFString, &outputPort)

guard let appOutput = endpoint(named: "MidiFilter Out", count: MIDIGetNumberOfSources(), at: MIDIGetSource),
      let appInput = endpoint(named: "MidiFilter In", count: MIDIGetNumberOfDestinations(), at: MIDIGetDestination) else {
    print("MidiFilter läuft nicht — die virtuellen Ports sind nicht da.")
    exit(1)
}
MIDIPortConnectSource(inputPort, appOutput, nil)

func send(_ status: UInt8, _ data1: UInt8, _ data2: UInt8, _ note: String) {
    record(String(format: "%8.1f ms   IN   Kanal %-2d  %@",
                  Date().timeIntervalSince(start) * 1000, Int(status & 0x0F) + 1, note))
    var word = UInt32(0x2000_0000) | (UInt32(status) << 16) | (UInt32(data1) << 8) | UInt32(data2)
    var list = MIDIEventList()
    withUnsafeMutablePointer(to: &list) { listPtr in
        let packet = MIDIEventListInit(listPtr, ._1_0)
        _ = MIDIEventListAdd(listPtr, MemoryLayout<MIDIEventList>.size, packet, mach_absolute_time(), 1, &word)
        MIDISendEventList(outputPort, appInput, listPtr)
    }
}

/// Kanäle sind hier 1-basiert, wie in der Oberfläche.
func noteOn(_ channel: UInt8, _ note: UInt8, _ velocity: UInt8, _ comment: String = "") {
    send(0x90 | (channel - 1), note, velocity, "Note On  \(note)  v\(velocity)   \(comment)")
}
func noteOff(_ channel: UInt8, _ note: UInt8, _ comment: String = "") {
    send(0x80 | (channel - 1), note, 0, "Note Off \(note)          \(comment)")
}
func pause(_ ms: UInt32) { usleep(ms * 1000) }

/// Anschlag auf eine bereits klingende Saite: Note Off, dann sofort Note On.
func pluck(_ channel: UInt8, _ note: UInt8, _ velocity: UInt8, _ comment: String = "") {
    noteOff(channel, note)
    pause(2)
    noteOn(channel, note, velocity, comment)
}

func playScenarios() {
    print("\n=== 1. Bund drücken, anschlagen, Bund loslassen (Saite 6) ===")
    noteOn(6, 55, 100, "← Bund-Taste")
    pause(400)
    pluck(6, 55, 120, "← Anschlag")
    pause(400)
    noteOff(6, 55, "← Bund losgelassen")
    pause(400)

    print("\n=== 2. Zweite Saite stiehlt die Stimme, dann Rückfall ===")
    noteOn(6, 55, 100, "← Bund Saite 6")
    pause(200)
    pluck(6, 55, 110, "← Anschlag Saite 6")
    pause(300)
    noteOn(5, 60, 100, "← Bund Saite 5")
    pause(200)
    pluck(5, 60, 115, "← Anschlag Saite 5")
    pause(300)
    noteOff(5, 60, "← Saite 5 aus")
    pause(300)
    noteOff(6, 55, "← Saite 6 aus")
    pause(400)

    print("\n=== 3. Kanal 7 liegt ausserhalb 1–6 ===")
    noteOn(7, 72, 100)
    pause(200)
    noteOff(7, 72)
    pause(500)
}

DispatchQueue.global().async {
    pause(300)
    repeat {
        playScenarios()
        if loopForever { pause(1000) }
    } while loopForever
    pause(300)
    exit(0)
}

CFRunLoopRun()
