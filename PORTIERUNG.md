# SID-Synth auf macOS und iPadOS — Notizen für eine mögliche Portierung

**Stand: 13.10.2026, nur Planung, nichts davon gebaut.** Ziel: Klänge am Mac oder iPad
bauen, dort genauso hören wie aus der Box und dann 1:1 auf die Box übertragen.

Grundlage ist der Stand auf Branch `sid-synth`. Wie die Box aufgebaut ist, steht in
[Firmware/README.md](Firmware/README.md), Abschnitt „Der eingebaute SID-Synth“.

---

## Was „1:1“ heißt

**Das Preset selbst ist verlustfrei übertragbar.** Ein Preset besteht aus einem Namen und
je Parameter einem ganzzahligen Rohwert, gespeichert unter dem Schlüssel aus
`Firmware/src/synth/params.h`:

```json
{"fmt":1,"name":"Sägezahn-Lead","v":{"o1.wave":2,"o1.semi":0,"f.cut":900,"me.d":70}}
```

Die Box rechnet beim Laden nichts um. Ein Wert, der in der App 900 ist, ist auf der Box
900. Fehlende Schlüssel bekommen die Voreinstellung, unbekannte werden übergangen.

**Der Klang ist gleich, wenn dieselbe Rechnung läuft.** Dazu muss die App diese Dinge genau
so machen wie die Box:

| | Box |
|---|---|
| Emulation | reSID 0.16, `SAMPLE_FAST`, PAL-Takt 985 248 Hz |
| Abtastrate | 44 100 Hz, mono |
| Modulation | alle 128 Samples (≈ 344,5-mal pro Sekunde): LFOs, Hüllkurve, Glide, dann die SID-Register |
| Formeln | `engine.cpp` (Tonhöhe, Cutoff, Pulsbreite) und `params.cpp` (`curve()`) |
| Pegel | Lautstärke-Register `p.vol`, danach kein weiterer Verstärker |

Bitgleich wird es trotzdem nicht ganz. `expf`, `exp2f` und `powf` liefern auf dem ESP32
und auf Apple-Geräten in der letzten Stelle manchmal etwas anderes. Ein Register kann
dann um 1 abweichen. Hören kann man das nicht. Wer es prüfen will, schreibt auf beiden
Seiten die Registerwerte je Block mit und vergleicht sie.

**Nicht gleich ist der analoge Teil.** Bei der Box kommt der Ton aus dem PCM5102 als
Line-Pegel, beim Mac aus dessen eigenem Ausgang. Zum Vergleichen beide über dasselbe Pult
abhören, zum Beispiel die AG06 MK2.

---

## Was sich übernehmen lässt

| Teil | Übertragbar? | Bemerkung |
|---|---|---|
| `lib/reSID/` | ja, unverändert | reines C++; lokal nur der Schalter `RESID_ENABLE_INTERPOLATE`. **GPL**, siehe unten |
| `synth/params.h/.cpp` | ja, unverändert | nur `<math.h>`, `<string.h>`. Die App muss **dieselbe Datei** übersetzen, nicht abschreiben — sonst laufen die Schlüssel auseinander |
| Werksklänge in `presets.cpp` | ja | `factory()` herauslösen; JSON-Teil hängt an ArduinoJson (läuft auch am Rechner) und LittleFS (ersetzen) |
| `synth/engine.cpp` | **zur Hälfte** | Klangrechnung portabel; FreeRTOS-Warteschlange, I2S, `ESP.getCycleCount()` und Arduino nicht |
| `midi_filter`, `midi_event` | ja | schon heute am Rechner getestet (`make firmware-test`); Swift-Fassung in `Packages/MidiFilterCore` |
| `synth_main.cpp` | nein | Befehle, Regler, Preferences, Konsole — die kleine Logik darin in der App neu schreiben |
| `web/index.html` | ja, als Ganzes | baut sich aus `/api/schema` selbst auf; siehe „Editor“ unten |

### Der eine Umbau vorher: die Klangrechnung aus `engine.cpp` herauslösen

Heute steckt in `audio_task()` beides: die Rechnung und die ESP32-Hülle darum. Für die
Portierung wird das getrennt:

- **`engine_core.h/.cpp`, ohne jede Plattform:** Zustand (Stimme, Hüllkurve, LFOs,
  Schattenregister, reSID), `apply(command)` und `render_block(int16_t out[128])`, also
  Modulation, Register und `g_sid.clock()`. Dazu Ein- und Ausblenden und Pegelmessung.
- **`engine.cpp` auf der Box:** Warteschlange, I2S, Lastmessung, Aufgabe auf Kern 1 —
  ruft nur noch `render_block()` auf.
- **Die App:** eigene Warteschlange (lock-free, ein Schreiber, ein Leser) und der
  Audio-Callback, der ebenfalls `render_block()` aufruft.

Das zuerst **in der Firmware** machen und auf dem Board prüfen, dass sich nichts ändert:
`tools/synth_check.py` muss dieselben Pegel zeigen (heute 20–47 %), die Last bleibt bei
14–16 %. Erst dann gibt es den gemeinsamen Kern, den beide Seiten übersetzen.

---

## Aufbau der App (Vorschlag)

Das vorhandene Xcode-Projekt (`project.yml`, XcodeGen, macOS 14 / iOS 17, ein Target
für beide Systeme) hat schon CoreMIDI (`Sources/MIDI/`) und Hintergrund-Audio für iOS.
Daneben ein zweites Target oder eine zweite App:

- **Swift Package `SIDEngine`** mit C++-Target: `reSID`, `engine_core`, `params`,
  Werksklänge. SwiftPM kann C++-Targets; angesprochen über Swift-C++-Interop oder eine
  kleine C-Schnittstelle (`sid_note_on`, `sid_set_param`, `sid_render`).
- **Audio:** `AVAudioEngine` mit einem `AVAudioSourceNode` im Format **44,1 kHz mono**.
  Den Rest auf die Hardware-Rate (meist 48 kHz) rechnet AVAudioEngine selbst um. Der
  Callback bekommt wechselnde Längen (z. B. 512 oder 471 Frames): intern immer ganze
  128er-Blöcke rechnen und einen Rest bis zum nächsten Aufruf aufheben. Sonst stimmt
  die Modulationsrate nicht mehr mit der Box überein.
- **Echtzeitregeln im Callback:** kein Lock, keine Speicheranforderung, kein Swift-Objekt.
  Noten und Parameter kommen wie auf der Box über eine Warteschlange.
- **MIDI:** CoreMIDI, die DG-20 über den C2MIDI Pro direkt an Mac oder iPad (USB-C). Der
  EZ-AG-Filter aus `MidiFilterCore` lässt sich wie auf der Box vorschalten.
- **Später, wenn gewünscht:** dieselbe Engine als AUv3-Plug-in, dann läuft sie auch in
  GarageBand, Logic oder AUM. Deutlich mehr Arbeit, nicht für den ersten Schritt.

### Editor

Zwei Wege:

1. **Den Web-Editor wiederverwenden:** `index.html` in einer `WKWebView`. Statt des
   WebSockets bekommt sie eine kleine Brücke nach Swift: dieselben Textbefehle
   (`set f.cut 900`, `load 2`, …) und dasselbe Schema. Am schnellsten, und Mac, iPad und
   Box sehen gleich aus. Den Code dafür einmal so umbauen, dass er WebSocket und Brücke
   gleichermaßen bedienen kann.
2. **Eigener SwiftUI-Editor**, gebaut aus derselben Parametertabelle. Schöner auf dem
   iPad, aber jede Änderung am Editor gibt es dann zweimal.

Empfehlung: mit 1 anfangen.

---

## Übertragen zwischen App und Box

**Heute gibt es nur die Richtung Box → Rechner:** `GET http://sid.local/api/backup` liefert
alle belegten Plätze in einem Dokument:

```json
{"fmt":1,"presets":[{"slot":1,"fmt":1,"name":"Sägezahn-Lead","v":{…}}, …]}
```

**Für die Richtung Rechner → Box gibt es drei Wege:**

1. **Neuer Endpunkt `POST /api/restore`** auf der Box, der genau dieses Dokument annimmt
   und die Plätze schreibt. Dazu im Editor eine Schaltfläche „Presets laden“. Das ist der
   Weg, den ich empfehle: ein Format für beide Richtungen, und es geht auch ohne App
   vom Browser aus. Jeder Schreibvorgang braucht `while_muted` (66–99 ms je Platz,
   NOTES.md, Diagnose-Falle 1). 32 Plätze bedeuten also gut drei Sekunden Stille — vorher ansagen.
2. **Über die vorhandenen Befehle per WebSocket:** `name <text>`, dann `set <schlüssel>
   <wert>` für jeden Parameter, dann `save <platz>`. Das geht ohne neue Firmware. Der Mac
   oder das iPad muss dafür im WLAN „SID-Synth“ sein. Der Firmen-PC kommt da nicht
   hinein, ein privater Mac oder das iPad schon.
3. **Über die serielle Konsole**, mit denselben Befehlen. Mac mit USB-Kabel am Board, ohne
   WLAN.

**Fallen dabei:**

- **Platznummern:** Befehle zählen ab 0 (`load 0` … `load 31`), das Backup und die Anzeige
  ab 1 (`"slot":1`). Beim Umsetzen eins abziehen.
- **Name:** höchstens 23 Byte UTF-8. Die Box kürzt längere Namen und schneidet dabei kein
  Zeichen durch (`set_name()`). Die App sollte genauso kürzen, sonst heißt das Preset auf
  der Box anders.
- **Wertebereiche:** Die Box begrenzt jeden Wert auf min/max aus `params.h`. Die App muss
  dieselben Grenzen nehmen, sonst klingt ein Wert außerhalb dort anders.
- **Neue Parameter:** immer als neue Zeile in `params.h`, nie Schlüssel umbenennen oder
  Bedeutungen ändern. Wenn es doch einmal nötig ist, `fmt` hochzählen und beim Laden
  umrechnen.

---

## Die vier Drehregler

**Auf der Box:** Die Firmware unterstützt alle vier Regler des Encoder-Boards (Adafruit
5752):

| Regler | Drehen | Drücken |
|---|---|---|
| 1–3 | Parameter verstellen (Vorgabe: Cutoff, Resonanz, LFO-1-Tempo; im Editor oder mit `knob <1..3> <schlüssel>` umstellbar, bleibt über Neustart) | Parameter zurück auf den gespeicherten Wert |
| 4 | durch die belegten Presets blättern | alle Änderungen am Preset verwerfen |

Schrittweite: Bereich / 128 je Raste, schnell gedreht bis vierfach. Die LEDs zeigen den
Stand im Wertebereich, Regler 4 leuchtet orange bei ungespeicherten Änderungen. Auf dem
Board **noch nicht geprüft**: Drehrichtung und Gefühl der Schrittweite (NOTES.md,
Schritt 2).

**In der App:** Das Board hängt am I2C und lässt sich nicht direkt an Mac oder iPad
anschließen. Möglich sind diese Wege:

- dieselben vier Rollen als Regler auf dem Bildschirm, mit derselben Schrittlogik
  (`turn_knob()` in `synth_main.cpp`)
- ein beliebiger MIDI-Controller mit Endlosreglern (relative CCs), auf die vier Rollen
  gelegt. Das geht auf Mac und iPad gleich.
- nur am Mac: das Board über eine USB-I2C-Brücke (z. B. Adafruit MCP2221A) mit der
  seesaw-Ansteuerung aus `controls.cpp`. Umständlich, für das iPad gar nicht möglich.

---

## Was man dafür braucht

- **Einen Mac mit Xcode.** Der Windows-Arbeitsrechner reicht nur für den Firmware-Teil.
  Er hat auch keinen C++-Compiler für den Rechner selbst.
- **Signieren fürs iPad:** Mit einer kostenlosen Apple-ID läuft eine selbst gebaute App
  nur 7 Tage und muss dann neu aufgespielt werden. Mit einem bezahlten Entwicklerkonto
  läuft sie ein Jahr.
- **Lizenz:** reSID steht unter der GPL. Die App wird damit auch GPL. Für den eigenen
  Gebrauch ist das kein Problem. Für eine Veröffentlichung im App Store passt die GPL
  schlecht zu dessen Bedingungen.

## Reihenfolge, falls es so weit kommt

1. Firmware: `engine_core` herauslösen, auf dem Board prüfen, dass Pegel und Last gleich
   bleiben.
2. Am Mac ein kleines Kommandozeilenprogramm: Preset-JSON plus Notenfolge rein, WAV raus.
   Daran die Gleichheit mit der Box prüfen: die Box über die AG06 MK2 aufnehmen und
   neben das WAV legen.
3. Firmware: `POST /api/restore` und die Schaltfläche im Editor.
4. App für den Mac: `SIDEngine`-Paket, AVAudioEngine, CoreMIDI, Web-Editor in der
   `WKWebView`.
5. Dieselbe App auf dem iPad (Hintergrund-Audio ist im Projekt schon eingetragen).
6. Wenn gewünscht: AUv3.

Grobe Schätzung, ungeprüft: Schritte 1–3 etwa zwei Tage, die App für Mac und iPad mit
dem Web-Editor noch einmal zwei bis drei Tage.
