"""Prüft die Synth-Firmware über die serielle Konsole, ganz ohne WLAN.

python tools/synth_check.py COM5

Startet das Board neu (RTS), liest Schema und Zustand, spielt jeden Werksklang an und
misst den Pegel, speichert auf Platz 32 und löscht ihn wieder. Braucht pyserial; das
Python von PlatformIO bringt es mit: ~/.platformio/penv/Scripts/python.exe
"""
import json
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8")
port = sys.argv[1]
s = serial.Serial(port, 115200, timeout=0.05)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False


def read_for(seconds):
    end = time.time() + seconds
    out = b""
    while time.time() < end:
        out += s.read(4096)
    return out.decode("utf-8", errors="replace")


def send(command, wait=0.4):
    s.write((command + "\n").encode())
    return read_for(wait)


def json_line(text, prefix):
    """Die erste Zeile, die mit `prefix` beginnt, als JSON. Gehört das Präfix selbst zum
    JSON (beginnt mit '{'), wird die ganze Zeile gelesen."""
    for line in text.splitlines():
        if line.startswith(prefix):
            return json.loads(line if prefix.startswith("{") else line[len(prefix):])
    raise RuntimeError(f"keine Zeile mit {prefix!r} in:\n{text}")


def stat():
    return json_line(send("stat", 0.4), "{")


boot = read_for(4)
print("--- Start ---")
print("\n".join(l for l in boot.splitlines() if l and not l.startswith(("ESP-ROM", "Build", "rst", "SPIWP", "mode", "load", "entry"))))

schema = json_line(send("schema", 1.0), '{"slots"')
groups = []
for p in schema["params"]:
    if p["group"] not in groups:
        groups.append(p["group"])
print(f"--- Schema: {len(schema['params'])} Parameter, {schema['slots']} Plätze, Gruppen: {', '.join(groups)}")

state = json_line(send("state", 1.0), "state ")
print(f"--- Zustand: Platz {state['slot'] + 1} „{state['name']}“, {len(state['v'])} Werte, "
      f"belegt: {sum(state['used'])}, eigene: {sum(state['own'])}, thru={state['thru']}")
assert len(state["v"]) == len(schema["params"])

def peak():
    text = send("peak", 0.3)
    for line in text.splitlines():
        if line.startswith("PEAK "):
            return int(line[5:]) / 327.67
    raise RuntimeError(text)


print("--- Werksklänge, je eine Note A3 (Spitze in % Vollaussteuerung)")
for slot in range(8):
    out = send(f"load {slot}", 0.3)
    name = next((l for l in out.splitlines() if l.startswith("PRESET")), "?")
    peak()
    send("on 57 100", 0.7)
    attack = peak()
    read_for(0.5)
    held = peak()
    send("off 57", 1.0)
    load = stat()
    print(f"  {name:28s} erste 0,7 s {attack:5.1f} %   danach gehalten {held:5.1f} %"
          f"   CPU {load['load']:.1f} % (max {load['max']:.1f})")

print("--- Lautstärke 0 muss still sein")
send("load 0", 0.3)
send("set p.vol 0", 0.2)
send("on 60 100", 1.3)
print(f"  Pegel bei Lautstärke 0: {stat()['peak'] / 327.67:.1f} %")
send("off 60", 0.2)
send("revert", 0.3)

print("--- Speichern und Löschen auf Platz 32")
out = send("save 31", 1.5)
print("  " + " | ".join(l for l in out.splitlines() if "Gespeichert" in l or "fehl" in l))
state = json_line(send("state", 1.0), "state ")
print(f"  Platz 32 belegt={state['used'][31]} eigen={state['own'][31]} Name „{state['names'][31]}“, aktueller Platz {state['slot'] + 1}")
out = send("erase 31", 1.0)
print("  " + " | ".join(l for l in out.splitlines() if "gelöscht" in l or "fehl" in l))
state = json_line(send("state", 1.0), "state ")
print(f"  danach: Platz 32 belegt={state['used'][31]} eigen={state['own'][31]}")

final = stat()
print(f"--- Ende: CPU {final['load']:.1f} %, längste Lücke seit Start {final['gap']:.1f} ms, "
      f"verworfene Befehle {final['drop']}, Heap frei {final['heap']} KB")
