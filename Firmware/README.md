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

`test/test_firmware.cpp`, 73 Prüfungen ohne Testframework — ein Compiler-Aufruf, läuft in
Millisekunden. Darunter dieselben beiden **real an der EZ-AG gemessenen Sequenzen** wie
auf dem Mac, damit beide Umsetzungen nachweislich dasselbe tun.

## Die Firmware

`src/main.cpp`, für den ESP32-S3 mit Arduino-Framework:

```sh
pio run                # übersetzen
pio run -t upload      # flashen
pio device monitor     # serielle Ausgabe
```

Die Hardware steckt hinter `MidiPort` in `src/midi_ports.h`: `UartMidiPort` für
DIN-Buchsen, `UsbHostMidiPort` für den C2MIDI Pro. Alles darüber ist die geprüfte,
portable Logik. Die gemessenen EZ-AG-Werte stehen fest einkompiliert in
`ezag_config()` (`src/ezag_config.h`) — dieselben für die Filter-Box und den Synth.

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

## Der eingebaute SID-Synth

Eine zweite Firmware macht aus der Box einen SID-Synthesizer: ein nachgebildeter SID-Chip
statt (oder zusätzlich zu) der SidStation.

```sh
pio run -e synth -t upload    # flashen; die Presets im Flash bleiben dabei stehen
```

```
Casio DG-20 → C2MIDI Pro → [EZ-AG-Filter] → SID-Engine (Kern 1) → I2S → PCM5102A
                                          ↘ MIDI OUT → SidStation
```

Gespielt wird mit der **Casio DG-20**; ihr MIDI geht ungefiltert in die Engine, auf allen
Kanälen. Der EZ-AG-Filter von oben lässt sich im Editor unter „Einstellungen"
zuschalten, die Weiterleitung an MIDI OUT ebenso abschalten. Program Change wählt das
Preset mit derselben Nummer (0 = Platz 1). Was ein Instrument tatsächlich schickt,
zeigt `miditrace 1` auf der Konsole.

**Klang bearbeiten:** mit dem WLAN **SID-Synth** verbinden (Passwort `sid12345`), dann
**http://sid.local** oder http://192.168.4.1 öffnen. Jede Reglerbewegung ist nach 3 ms
im Klang; eine Tastatur auf der Seite spielt Noten, auch ohne Gitarre. Solange kein DAC
angeschlossen ist, lässt sich der Klang mit „Ton im Browser" vorhören — etwa 150 ms
verzögert und mit halber Abtastrate, also nur zum Einstellen.

**Klangerzeugung** wie bei der SidStation: drei Oszillatoren auf einer Note (Wellenform,
Transponierung, Feinstimmung, Pulsbreite, SID-Hüllkurve, Sync, Ringmodulation), der
Filter des 6581 oder 8580, eine Hüllkurve für Filter und Tonhöhe, zwei LFOs, Glide,
Legato, Pitchbend. Gerechnet von reSID 0.16 im Modus SAMPLE_FAST bei 44,1 kHz — gut 14 %
eines Kerns. Die Modulation läuft blockweise, 344-mal pro Sekunde.

**Presets:** 32 Plätze, je eine JSON-Datei im LittleFS, beim Start alle im RAM. Plätze 1–8
haben Werksklänge; ein eigener Klang auf einem dieser Plätze überdeckt den Werksklang, bis
er gelöscht wird. Gespeichert wird nur auf Knopfdruck, und vorher wird ausgeblendet:
jeder Schreibzugriff auf den Flash hält beide Kerne bis zu 40 ms an, länger als der
Ausgabepuffer (11,6 ms) reicht. „Alle Presets als Datei sichern" im Editor lädt eine
JSON-Datei mit allen Klängen herunter, „Presets aus Datei laden" spielt so eine Datei
wieder ein (`POST /api/restore`). Geschrieben werden nur Plätze, die sich ändern; Plätze,
die nicht in der Datei stehen, bleiben. Der Ton setzt dabei kurz aus, bei einer vollen
Datei bis zu drei Sekunden.

| Datei | Inhalt |
| --- | --- |
| `src/synth/params.h` | **die eine Parametertabelle** — Engine, Presets und Editor lesen sie; ein neuer Parameter ist eine Zeile |
| `src/synth/engine.*` | reSID, Modulation, I2S, Ein-/Ausblenden, Vorhör-Puffer |
| `src/synth/presets.*` | Plätze, Werksklänge, JSON lesen und schreiben |
| `src/synth/web.*` | WLAN-Zugangspunkt, Webserver, WebSocket, `/api/schema`, `/api/backup`, `/api/restore` |
| `src/synth/controls.*` | das Encoder-Board am I2C, in einer eigenen Aufgabe auf Kern 0 |
| `src/synth/synth_main.cpp` | Zustand des Editors, Befehle, MIDI vom Filter zur Engine |
| `web/index.html` | der Editor; `tools/embed_web.py` packt ihn beim Bauen in die Firmware |
| `lib/reSID/` | reSID 0.16 von Dag Lem in der MCU-Fassung aus pfm2sid, **GPL-2.0-or-later** |

**Serielle Konsole** (115200 Baud) versteht dieselben Befehle wie der Browser, zum
Prüfen ohne WLAN: `on 57 100`, `off 57`, `set f.cut 400`, `load 2`, `save 31`, `stat`,
`peak`, `dump`, `schema`, `state`, `miditrace 1`. Alle fünf Sekunden kommt eine
Statuszeile. Nur an der Konsole: `usb` gibt Geschwindigkeit und rohe Deskriptoren des
Geräts an der nativen USB-Buchse aus; `backup` gibt alle Presets aus, `restore <bytes>`
nimmt danach so viele Bytes Sicherungsdatei an. `tools/synth_console.py` schickt Befehle,
ohne das Board neu zu starten; `tools/synth_presets.py` sichert und spielt Presets über
USB zurück, ohne WLAN; `tools/synth_check.py` ist ein Selbsttest über die Konsole.

**Lizenz:** reSID steht unter der GPL. Wer die Synth-Firmware weitergibt, muss deshalb
auch den Quelltext unter der GPL weitergeben. Die Filter-Box ohne Synth ist davon nicht
betroffen — sie bindet reSID nicht ein.

### Verdrahtung

```
PCM5102A-Modul          ESP32-S3
  VIN ─────────────────── 5V
  GND ─────────────────── GND
  BCK ─────────────────── GPIO 12
  LCK ─────────────────── GPIO 13
  DIN ─────────────────── GPIO 14
  SCK ─────────────────── GND      (den Takt erzeugt der Chip selbst aus BCK)
```

Auf der Rückseite der üblichen lila Module stehen vier Lötbrücken; richtig ist
1 = L, 2 = L, 3 = H, 4 = L. Brücke 3 (XSMT) auf L schaltet den Ausgang stumm — die
häufigste Ursache für „kommt nichts raus". Der Ausgang ist Line-Pegel.

```
Encoder-Board (STEMMA QT)   ESP32-S3
  rot   (3V3) ─────────────── 3V3   (nicht 5 V)
  schwarz (GND) ───────────── GND   (rechte Leiste; das GND links unten braucht der DAC)
  blau  (SDA) ─────────────── GPIO 8
  gelb  (SCL) ─────────────── GPIO 9
```

Das DevKitC hat keine STEMMA-QT-Buchse, nur Stifte. Dazwischen gehört ein Kabel
STEMMA QT auf Dupont-Buchse (Adafruit 4397). Welche der beiden weißen Buchsen am
Encoder-Board es nimmt, ist egal; die andere bleibt für ein OLED frei. Das Board
verträgt auch 5 V, zieht SDA und SCL dann aber auf 5 V hoch, und das halten die Pins des
ESP32 nicht aus.

**Encoder-Board:** der Adafruit I2C Quad Rotary Encoder („NeoRotary 4", Produkt 5752)
aus dem Chromakey-Controller, angesteuert genau wie dort. Regler 1–3 verstellen je einen
Parameter — voreingestellt Cutoff, Resonanz und das Tempo von LFO 1, im Editor unter
„Einstellungen" frei zuzuweisen. Rund 128 Rasten für den ganzen Bereich, schnell gedreht
bis zu viermal so weit. Ein Druck holt den gespeicherten Wert zurück. Regler 4 blättert
durch die Presets, ein Druck verwirft alle Änderungen. Die LEDs leuchten umso heller, je
höher der Wert steht; die vierte wird orange, solange etwas ungespeichert ist. Das Board
darf auch im Betrieb eingesteckt werden, die Firmware sucht alle zwei Sekunden danach.
An der Konsole spielen `turn <1..4> <rasten>` und `press <1..4>` dasselbe ohne Board.

## Noch offen

- ~~Verdrahtung am echten Board prüfen~~ — am 2026-08-25 über den USB-Host-Weg erledigt:
  alle sechs Saitenkanäle kommen an, gehen auf Kanal 1 hinaus, Legato-Reihenfolge stimmt.
  Die DIN-Verdrahtung mit Optokoppler ist dagegen weiterhin ungeprüft.
- Die Anschlagschwelle nochmal sauber ausmessen. Am 2026-08-20 hiess es 48 bis 96, am
  2026-08-25 lagen Anschläge zwischen 23 und 124 — mit der alten Schwelle 32 fielen leise
  Anschläge stumm heraus. 20 liegt in der gemessenen Lücke zwischen 16 und 23, ist aber
  aus einer einzigen Sitzung abgeleitet.
- Optional: stromloser Anlauf — die SidStation hängt nach einem Neustart der Box nie in
  einer Note, weil `setup()` ein All Notes Off schickt; das gilt aber nicht umgekehrt,
  wenn die Box mitten im Spiel abstürzt
