# MidiFilter Firmware (ESP32)

Der Filter als eigenständige Hardware: Gitarre rein, SidStation raus, kein Rechner
dazwischen.

```sh
make firmware-test     # baut und läuft auf dem Mac, ohne Board
```

## Was der Filter macht

Eine EZ-AG schickt jede Saite auf einem eigenen MIDI-Kanal und meldet dabei auch Töne, die
niemand hören will. Direkt in einen monophonen Synth wie die SidStation gespielt gibt das
ein Durcheinander. Der Filter sitzt dazwischen und räumt auf.

### Was unterdrückt wird

| `Verdict` | Was wegfällt | Woran es erkannt wird |
| --- | --- | --- |
| `SuppressedFret` | Eine gegriffene Note ohne Anschlag | Velocity — Bund-Noten kommen mit exakt 16, Anschläge ab 23; alles bis `velocity_threshold` (20) gilt als Griff |
| `SuppressedHammerOn` | Hammer-On und Pull-Off | Die Gitarre kündigt beides mit **CC 84** (Portamento) an; dieses CC und jedes Note On derselben Saite innerhalb von 20 ms danach fallen weg |
| `SwallowedNoteOff` | Das Note Off zu einem unterdrückten Note On | Bitfeld `suppressed` je Saite — wurde das On verworfen, muss das Off mit, sonst bekommt der Synth ein Off für eine Note, die nie geklungen hat |
| `FilteredChannel` | Alles ausserhalb der Saitenkanäle 1 bis 6 | Kanalbereich |
| `DroppedOther` | Andere Nachrichtenarten, wenn `pass_other_messages` aus ist | Nachrichtenart |

### Was mit dem Rest geschieht

- **Zusammenlegen**: alle sechs Saitenkanäle gehen auf einen Ausgangskanal.
- **Monophone Stimmenverwaltung** mit Last-Note-Priorität — ein neuer Anschlag übernimmt die
  Stimme und fällt beim Loslassen nicht auf eine noch gehaltene Note zurück.
- **Legato**: das Note On der neuen Note geht vor dem Note Off der alten hinaus, damit der
  Synth gleitet statt neu anzuschlagen.
- **All Notes Off** (CC 123) beim Start und bei jedem Wechsel der Konfiguration, damit die
  SidStation nie in einer Note hängen bleibt.

## Was hier schon fertig ist

`src/midi_filter.h` / `.cpp` — die Portierung von `EZAGFilter.swift` auf C++17. Gleiches
Verhalten und gleiche Namen, aber:

- **keine dynamische Speicherverwaltung**, keine Ausnahmen, keine RTTI
- **`uint32_t`-Mikrosekunden statt `double`-Sekunden** — Fließkomma gehört nicht in den
  MIDI-Pfad eines Mikrocontrollers
- Zeitvergleiche über vorzeichenlose Differenzen, dadurch **überlaufsicher**: `micros()`
  läuft nach rund 71 Minuten über, ohne dass der Filter etwas davon merkt (getestet)
- gehaltene und unterdrückte Noten als Bitfelder, 32 Byte pro Saite statt Mengen

`src/midi_event.h` / `.cpp` — der Byte-Parser, den es auf dem Mac gar nicht gibt.
CoreMIDI liefert fertige Nachrichten; an einer UART kommt MIDI roh an und will richtig
zerlegt werden:

- **Running Status** — nach `90 3C 40` folgt oft nur noch `3E 40`, ohne Statusbyte
- **Echtzeit-Bytes** (`F8` Clock, `FE` Active Sensing) stehen mitten in Nachrichten und
  dürfen den Zustand nicht durcheinanderbringen
- **SysEx** und System Common werden übersprungen, ohne den Parser zu verwirren
- `Note On` mit Velocity 0 wird zu `Note Off` normalisiert

Der Modus „kurze Noten verwerfen" der Mac-App fehlt hier bewusst: die Messung am
Instrument hat gezeigt, dass Bund-Noten gehalten werden und keine Blitzer sind.

## Tests

`test/test_firmware.cpp`, 78 Prüfungen ohne Testframework — ein Compiler-Aufruf, läuft in
Millisekunden. Darunter dieselben beiden **real an der EZ-AG gemessenen Sequenzen** wie
auf dem Mac, damit beide Umsetzungen nachweislich dasselbe tun.

## Die Firmware

`src/main.cpp`, für den ESP32-S3 mit Arduino-Framework:

```sh
pio run                # übersetzen
pio run -t upload      # flashen
pio device monitor     # serielle Ausgabe
```

Die Hardware steckt hinter `MidiPort`. Zurzeit gibt es `UartMidiPort` für DIN-Buchsen;
alles darüber ist die geprüfte, portable Logik. Die gemessenen EZ-AG-Werte stehen fest
einkompiliert in `ezag_config()`.

## Verdrahtung

```
MIDI IN (DIN)                              ESP32-S3
  Pin 4 ──[220 Ω]──┬── Optokoppler ────────── GPIO 17  (RX)
  Pin 5 ───────────┘   (6N138 / H11L1)

MIDI OUT (DIN)
  Pin 4 ──[220 Ω]────────────────────────── 3V3
  Pin 5 ──[220 Ω]────────────────────────── GPIO 18  (TX)
  Pin 2 ─────────────────────────────────── GND
```

Der Optokoppler am Eingang ist nicht optional — er ist im MIDI-Standard vorgeschrieben
und trennt die Masse beider Geräte, damit keine Brummschleife entsteht. Fertige
Breakout-Platinen mit beiden Buchsen gibt es für wenige Euro.

**GPIO 19 und 20 des S3 nicht verwenden**, die liegen am USB.

## USB-Host statt DIN: der CME C2MIDI Pro

Am 2026-08-25 nachgereicht und am Instrument geprüft. Der C2MIDI Pro ist ein
USB-MIDI-**Gerät**, der S3 arbeitet ihm gegenüber als USB-**Host** und versorgt ihn mit
den 5 V, die er braucht (116 mW). Das Kabel bringt beide DIN-Buchsen mit, **der
Optokoppler entfällt damit**: der weisse Stecker geht an MIDI OUT der Gitarre, der
schwarze an MIDI IN des Klangerzeugers.

TinyUSB scheidet dafür aus — im Arduino-Kern ist nur die Geräteseite einkompiliert,
`class/midi/` enthält `midi_device.h` und kein `midi_host.h`. Benutzt wird stattdessen
der native USB-Host-Stack von ESP-IDF (`usb/usb_host.h`), der im Kern **2.0.17** bereits
vorhanden ist; ein Wechsel auf Kern 3.x ist nicht nötig.

`src/USBConnection.*` und `src/MIDITransport.h` stammen aus ESP32_Host_MIDI 7.2.0 von
Saulo Veríssimo (MIT, siehe `src/USBConnection.LICENSE`). Übernommen ist nur der
USB-Transport. **Eine Änderung war nötig:** das Original verwarf jede Nachricht, solange
ein OUT-Transfer lief — bei Legato wäre damit das Note Off der alten Note verschwunden
und der Synth in der Note hängengeblieben. Jetzt wird eingereiht, siehe `TX_QUEUE_SIZE`.

Welcher Weg benutzt wird, entscheidet `kUseUsbHost` in `src/main.cpp`. Auf `false` läuft
weiterhin `UartMidiPort` über die DIN-Verdrahtung oben — die Filterlogik und ihre Tests
merken von beidem nichts, dafür ist `MidiPort` da.

## Noch offen

- ~~Verdrahtung am echten Board prüfen~~ — am 2026-08-25 über den USB-Host-Weg erledigt:
  alle sechs Saitenkanäle kommen an, gehen auf Kanal 1 hinaus, Legato-Reihenfolge stimmt.
  Die DIN-Verdrahtung mit Optokoppler ist dagegen weiterhin ungeprüft.
- Die Anschlagschwelle nochmal sauber ausmessen. Am 2026-08-20 hiess es 48 bis 96, am
  2026-08-25 lagen Anschläge zwischen 23 und 124 — mit der alten Schwelle 32 fielen leise
  Anschläge stumm heraus. 20 liegt in der gemessenen Lücke zwischen 16 und 23, ist aber
  aus einer einzigen Sitzung abgeleitet. Die Mac-App benutzt seit dem 2026-08-26 denselben
  Wert, eine Korrektur gehört also in beide Voreinstellungen.
- Optional: stromloser Anlauf — die SidStation hängt nach einem Neustart der Box nie in
  einer Note, weil `setup()` ein All Notes Off schickt; das gilt aber nicht umgekehrt,
  wenn die Box mitten im Spiel abstürzt
