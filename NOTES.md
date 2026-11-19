# SID-Synth — Projektnotizen

**Stand: 19.11.2026** — Branch `sid-guition` (lokal, noch nicht committet). **Neues
Gerät:** der Synth zieht auf ein **Guition JC4827W543** (4,3″-Display) um, weil das
Display der alten Box zu klein war — und dasselbe Gerät ersetzt auf der Bühne das
**iPixel-Bluetooth-Panel** als vierte Flaschen-Taschen-Tafel. Der Tab5 bleibt allein
die Bühnen-Zuspielung (midi-ft-bridge). Siehe Abschnitt „Guition-Gerät“.

Sitzung 19.11. in Kürze:
- Board erkannt (ESP32-S3 N4R8: 4 MB Flash, 8 MB Octal-PSRAM), Werks-Firmware gesichert:
  `D:\Developer\Arduino\SID\backup\guition-jc4827w543-factory-4MB.bin`. Foto der
  Platine mit allen Stecker-Aufdrucken: `D:\Developer\Arduino\SID\snap\expressiv.jpeg`.
- Neue Umgebung `pio run -e guition`, dieselbe Synth-Firmware: Display, Encoder-Board an
  P5, DIN-MIDI statt C2MIDI (USB-C bleibt für Konsole und Flashen), DAC-Pins auf P3.
- **Auf der Hardware geprüft:** Encoder-Board wird gefunden; Engine 17–19 % Last,
  Testnote 39 % Pegel; das Gerät meldet sich am Hotspot des Tab5 an (feste Adresse
  192.168.10.23), der Tab5 spielt „Pixel“, **das Display zeigt den Clip** (32×16, je
  Punkt 15×15 Pixel), ~150 Pakete/s, keins verworfen.
- Tab5 von der Bass-Synth-Firmware zurück auf die Bridge (`tab5-playout`, auf diesem PC
  mit ESP-IDF 5.5.1 gebaut); dort Tafel D in die eingebaute Tafelliste aufgenommen —
  auf der SD-Karte liegt **keine** config.json. NVS des Bass-Synth gesichert:
  `backup\tab5-e8f60ae2e8b7-bass-synth-nvs-2026-11-19.bin` (Patches, Loops, bleiben auch
  im Gerät stehen).
- Noch nicht gesehen/gesagt: ob die Farbprobe beim Start stimmt und ob ein Dreh am
  Encoder auf die SID-Seite umschaltet.

Sitzung 13.10. in Kürze:
- Anschluss des Encoder-Boards geklärt, Foto der Rückseite:
  [docs/encoder-board-rueckseite.jpg](docs/encoder-board-rueckseite.jpg) — zwei gleichwertige STEMMA-QT-Buchsen
  links und rechts, die Lochreihen bleiben leer, die Adress-Lötpunkte nicht anfassen.
  Das DevKitC hat keine STEMMA-QT-Buchse → Kabel Adafruit 4397 (STEMMA QT auf
  Dupont-Buchse, 15 cm). Belegung in Schritt 2 und in der README.
- USB-Audio-Interface statt DAC durchgesprochen, zurückgestellt (siehe unten). Später
  am Tag einen Yamaha AG06 MK2 angesteckt: an der nativen Buchse kam nur ein Hub an.
- Das Board hing zwischendurch an **COM6**, und Windows meldete den CH340 als
  „funktioniert nicht" — einmal ab- und wieder anstecken, Port neu nachsehen. Abends
  war es wieder COM5.
- **Presets hochladen:** im Editor „Presets aus Datei laden“ (`POST /api/restore`), an der
  Konsole `backup` / `restore <bytes>`, dazu `tools/synth_presets.py`. Über die Konsole
  auf dem Board geprüft, der Weg über WLAN noch nicht (siehe „Was fehlt“).
- Fehler behoben: ein Name, der auf einen Umlaut endet, verlor seinen letzten Buchstaben
  („Menü“ → „Men“). Die Kürzung steckt jetzt einmal in `params.cpp` (`set_name`).
- Sicherung der Presets vom 13.10. (8 Plätze, eigene auf 1 und 2):
  [docs/sid-presets-2026-10-13.json](docs/sid-presets-2026-10-13.json) — lässt sich
  mit „Presets aus Datei laden“ wieder einspielen.
- Plan für eine Mac-/iPad-App: [PORTIERUNG.md](PORTIERUNG.md).

Technischer Aufbau, Verdrahtung und Befehle: [Firmware/README.md](Firmware/README.md),
Abschnitt „Der eingebaute SID-Synth". Diese Datei hier ist Stand, Plan und Fallen.

---

## Guition-Gerät (ab 19.11.)

**Guition JC4827W543:** ESP32-S3 (Modul „N4R8“), 4,3″ IPS 480×272, NV3041A über QSPI,
USB-C an der nativen USB-Schnittstelle (USB-Serial-JTAG, am Firmen-PC **COM15**),
MAC 44:bd:8d:7a:6d:e4. Touch vermutlich resistiv (6-polige Touch-Buchse leer), wird
nicht gebraucht.

| Stecker | Aufdruck | Belegung |
|---|---|---|
| P1 | GND RXD TXD +5V | frei — +5 V/GND für den DAC |
| P2 | IO46 IO9 IO14 IO5 | frei (IO46 ist ein Boot-Pin) |
| P3 | IO6 IO7 IO15 IO16 | DAC: BCK 6, LRCK 7, DIN 15 · DIN-MIDI-Eingang an 16 |
| P4 | GND 3.3V IO17 IO18 | dieselben Pins wie P5 — nur GND/3,3 V fürs MIDI-Modul |
| P5 | GND 3.3V IO17 IO18 | STEMMA QT: **Encoder-Board** (SDA 17, SCL 18), steckt |

Intern: Display CS 45, SCK 47, D0–D3 21/48/40/39, Hintergrundlicht 1. Alles in
`Firmware/src/synth/board.h`.

**Display** (`display.cpp`): im Normalbetrieb die vierte Tafel — das 32×16-Bild vom
Tab5, jeder Punkt als Block von 15×15 Pixeln (480×240, oben und unten 16 Pixel
schwarz), die Auflösung des iPixel bleibt. Ein Dreh oder Druck am Encoder zeigt die
**SID-Seite** (Platz, Name, die drei Live-Regler mit Balken, MIDI, Pegel, Netz) und
bleibt 4 s nach dem letzten Dreh. Kommt 2 s kein Bild vom Tab5, bleibt die SID-Seite
stehen. Beim Start 3 s Farbprobe (ROT/GRÜN/BLAU/WEISS).

**Tafel** (`panel.cpp`): UDP 1337, PPM P6 mit „#FT: x y“ (wie der Tab5 sendet) oder
Versatz im Nachspann. Auf dem Tab5 (`midi-ft-bridge`, Branch `tab5-playout`,
`main.c`/`config.c`) steht die Tafel **D_sidpanel 192.168.10.23, Ausschnitt 256,0 32×16**
mit fester DHCP-Adresse für die MAC oben. Die Mac-App-config.json hat noch den
iPixel-Eintrag.

**Netz:** das Gerät meldet sich beim gespeicherten fremden Netz an (`wifi <ssid> <pw>`,
im NVS, auf der Bühne `strato_accesspoint`) und hält das eigene WLAN „SID-Synth“ (Kanal
6 wie der Tab5) für den Editor offen. Auf dem Bühnen-Hotspot ist der Editor unter
http://192.168.10.23 erreichbar.

**MIDI:** DIN über ein Optokoppler-Modul (3,3 V) an IO16, Pull-up an, kein MIDI OUT.
Der Weg über den C2MIDI ist für dieses Gerät gestrichen.

Neue Konsolenbefehle: `screen auto|sid|tafel|farben`, `paneltest <s>` (Testbild wie vom
Tab5), `tafel` (empfangenes Bild als Helligkeitsstufen), `wifi [<ssid> <pw> | -]`,
`reboot`. Die Konsole schreibt das WLAN-Passwort nicht zurück.

## Ziel

Aus der EZ-AG-Filter-Box einen SID-Synthesizer machen: der ESP32-S3 rechnet einen
SID-Chip nach (reSID), gespielt wird mit der **Casio DG-20** über den CME C2MIDI Pro.
Klang im Browser bearbeiten, Presets speichern, drei Live-Regler und ein Preset-Regler.

## Entscheidungen

| Was | Entscheidung |
|---|---|
| Board | das bisherige ESP32-S3-DevKitC-1, N16R8-Nachbau (16 MB Flash, 8 MB Octal-PSRAM, CH340 an COM5) |
| Instrument | Casio DG-20 — MIDI geht ungefiltert in die Engine, EZ-AG-Filter im Editor abschaltbar, Voreinstellung aus |
| Klangerzeugung | reSID 0.16, SAMPLE_FAST, 44,1 kHz, ein SID mit drei Oszillatoren auf einer Note (wie SidStation mono) |
| Audio | Youmile GY-PCM5102, pHAT-Bauform (Amazon B09NXKPZ8N), bestellt; Stiftleiste lose → wird gelötet |
| Regler | Adafruit Quad Rotary Encoder 5752 („NeoRotary 4") aus dem Chromakey-Projekt `D:\Developer\Arduino\ESP32` |
| Aufbau | „Weg A": Board + Module, bis der Tab5 läuft. **M5Stack Tab5 am 15.10. bestellt** (ESP32-P4, USB-A-Host, ES8388 mit Klinke, 5″-Touch, Grove) — das Gerät aus einem Guss, siehe „Tab5" |
| **ab 19.11.** | **Guition JC4827W543** statt Box und Tab5: Synth + Display + vierte Tafel (ersetzt das iPixel-Panel) in einem Gerät. DIN-MIDI statt C2MIDI, derselbe PCM5102 an P3, Encoder-Board an P5. Der Tab5 bleibt die Bühnen-Zuspielung. Siehe „Guition-Gerät" |

---

## Was funktioniert (auf dem Board geprüft)

- Alle 8 Werksklänge klingen, Pegel 20–47 %, keine Übersteuerung (`tools/synth_check.py`)
- CPU-Last der Klangerzeugung 14–16 % eines Kerns, Spitze um 30 %
- Speichern 66–99 ms, ausgeblendet; Löschen; Presets überleben das Neu-Flashen
- Editor auf dem iPhone: Seite, Regler, Presets, Speichern, Ton im Browser
- WebSocket-Rundlauf Median 6–8 ms, bis 52 Reglerbefehle/s ohne Wirkung auf den Klang
- Regler-Logik über die Konsole (`turn`/`press`): Schrittweite, Zurückholen, Preset-Blättern,
  Belegung bleibt über Neustart
- Filter-Firmware (`pio run`) baut nach dem Umbau weiterhin
- Presets zurückspielen über die Konsole (`restore`, `tools/synth_presets.py`): nur
  geänderte Plätze werden geschrieben (dieselbe Sicherung → „0 geladen, 8 unverändert“),
  3 Plätze in 427 ms; Werte außerhalb des Bereichs begrenzt, unbekannte Schlüssel
  übergangen, Platz doppelt → der letzte gilt; Editor folgt seinem Platz, ungespeicherte
  Änderungen bleiben; kaputte Datei, `fmt` 2, leere Liste, zu groß → Meldung, nichts
  geschrieben. JavaScript des Editors mit `node --check` geprüft.

## Was fehlt — nächste Schritte, in dieser Reihenfolge

**Guition-Gerät (gilt ab 19.11. vor der alten Liste darunter):**

G1. **Ansehen und sagen:** stimmt die Farbprobe beim Start (ROT/GRÜN/BLAU/WEISS)? Schaltet
    ein Dreh am Encoder auf die SID-Seite und nach 4 s zurück zur Tafel? Ist die SID-Seite
    lesbar (Umlaute, Werte, Balken)? Drehrichtung der Encoder.
G2. **DAC an P3:** BCK → IO6, LRCK → IO7, DIN → IO15, VIN/GND von P1 (+5V/GND).
    Lötbrücken wie bisher (FLT = L, DEMP = L, XSMT = H, FMT = L). Gebraucht: zwei
    4-polige Kabel mit Stecker passend zu P1/P3 auf Dupont-Buchse.
G3. **DIN-MIDI-Modul** (Optokoppler, 3,3 V tauglich) an IO16 (P3), 3,3 V/GND von P4.
    Dann DG-20 mit `miditrace 1` (Kanal, Program Change, mehrstimmig?).
G4. **Ohne Tab5 mit gespeichertem Bühnen-WLAN:** stört das ständige Suchen nach
    `strato_accesspoint` das eigene WLAN „SID-Synth“ (Editor am iPhone)? Wenn ja: seltener
    suchen oder nur auf Kanal 6.
G5. Bühnen-Durchlauf: alle drei Tafeln + Tafel D, Fantom-Pads, eine Stunde; dabei
    `stat` auf beiden Geräten (Lücken `gap`, verlorene Kacheln auf dem Tab5).
G6. Tafel D auch in die config.json der Mac-App (statt des iPixel-Eintrags), falls die
    Mac-App ohne Tab5 spielen soll — dann muss das Gerät in deren Netz.
G7. Commits: `sid-guition` hier, `tab5-playout` (main.c, config.c) in midi-ft-bridge.

**Alte Liste (DevKitC-Box), Punkte 1–3 gehen in G2/G3 auf, Punkt 7 entfällt:**

0. **Hochladen im Browser prüfen** (geht jederzeit, ohne neue Hardware), am iPhone im
   WLAN „SID-Synth“: Einstellungen → „Alle Presets als Datei sichern“, dann „Presets aus
   Datei laden“ mit genau dieser Datei → erwartet „0 Presets geladen, 8 unverändert“.
   Danach einen Klang ändern, auf einem freien Platz speichern, sichern, Platz löschen,
   Datei wieder laden → der Platz ist zurück. Der Firmen-PC kommt nicht ins WLAN, deshalb
   ist dieser Weg (`on_restore_body` in `web.cpp`) noch ungeprüft.

1. **DAC anschließen.** Foto von beiden Seiten des gelöteten Moduls ansehen, dann:
   - Lötbrücken, falls vorhanden: **FLT = L, DEMP = L, XSMT = H, FMT = L** (XSMT auf L = stumm)
   - Pi-Pins **2 → 5V, 6 → GND, 12 → GPIO 12, 35 → GPIO 13, 40 → GPIO 14**
   - Am Verstärker über die Tastatur im Editor spielen; erwartet ~15 ms Latenz
2. **Encoder-Board** per STEMMA-QT-auf-Dupont-Kabel (Adafruit 4397), egal an welche der
   beiden weißen Buchsen: rot → 3V3 (links oben), schwarz → GND der **rechten** Leiste
   (links gibt es nur ein GND, das braucht der DAC), blau → GPIO 8, gelb → GPIO 9.
   Im Log muss `ENC   Encoder-Board gefunden` stehen.
   **Ungeprüft:** Drehrichtung — `kEncoderSign = -1` ist aus dem Chromakey-Projekt
   übernommen; Schrittweite (~128 Rasten für den ganzen Bereich) am Gefühl prüfen.
3. **DG-20 über den C2MIDI**, mit `miditrace 1` auf der Konsole. Klären:
   Kanal, Velocity, Pitchbend, ob sie Program Change schickt (**wechselt dann das
   Preset!** — gegebenenfalls abschaltbar machen), ob sie mehrstimmig sendet (die
   Engine spielt einstimmig, die zuletzt angeschlagene Note).
4. **OLED**, falls bestellt (Adafruit 938, 1,3″ 128×64, STEMMA QT, Adresse 0x3C) —
   wird an der zweiten Buchse des Encoder-Boards durchgesteckt. Anzeige schreiben:
   Platz, Presetname, die drei Reglerwerte.
5. **Umbau der Klangerzeugung prüfen** (13.10. abends für die Mac-App gemacht, siehe
   PORTIERUNG.md): nach dem Flashen `tools/synth_check.py` — Pegel müssen bei 20–47 %
   bleiben, Last bei 14–16 %. Dann einmal aus der Mac-App einen Klang auf die Box
   übertragen (Menü „Box“).
6. Danach: Pull Request `sid-synth` → `main`.
7. ~~**Umzug auf den Tab5**~~ — entfällt (19.11.): der Tab5 ist die Bühnen-Zuspielung,
   der Synth zieht auf das Guition-Gerät.

Ideen für später: dreistimmiger Modus (eine SID-Stimme je Note), zweiter SID für
Stereo, 96 kHz gegen Aliasing in hohen Lagen (Last 26 %). App für macOS und
iPadOS, um Presets dort zu bauen und 1:1 auf die Box zu übertragen: geplant in
[PORTIERUNG.md](PORTIERUNG.md).

**USB-Audio-Interface statt DAC (13.10., zurückgestellt — mehr dagegen als dafür):**
Test mit dem vorhandenen Steinberg UR22mkII (Audio + MIDI-DIN) wäre möglich, unterbleibt
vorerst, weil er dafür über einen Hub umgesteckt werden müsste. Für den Versuch: UR22 an
die native USB-Buchse des ESP32 statt des C2MIDI, eigene Versorgung über seine
„5V DC"-Buchse. Erst Deskriptoren auslesen: läuft er im Full-Speed-Modus des S3
(12 Mbit/s), UAC1 oder UAC2? Die MIDI-Schnittstelle könnte die jetzige Firmware schon
beanspruchen. Für Audio bräuchte es Arduino 3 / IDF 5 und eine Engine, deren Takt der
USB vorgibt statt I2S — mehrere Tage Arbeit.

Erster Blick am 13.10. mit einem **Yamaha AG06 MK2** (laut Yamaha UAC2) an der nativen
Buchse, Konsolenbefehl `usb` (neu): es meldete sich **nur ein Hub**, VID 0424 / PID 2422
(Microchip USB2422, zwei Ports, „self-powered"), im Full-Speed-Modus. Steckt der AG06
direkt am Board, sitzt der Hub in ihm selbst — noch offen. Das Audiogerät dahinter sieht
der S3 nicht: Arduino 2.0.17 / IDF 4.4 kennt keine Hubs, erst neuere IDF-Versionen mit
`CONFIG_USB_HOST_HUBS_SUPPORTED` — und auch dann nur Geräte mit derselben
Geschwindigkeit wie der Hub, hier also Full Speed.

## Tab5 (bestellt am 15.10.)

Gesucht war ein fertiges Board mit Klinke und Display. Ein ESP32-S3-Board mit beidem gibt
es nicht — die S3-Boards mit Display haben nur einen Lautsprecheranschluss. Verworfen:
CoreS3 + Module Audio (nur eine USB-Buchse → keine Konsole, solange der C2MIDI steckt),
Cardputer-Adv (kein PSRAM). Der Tab5: ESP32-P4, 16 MB Flash, 32 MB PSRAM, WLAN über einen
ESP32-C6, 5″-Touch 1280×720, ES8388 mit 3,5-mm-Klinke, USB-A-Host, USB-C, Grove.
Empfohlen war der K145 (mit NP-F550-Akku), bei Mouser um 52 € netto.

Ein Händler führte ihn als „EOL" („wird nicht mehr hergestellt … solange der Bestand
reicht"). Die Produktseite bei M5Stack hat am 15.10. keinen solchen Vermerk, beide
Varianten auf Lager (55 $ / 60 $ mit Akku); am 28.04.2026 wurde noch der Display-Treiber
getauscht, das Datenblatt ist vom 14.10.2026. Händlerbestand kann eine ältere
Display-Variante sein (bei DigiKey Datecode 2509). Anfrage
beim Hersteller läuft (EOL? welche Hardware-Version? Versand?) — Antwort hier nachtragen.

Aus der [Doku](https://docs.m5stack.com/en/core/Tab5) und dem Blockschaltbild dort:
- **USB:** USB-C hängt an `USB1_D+/D-`, USB-A an `USB2_OTG_D+/D-` — getrennte Leitungen,
  also C2MIDI an USB-A und gleichzeitig Flashen/Konsole über USB-C (M5Stack setzt
  `-DARDUINO_USB_MODE=1`, Konsole über USB-Serial-JTAG).
- **5 V an USB-A** (`SYS_USB5V`) kommen aus der 5-V-Schiene hinter der Stromweiche, die
  aus USB-C gespeist wird — geht auch ohne Akku. Muss per `USB5V_EN` (IO-Expander)
  eingeschaltet werden. Ordentliches USB-C-Netzteil nehmen, Ladeteil, Display und C2MIDI
  hängen alle daran.
- **Audio:** Klinke direkt am ES8388 (I2C 0x10; I2S MCLK G30, BCLK G27, DOUT G26,
  LRCK G29; I2C SCL G32, SDA G31). Lautsprecher-Verstärker NS4150 getrennt dahinter —
  per `SPK_EN` abschalten, sonst spielt er mit. Kopfhörer-Erkennung `HP_DET`.
- **Grove** hängt an zwei freien GPIOs (nicht am internen I2C) und liefert **5 V**.
  Vor dem Anstecken des Encoder-Boards prüfen, ob es SDA/SCL dann auf 5 V hochzieht —
  der P4 verträgt 3,3 V.
- **PlatformIO** laut M5Stack: `platform = https://github.com/pioarduino/platform-espressif32.git#54.03.21`,
  `board = esp32-p4-evboard`, `board_build.mcu = esp32p4`, `-DBOARD_HAS_PSRAM`,
  M5Unified ≥ 0.2.23, M5GFX ≥ 0.2.30.

Was der Umzug bedeutet (mehrere Tage, nichts davon geprüft):
- Arduino-Kern 3.x / IDF 5 statt des festgenagelten 2.0.17 — der USB-Host-Teil ist nur
  gegen 2.0.17 geprüft
- WLAN über den C6 (ESP-Hosted)
- ES8388 über I2C einstellen (der PCM5102 brauchte das nicht)
- Anzeige für das 5″-Display
- reSID-Last auf dem P4 neu messen
- **Falle:** drei Display-Varianten (Mai 2025, Okt. 2025, Apr. 2026) — nur aktuelle
  M5Stack-Bibliotheken erkennen alle; ESPHome kann nur die erste

Der Tab5 ist auch Versuchsgerät für das zweite Projekt **midi-ft-bridge** (Fantom → Videoclips
auf LED-Panels, heute eine Mac-App): Plan und Testreihenfolge dort in `WISSEN/ESP32-PORT.md`.
Wenn das Board da ist, beginnen die Tests dort — vorher klären, ob zuerst der SID-Umzug oder
die Bridge.

## Noch zu besorgen

- Dupont-Kabel **Buchse–Buchse** — Board und DAC haben beide Stifte; die beim DAC
  beiliegenden Kabel sind Buchse–Stecker und passen dann nicht
- STEMMA-QT-Kabel auf Dupont-Buchse (z. B. Adafruit 4397) für das Encoder-Board
- Klinkenkabel 3,5 mm auf den Eingang von Mischpult oder Verstärker (Line, nicht
  Instrumenteneingang)
- optional OLED Adafruit 938 + STEMMA-QT-Kabel Stecker–Stecker (z. B. Adafruit 4210)
- für den Tab5: Kabel Grove auf STEMMA QT (Encoder-Board), USB-C-Netzteil, und falls der
  C2MIDI einen USB-C-Stecker hat, einen Adapter USB-C-Buchse auf USB-A-Stecker

---

## Alltag

```sh
cd Firmware
pio run -e synth -t upload --upload-port COM5     # Synth flashen, Presets bleiben
pio run -t upload --upload-port COM5              # zurück zur reinen Filter-Box
~/.platformio/penv/Scripts/python.exe tools/synth_check.py COM5      # Selbsttest
~/.platformio/penv/Scripts/python.exe tools/synth_console.py COM5 "stat" "miditrace 1"
```

PlatformIO liegt unter `~/.platformio/penv/Scripts/pio.exe` und ist nicht im PATH.
Der COM-Port wechselt je nach USB-Buchse (13.10. war es COM6) — nachsehen mit
`~/.platformio/penv/Scripts/python.exe -m serial.tools.list_ports`.

Guition-Gerät (USB-C, am Firmen-PC COM15; Tab5 COM13):

```sh
cd Firmware
pio run -e guition -t upload --upload-port COM15   # Presets und WLAN-Daten bleiben
# Tab5 (Bridge), PowerShell:
#   . C:\Users\rpreini1\esp\esp-idf-v5.5.1\export.ps1
#   cd D:\Developer\C++\midi-ft-bridge\midi-ft-bridge\tab5\firmware; idf.py -p COM13 build flash
# Tab5-Konsole: play Pixel / stop  (Intro hat im Bereich der Tafel D nichts)
```

- WLAN **SID-Synth**, Passwort `sid12345`, Editor unter **http://sid.local** oder
  http://192.168.4.1
- Vollsicherung der alten Filter-Firmware: `D:\Developer\Arduino\SID\backup\
  ezag-filter-2026-10-12-full16MB.bin` (zurückspielen mit esptool `write_flash 0 …`)

---

## Diagnose-Fallen

1. **Flash-Schreiben hält beide Kerne 23–37 ms an** — länger als der I2S-Puffer
   (4 × 128 Samples = 11,6 ms). Deshalb wird vor jedem Schreiben ausgeblendet
   (`while_muted` in `synth_main.cpp`), auch für NVS-Einstellungen. Neue Schreibzugriffe
   immer dort hindurch.
2. **Der I2S-Unterlaufzähler sieht diese Aussetzer nicht** — die Interrupt-Routine steht
   während des Flash-Zugriffs ebenfalls. Maßgeblich ist die gemessene Lücke zwischen
   zwei Blöcken (`gap` in der Statuszeile).
3. **iPhone stumm im Browser:** der Lautlos-Schalter schaltet Web-Audio ab. Die Seite
   setzt `navigator.audioSession.type = 'playback'`, trotzdem zuerst den Schalter prüfen.
4. **`peak` in der Statuszeile ist ein Sekundenfenster** — kurze Klänge (Pluck) fallen
   leicht dazwischen. Zum Messen den Konsolenbefehl `peak` nehmen: Spitze seit dem
   letzten Abruf.
5. **Lautstärke ändern knackt beim 6581** — das ist echtes Chipverhalten (DC-Sprung),
   kein Fehler. Pegel nach so einer Änderung erst nach ein, zwei Sekunden messen.
6. **Serielle Werkzeuge starten das Board neu**, wenn sie RTS setzen (Auto-Reset).
   `synth_console.py` lässt DTR/RTS unten, `synth_check.py` startet absichtlich neu.
7. **COM5 belegt** → Upload scheitert. Erst jeden laufenden Mitschnitt beenden.
8. **Der Firmen-PC kommt nicht ins SID-WLAN** (Internet über USB-LAN, WLAN-Adapter frei,
   aber nicht verbunden) — Tests im Browser übers iPhone.
9. **`"usb":true` heißt nicht „MIDI-Gerät da"** — findet `USBConnection` keine
   MIDI-Streaming-Schnittstelle, liest es auf gut Glück Endpunkt 0x81 und meldet sich
   trotzdem bereit. So stand es auch bei einem Hub. Was wirklich dran ist, zeigt `usb`.
10. **`LittleFS.exists()` schreibt für jede fehlende Datei einen Fehler** auf die Konsole;
   deshalb liest `presets::begin()` einmal das Verzeichnis.
11. **Guition: GPIO 48 ist eine Datenleitung des Displays** — die Board-Definition
   `esp32-s3-devkitc-1` legt dort die Aktivitäts-LED hin. `SYNTH_HAS_ACTIVITY_LED` ist
   für das Guition aus; nie `neopixelWrite(LED_BUILTIN, …)` aufrufen.
12. **Display-Bibliothek 1.4.9, nicht neuer:** Arduino_GFX ab 1.5/1.6 bindet
   `esp32-hal-periman.h` ein, das es erst im Arduino-Kern 3.x gibt — mit dem
   festgenagelten 2.0.17 bricht der Bau ab.
13. **Konsole des Guition neu starten (Windows):** RTS allein löst über USB-Serial-JTAG
   keinen Reset aus, erst mit einer DTR-Änderung dahinter (so macht es esptool).
14. **`Serial1.begin()` mit zwei Pins −1 nimmt die Standard-Pins** (TX1 = 17 = SDA des
   Encoder-Boards). Für DIN-MIDI wird RX 16 gesetzt, TX −1 — dann bleibt 17 frei.
15. **Tafel D bleibt schwarz?** Der Clip hat dort vielleicht nichts (Intro). `tafel` auf
   der Konsole zeigt das empfangene Bild als Zeichen, `stat` zählt die Pakete (`tafel`)
   und das Alter des letzten (`tafel_alt`).

## Messwerte

| | |
|---|---|
| reSID FAST, 44,1 kHz | 12,8 % eines Kerns |
| reSID FAST, 96 kHz / zwei SIDs | 26 % / 25 % |
| reSID INTERPOLATE | 156 % — nicht machbar |
| Synth gesamt | 14–16 %, Spitze ~30 % |
| Freier Heap | 170–187 KB |
| Firmware | 1,18 MB von 6,25 MB |
| Speichern eines Presets | 66–99 ms |
| WebSocket-Rundlauf | Median 6–8 ms, max ~31 ms |
