# SID-Synth — Projektnotizen

**Stand: 12.10.2026** — Branch `sid-synth`, gepusht. Die Synth-Firmware läuft auf dem
Board, der Editor ist auf dem iPhone getestet, Ton kommt bisher nur über „Ton im
Browser". **Es wartet alles auf Hardware:** das DAC-Modul ist bestellt und wird gelötet,
Kabel fehlen noch.

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
| Aufbau | „Weg A": Board + Module. M5Stack Tab5 (ESP32-P4, USB-A-Host, ES8388 mit Klinke, 5″-Touch, Grove) bleibt Option für ein späteres Gerät aus einem Guss |

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

## Was fehlt — nächste Schritte, in dieser Reihenfolge

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
5. Danach: Pull Request `sid-synth` → `main`.

Ideen für später: dreistimmiger Modus (eine SID-Stimme je Note), zweiter SID für
Stereo, 96 kHz gegen Aliasing in hohen Lagen (Last 26 %), Tab5-Port.

**USB-Audio-Interface statt DAC (13.10., zurückgestellt — mehr dagegen als dafür):**
Test mit dem vorhandenen Steinberg UR22mkII (Audio + MIDI-DIN) wäre möglich, unterbleibt
vorerst, weil er dafür über einen Hub umgesteckt werden müsste. Für den Versuch: UR22 an
die native USB-Buchse des ESP32 statt des C2MIDI, eigene Versorgung über seine
„5V DC"-Buchse. Erst Deskriptoren auslesen: läuft er im Full-Speed-Modus des S3
(12 Mbit/s), UAC1 oder UAC2? Die MIDI-Schnittstelle könnte die jetzige Firmware schon
beanspruchen. Für Audio bräuchte es Arduino 3 / IDF 5 und eine Engine, deren Takt der
USB vorgibt statt I2S — mehrere Tage Arbeit.

## Noch zu besorgen

- Dupont-Kabel **Buchse–Buchse** — Board und DAC haben beide Stifte; die beim DAC
  beiliegenden Kabel sind Buchse–Stecker und passen dann nicht
- STEMMA-QT-Kabel auf Dupont-Buchse (z. B. Adafruit 4397) für das Encoder-Board
- Klinkenkabel 3,5 mm auf den Eingang von Mischpult oder Verstärker (Line, nicht
  Instrumenteneingang)
- optional OLED Adafruit 938 + STEMMA-QT-Kabel Stecker–Stecker (z. B. Adafruit 4210)

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
9. **`LittleFS.exists()` schreibt für jede fehlende Datei einen Fehler** auf die Konsole;
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
