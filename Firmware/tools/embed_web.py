"""Packt web/index.html mit gzip und legt es als C-Array in src/synth/web_page.h ab.

Läuft vor jedem Bau der Umgebung `synth` (extra_scripts in platformio.ini). Die Datei
wird nur neu geschrieben, wenn sich etwas geändert hat — sonst übersetzt PlatformIO
jedes Mal alles, was sie einbindet.
"""
import gzip
import os

Import("env")  # noqa: F821 — von PlatformIO bereitgestellt

project = env["PROJECT_DIR"]  # noqa: F821
source = os.path.join(project, "web", "index.html")
target = os.path.join(project, "src", "synth", "web_page.h")

with open(source, "rb") as f:
    packed = gzip.compress(f.read(), compresslevel=9, mtime=0)

lines = []
for start in range(0, len(packed), 20):
    chunk = packed[start:start + 20]
    lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

header = (
    "// Erzeugt von tools/embed_web.py aus web/index.html — nicht von Hand ändern.\n"
    "\n"
    "#pragma once\n"
    "\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "\n"
    "const uint8_t kIndexHtmlGz[] = {\n"
    + "\n".join(lines)
    + "\n};\n"
    f"const size_t kIndexHtmlGzSize = {len(packed)};\n"
)

old = None
if os.path.exists(target):
    with open(target, "r", encoding="utf-8") as f:
        old = f.read()
if old != header:
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"embed_web: web/index.html -> {len(packed)} Bytes gepackt")
