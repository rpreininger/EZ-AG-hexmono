import MidiFilterCore
import SwiftUI

struct FilterSection: View {
    @EnvironmentObject private var engine: MIDIEngine

    private var config: Binding<FilterConfig> { $engine.config }

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            presets
            channels
            monophony
            fretSuppression
        }
    }

    // MARK: Presets

    private var presets: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionHeader("Voreinstellungen")
            HStack {
                Button("Diagnose") { engine.config = .diagnostic }
                    .help("Alles durchlassen, nichts verändern — um im Monitor zu sehen, was die Gitarre wirklich sendet.")
                Button("EZ-AG → Mono-Synth") { engine.config = .ezagMono }
                    .help("Kanäle 1–6 auf 1, monophon, Bund-Unterdrückung über Retrigger-Erkennung.")
            }
        }
    }

    // MARK: Channels

    private var channels: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionHeader("Kanäle", subtitle: "Die EZ-AG sendet jede Saite auf einem eigenen Kanal.")
            HStack {
                channelPicker("von", selection: config.lowestInputChannel)
                channelPicker("bis", selection: config.highestInputChannel)
                channelPicker("auf Kanal", selection: config.outputChannel)
            }
            Picker("Andere Kanäle", selection: config.otherChannels) {
                ForEach(OtherChannelPolicy.allCases, id: \.self) { policy in
                    Text(policy.displayName).tag(policy)
                }
            }
            Toggle("CC, Pitch Bend usw. weiterleiten", isOn: config.passOtherMessages)
            Stepper("Transponieren: \(engine.config.transpose > 0 ? "+" : "")\(engine.config.transpose) Halbtöne",
                    value: config.transpose, in: -24...24)
        }
    }

    private func channelPicker(_ label: String, selection: Binding<UInt8>) -> some View {
        Picker(label, selection: selection) {
            ForEach(0..<16, id: \.self) { channel in
                Text("\(channel + 1)").tag(UInt8(channel))
            }
        }
        .frame(maxWidth: 130)
    }

    // MARK: Monophony

    private var monophony: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionHeader("Monophon", subtitle: "Sechs Saiten, eine Stimme — der Synth bekommt immer nur eine Note.")
            Toggle("Auf eine Stimme reduzieren", isOn: config.monophonic)
            Group {
                Picker("Priorität", selection: config.priority) {
                    ForEach(NotePriority.allCases, id: \.self) { priority in
                        Text(priority.displayName).tag(priority)
                    }
                }
                .disabled(!engine.config.fallbackToHeldNote)
                .help("Wirkt nur zusammen mit dem Zurückfallen: sie entscheidet, zu welcher gehaltenen Note der Synth zurückkehrt. Ein neuer Anschlag übernimmt die Stimme immer.")
                Toggle("Legato (neue Note vor Note Off der alten)", isOn: config.legato)
                    .help("Für Mono-Synths im Legato-Modus: die Hüllkurve wird nicht neu getriggert, der Synth gleitet.")
                Toggle("Gleiche Note bei erneutem Anschlag neu triggern", isOn: config.retriggerSameNote)
                Toggle("Beim Loslassen auf gehaltene Note zurückfallen", isOn: config.fallbackToHeldNote)
                    .help("Aus: es klingt immer nur die zuletzt angeschlagene Note, Loslassen beendet sie. An: der Synth kehrt zu einer noch gehaltenen Note zurück — üblich bei Mono-Synths, auf der Gitarre tauchen dadurch alte Noten wieder auf.")
            }
            .disabled(!engine.config.monophonic)
        }
    }

    // MARK: Fret suppression

    private var fretSuppression: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionHeader("Bund-Tasten unterdrücken",
                          subtitle: "Bund und Anschlag senden dieselbe Note — die Unterscheidung geht nur über den Kontext.")

            Picker("Strategie", selection: config.fretMode) {
                ForEach(FretMode.allCases, id: \.self) { mode in
                    Text(mode.displayName).tag(mode)
                }
            }

            Text(engine.config.fretMode.explanation)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()

            Toggle("Hammer-On und Pull-Off unterdrücken", isOn: config.suppressHammerOns)
            Text("Beim Greifen ohne Anschlag sendet die EZ-AG die neue Note mit voller Velocity — die Velocity-Schwelle kann sie nicht erkennen. Sie kündigt sie aber mit CC 84 an. Unterdrückt klingt die angeschlagene Note einfach weiter, es erklingt also nur, was du wirklich anschlägst.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()

            Toggle("Griffwechsel ändert die Tonhöhe (Rutschen)", isOn: config.fretSlide)
                .disabled(engine.config.fretMode == .off || engine.config.fretMode == .blip)
            Text("Rutschst du auf dem Bund weiter, während die Saite noch klingt, wandert die Tonhöhe mit — statt dass die Bund-Note verworfen wird und der Ton endet. Die Velocity des Anschlags bleibt erhalten. Dafür wird das Note Off einer klingenden Note um \(Int(engine.config.slideWindowMs)) ms zurückgehalten, um den Griffwechsel vom Loslassen zu unterscheiden.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            if engine.config.fretSlide {
                Toggle("Als Pitch Bend statt als neue Note", isOn: config.slideUsesPitchBend)
                Text("Es klingt ja weiterhin derselbe Anschlag — ihn zu verschieben ist ein Pitch Bend. Die Hüllkurve wird dabei überhaupt nicht angefasst. Passt das Intervall nicht in den Bend-Bereich, wird auf einen echten Notenwechsel zurückgefallen.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                if engine.config.slideUsesPitchBend {
                    Stepper("Bend-Bereich des Synths: ±\(engine.config.pitchBendRange) Halbtöne",
                            value: config.pitchBendRange, in: 1...24)
                        .help("Muss mit der Einstellung am Synth übereinstimmen, sonst kommt jedes Rutschen im falschen Intervall heraus. ±2 ist fast überall die Voreinstellung.")
                }

                Slider(value: config.slideWindowMs, in: 2...40, step: 1) {
                    Text("Fenster")
                } minimumValueLabel: {
                    Text("2 ms").font(.caption2)
                } maximumValueLabel: {
                    Text("40 ms").font(.caption2)
                }
            }

            switch engine.config.fretMode {
            case .off:
                EmptyView()

            case .velocity:
                Stepper("Velocity-Schwelle: \(engine.config.velocityThreshold)",
                        value: config.velocityThreshold, in: 1...127)
                Toggle("Nur exakt diesen Wert verwerfen", isOn: config.velocityExactMatch)
                    .help("Wenn die Bund-Tasten immer denselben Velocity-Wert senden, ist das die präziseste Variante.")

            case .retrigger:
                Slider(value: config.retriggerWindowMs, in: 5...300, step: 5) {
                    Text("Fenster")
                } minimumValueLabel: {
                    Text("5 ms").font(.caption2)
                } maximumValueLabel: {
                    Text("300 ms").font(.caption2)
                }
                Text("Fenster: \(Int(engine.config.retriggerWindowMs)) ms — so lange nach einem Note Off gilt ein Note On als Anschlag.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle("Erster Anschlag einer stummen Saite durchlassen", isOn: config.allowFirstNoteOnIdleString)
                    .help("Sonst geht der erste Anschlag einer leeren Saite verloren. Dafür rutscht ein Bund-Druck auf einer stummen Saite durch.")

            case .blip:
                Slider(value: config.blipMaxMs, in: 5...200, step: 5) {
                    Text("Maximale Länge")
                } minimumValueLabel: {
                    Text("5 ms").font(.caption2)
                } maximumValueLabel: {
                    Text("200 ms").font(.caption2)
                }
                Text("Noten kürzer als \(Int(engine.config.blipMaxMs)) ms werden verworfen. Achtung: erzeugt genau diese Latenz.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

extension FilterConfig {
    /// Everything through, nothing changed — for looking at the raw instrument output.
    static var diagnostic: FilterConfig {
        var config = FilterConfig()
        config.lowestInputChannel = 0
        config.highestInputChannel = 15
        config.monophonic = false
        config.fretMode = .off
        config.otherChannels = .passThrough
        return config
    }

    /// The intended setup, tuned against a real EZ-AG on 2026-08-20.
    ///
    /// Measured: string N sends on channel N. A fret press and a pluck are structurally
    /// identical — both arrive as `Note Off` followed by `Note On` about half a
    /// millisecond later — so only the velocity tells them apart. Fret notes come in at
    /// exactly 16 every time. Plucks looked like 48 to 95 on the first session, but on
    /// 2026-08-25 quiet ones reached down to 23 — hence the threshold at 20, the same
    /// value the firmware runs.
    static var ezagMono: FilterConfig {
        var config = FilterConfig()
        config.lowestInputChannel = 0
        config.highestInputChannel = 5
        config.outputChannel = 0
        config.monophonic = true
        config.priority = .last
        config.fallbackToHeldNote = false
        config.legato = false
        config.fretMode = .velocity
        config.velocityThreshold = 16
        config.velocityExactMatch = false
        config.fretSlide = true
        return config
    }
}
