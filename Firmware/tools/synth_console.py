"""Schickt Befehle an die laufende Synth-Firmware, ohne sie neu zu starten.

python tools/synth_console.py COM5 "set p.vol 0" "wait 2.5" "stat" "on 60 100" "wait 1.5" "stat"

`wait <s>` liest nur mit. Ausgegeben wird alles, was das Board dabei schreibt, ohne die
regelmässigen STAT-Zeilen. DTR und RTS bleiben unten — sonst löst die Auto-Reset-
Schaltung des Boards aus. Braucht pyserial; das Python von PlatformIO bringt es mit:
~/.platformio/penv/Scripts/python.exe
"""
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8")
s = serial.Serial()
s.port = sys.argv[1]
s.baudrate = 115200
s.timeout = 0.05
s.dtr = False  # kein Neustart
s.rts = False
s.open()


def read_for(seconds):
    end = time.time() + seconds
    out = b""
    while time.time() < end:
        out += s.read(4096)
    return out.decode("utf-8", errors="replace")


read_for(0.2)
for step in sys.argv[2:]:
    if step.startswith("wait "):
        text = read_for(float(step.split()[1]))
    else:
        s.write((step + "\n").encode())
        text = read_for(0.4)
    for line in text.splitlines():
        if line and not line.startswith("STAT"):
            print(line)
s.close()
