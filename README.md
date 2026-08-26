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
Velocity: **Bund immer exakt 16**, Anschläge am 2026-08-20 zwischen 48 und 96, am
2026-08-25 an der laufenden Box bis auf 23 herunter.

Die Schwelle liegt deshalb direkt auf der Bund-Konstanten: **16 und darunter ist Bund,
alles darüber ein Anschlag.** Die 16 ist eine Konstante der Instrumenten-Firmware — die
Bund-Tasten sind nicht anschlagdynamisch, da streut nichts. Wie leise ein Anschlag wird,
hängt dagegen nur am Spiel; die 23 sind bloss das Leiseste einer Sitzung. Ein
Sicherheitsabstand nach oben würde also gegen eine Streuung schützen, die es nicht gibt,
und dafür leise Anschläge kosten. App und Firmware benutzen denselben Wert.

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

## Rutschen auf dem Bund

Ist die Saite angeschlagen und du rutschst mit dem Griff weiter, soll die Tonhöhe
mitwandern, statt dass der Ton endet. Dafür ist **„Griffwechsel ändert die Tonhöhe"**
da (`fretSlide`, in der Voreinstellung „EZ-AG → Mono-Synth" an).

Der Grund, warum das nicht von allein passiert: ein Griffwechsel sieht am MIDI-Kabel
genauso aus wie ein Loslassen mit anschliessendem Anschlag — erst das `Note Off` der
klingenden Note, dann ein `Note On`. Was folgt, weiss man erst eine halbe Millisekunde
später. Der Filter hält deshalb das `Note Off` einer klingenden Note kurz zurück:

```
0.000  ch6  Note Off  E2 (40)          → zurückgehalten
0.001  ch6  Note On   F2 (41)  v16     → Bund-Note: es war ein Griffwechsel
       out  Pitch Bend +4096           → derselbe Ton, einen Halbton höher
```

**Es ist ein Pitch Bend, keine neue Note.** Der angeschlagene Ton klingt ja weiter — ihn
zu verschieben heisst, ihn zu beugen. Die Hüllkurve wird dabei überhaupt nicht angefasst,
und die Velocity des Anschlags bleibt ohnehin erhalten, weil dieselbe Note stehen bleibt.
Was folgt:

- **Der Bend-Bereich muss stimmen.** Der Filter rechnet das Intervall in einen
  Bend-Wert um und braucht dafür den Bereich, auf den der Synth eingestellt ist
  (`pitchBendRange`, voreingestellt ±2 Halbtöne). Stimmt er nicht überein, kommt jedes
  Rutschen im falschen Intervall heraus.
- **Weiter als der Bereich geht es nicht.** Ein Rutschen über mehr Bünde als der Synth
  beugen kann, fällt auf einen echten Notenwechsel zurück — neue Note zuerst, dann die
  alte los, damit ein Mono-Synth wenigstens gleitet. Eine neu angerissene Note ist
  besser als eine falsche Tonhöhe. Wer weit rutschen will, stellt Synth *und* Filter auf
  ±12 und bekommt das ganze Griffbrett gebeugt.
- **Vor dem nächsten Anschlag geht der Bend zurück in die Mitte**, sonst erbt die neue
  Note die Verstimmung des Rutschers.
- **Kommt statt der Bund-Note ein Anschlag**, geht das zurückgehaltene `Note Off` voraus
  und alles läuft wie bisher. Kommt gar nichts, wird es nach dem Fenster nachgereicht —
  ein echtes Loslassen klingt also `slideWindowMs` (6 ms) länger nach. Mehr kostet der
  Modus nicht.

Wer den Notenwechsel lieber mag als den Bend, schaltet **„Als Pitch Bend statt als neue
Note"** aus; dann erklingt bei jedem Griffwechsel eine neue Note mit der Velocity des
Anschlags.

**Noch nicht am Instrument geprüft:** dass ein Rutschen wirklich als Bund-Note
ankommt. Gemessen ist nur, dass *Greifen* auf einer klingenden Saite `Note Off` + `Note
On v16` erzeugt (2026-08-20) — dass ein Weiterrutschen dieselbe Form hat, ist daraus
geschlossen, nicht gemessen. Rührt sich nichts, kündigt die EZ-AG das Rutschen
wahrscheinlich als Hammer-On/Pull-Off mit `CC 84` an; dann ist der Schalter
**„Hammer-On und Pull-Off unterdrücken"** der richtige Hebel — aus, und der Griffwechsel
erklingt mit seiner eigenen Velocity.

## Vorgehen bei einem anderen Instrument

1. Instrument und Synth in der App ankreuzen.
2. Voreinstellung **Diagnose** — alles geht unverändert durch.
3. Im Monitor „nur Noten" einschalten, dann sauber getrennt spielen: erst nur greifen,
   dann nur leere Saiten anschlagen, dann greifen und anschlagen.
4. **Log kopieren** und auswerten: **Δ ms** zeigt die Struktur, **Velocity** trennt die
   Auslöser, **Kanal** zeigt die Zuordnung.

Der Monitor markiert in der Spalte „Filter", warum eine Nachricht verworfen wurde —
`Bund`, `Hammer-On`, `Off verworfen`, `Kanal` — und `Rutsch` für einen Griffwechsel, der
die Tonhöhe verschoben hat.

## Stand

- Filterlogik: 40 Unit-Tests, alle grün, darunter zwei real gemessene Sequenzen
- Ende-zu-Ende über echtes CoreMIDI geprüft, Latenz 0,1–0,4 ms
- An der echten EZ-AG eingestellt und gespielt
- Offen: iOS-Build

## iOS

Das App-Target baut bereits für iOS (`supportedDestinations`), `UIBackgroundModes: audio`
ist gesetzt, damit MIDI im Hintergrund weiterläuft. Zum Bauen auf ein Gerät fehlt nur ein
Signing-Team in `project.yml`.
