# SID-Synth — Projektnotizen

**Stand: 13.10.2026** — Branch `sid-synth`, gepusht. Die
Synth-Firmware läuft auf dem Board, der Editor ist auf dem iPhone getestet, Ton kommt
bisher nur über „Ton im Browser". **Es wartet alles auf Hardware:** das DAC-Modul ist
da und wird am **14.10.** gelötet, Kabel fehlen noch. **Die nächste Sitzung beginnt mit
dem gelöteten DAC** — zuerst Fotos von beiden Seiten des Moduls ansehen (Schritt 1 unten).
Der USB-Audio-Weg ist damit vom Tisch (AG06 MK2, siehe unten).

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
7. **Umzug auf den Tab5**, sobald er da ist — siehe „Tab5" unten.

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
