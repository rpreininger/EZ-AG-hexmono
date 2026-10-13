# MidiFilter

Führt die sechs Saitenkanäle einer Yamaha EZ-AG auf einen Kanal zusammen, wirft die von
den Bund-Tasten erzeugten Noten weg und reduziert den Rest auf eine Stimme — damit ein
monophoner Synth sinnvoll damit spielbar ist.

macOS zuerst, iOS ist vorbereitet: die gesamte Logik liegt plattformunabhängig im Paket
`MidiFilterCore`, das App-Target baut bereits für beide Plattformen.

## Bauen und starten

```sh
make run             # generiert das Projekt, baut und startet die App
make test            # Unit-Tests der Filterlogik (ohne Xcode, ohne Hardware)
make demo            # simulierte EZ-AG gegen die laufende App, kein Instrument nötig
make firmware-test   # Tests der ESP32-Portierung, auf dem Rechner
make synth-run       # SID-Synth am Mac: Presets bauen, siehe PORTIERUNG.md
make synth-test      # Klangerzeugung der Box und Übertragen, auf dem Rechner
```

Ohne `make`:

```sh
xcodegen generate
xcodebuild -project MidiFilter.xcodeproj -scheme MidiFilter \
           -destination 'platform=macOS' -derivedDataPath build build
open build/Build/Products/Debug/MidiFilter.app
```

## Aufbau

```
Packages/MidiFilterCore/     plattformunabhängige Logik + Tests
  MIDIEvent.swift            Nachrichtenmodell, Parsing, Notennamen
  FilterConfig.swift         alle Einstellungen, Codable
  EZAGFilter.swift           Kanal-Merge, Bund-Erkennung, Mono-Stimmenverwaltung
  EventLog.swift             Ringpuffer für den Monitor, Export
Sources/MIDI/                CoreMIDI (läuft auf macOS und iOS)
  MIDISender.swift           Universal MIDI Packets senden/parsen, Zeitbasis
  MIDIRouter.swift           Brücke zwischen MIDI-Thread und App
  MIDIEngine.swift           Client, Ports, Geräteliste, Persistenz
Sources/App/                 SwiftUI
Firmware/                    ESP32-S3: dieselbe Logik in C++, siehe Firmware/README.md
Tools/                       midi-capture.swift, fake-ezag.swift
SIDSynth/                    Mac-App für Presets: Klangerzeugung + Editor der Box
```

Die App legt immer zwei virtuelle Ports an:

- **MidiFilter In** — hier kann Software hineinsenden (wird immer verarbeitet)
- **MidiFilter Out** — hier hängen sich Software-Synths und DAWs ein

Zusätzlich lassen sich beliebige Hardware-Eingänge und -Ausgänge auswählen
(z. B. `ESI MIDIMATE eX Anschluss 1`).

## Signalweg

```
Eingänge (Kanal 1–6)
      ↓  Kanalfilter        Kanäle ausserhalb des Bereichs: verwerfen oder durchlassen
      ↓  Bund-Unterdrückung Note On der Bund-Tasten verwerfen (+ zugehöriges Note Off)
      ↓  Merge              alles auf den Ausgangskanal
      ↓  Monophonie         Notenstack, Priorität, Legato, Transponierung
Ausgänge
```

## Gemessenes Verhalten der EZ-AG

Am 2026-08-20 am Instrument aufgezeichnet. Diese Messungen sind die Grundlage der
Voreinstellung „EZ-AG → Mono-Synth" und als Regressionstests festgehalten
(`testMeasuredFretAndPluckSequence`, `testMeasuredHammerOnSequence`).

**Saite N sendet auf Kanal N.** Leer angeschlagen:

| Kanal | 6 | 5 | 4 | 3 | 2 | 1 |
|---|---|---|---|---|---|---|
| Note | E2 (40) | A2 (45) | D3 (50) | G3 (55) | B3 (59) | E4 (64) |

**Ein Anschlag sendet kein Note Off.** Die Saite klingt unbegrenzt weiter, bis sie erneut
angeschlagen, gegriffen oder losgelassen wird.

**Greifen auf einer klingenden Saite** beendet die alte Note und sendet die neue:

```
0.000  ch6  Note Off  E2 (40)          klingende leere Saite endet
0.001  ch6  Note On   B2 (47)  v16     die Bund-Note
1.048  ch6  Note Off  B2 (47)
1.049  ch6  Note On   B2 (47)  v92     der Anschlag
1.683  ch6  Note Off  B2 (47)          Bund losgelassen
```

Bund-Druck und Anschlag sind **strukturell nicht zu unterscheiden** — beide sind ein
`Note Off` gefolgt von einem `Note On` nach etwa einer halben Millisekunde. Eine
Retrigger-Erkennung kann hier nichts ausrichten. Der einzige Unterschied ist die
Velocity: **Bund immer exakt 16**, Anschläge zwischen 48 und 96. Die Schwelle liegt
deshalb bei 32.

**Hammer-On und Pull-Off** kommen dagegen mit voller Velocity und sind über die
Velocity nicht greifbar. Die EZ-AG kündigt sie aber jeweils rund eine Millisekunde
vorher mit `CC 84` an (Portamento Control, Wert = Ausgangsnote):

```
1.327  ch1  Note On   C5  (72) v96     Anschlag
1.935  ch1  CC 84  72
1.936  ch1  Note On   C#5 (73) v96     Hammer-On
1.936  ch1  Note Off  C5  (72)         die alte Note endet erst danach
```

Werden Hammer-Ons unterdrückt, muss das `Note Off` der ersetzten Note **mit**
verworfen werden — sonst verstummt der Synth beim Griffwechsel, statt die angeschlagene
Note weiterzuführen. Das letzte Loslassen muss dagegen durchkommen, sonst hängt die Note.

## Vorgehen bei einem anderen Instrument

1. Instrument und Synth in der App ankreuzen.
2. Voreinstellung **Diagnose** — alles geht unverändert durch.
3. Im Monitor „nur Noten" einschalten, dann sauber getrennt spielen: erst nur greifen,
   dann nur leere Saiten anschlagen, dann greifen und anschlagen.
4. **Log kopieren** und auswerten: **Δ ms** zeigt die Struktur, **Velocity** trennt die
   Auslöser, **Kanal** zeigt die Zuordnung.

Der Monitor markiert in der Spalte „Filter", warum eine Nachricht verworfen wurde —
`Bund`, `Hammer-On`, `Off verworfen`, `Kanal`.

## Stand

- Filterlogik: 30 Unit-Tests, alle grün, darunter zwei real gemessene Sequenzen
- Ende-zu-Ende über echtes CoreMIDI geprüft, Latenz 0,1–0,4 ms
- An der echten EZ-AG eingestellt und gespielt
- Offen: iOS-Build

## iOS

Das App-Target baut bereits für iOS (`supportedDestinations`), `UIBackgroundModes: audio`
ist gesetzt, damit MIDI im Hintergrund weiterläuft. Zum Bauen auf ein Gerät fehlt nur ein
Signing-Team in `project.yml`.
