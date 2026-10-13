"""Presets über die serielle Konsole sichern und zurückspielen — ohne WLAN.

python tools/synth_presets.py COM5 backup sid-presets.json
python tools/synth_presets.py COM5 restore sid-presets.json

Das Dateiformat ist dasselbe wie bei „Alle Presets als Datei sichern" und „Presets aus
Datei laden" im Editor. Beim Zurückspielen schreibt das Board nur Plätze, die sich
ändern. Kein Neustart (DTR/RTS bleiben unten). Braucht pyserial; das Python von
PlatformIO bringt es mit: ~/.platformio/penv/Scripts/python.exe
"""
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8")
port, action, path = sys.argv[1:4]
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.05
s.dtr = False  # kein Neustart
s.rts = False
s.open()


def read_for(seconds):
    end = time.time() + seconds
    out = b""
    while time.time() < end:
        out += s.read(65536)
    return out.decode("utf-8", errors="replace").splitlines()


read_for(0.2)
if action == "backup":
    s.write(b"backup\n")
    found = [line for line in read_for(1.5) if line.startswith('{"fmt"')]
    if not found:
        sys.exit("Keine Sicherung bekommen — läuft die Synth-Firmware?")
    with open(path, "w", encoding="utf-8") as f:
        f.write(found[0])
    print(f"{path}: {len(found[0].encode())} Bytes")
elif action == "restore":
    data = open(path, "rb").read()
    s.write(f"restore {len(data)}\n".encode())
    time.sleep(0.1)
    # Häppchenweise: der Empfangspuffer des Boards fasst nur 256 Bytes.
    for i in range(0, len(data), 128):
        s.write(data[i:i + 128])
        time.sleep(0.015)
    for line in read_for(5.0):
        if line and not line.startswith(("STAT", ">")):
            print(line)
else:
    sys.exit("backup oder restore")
s.close()
