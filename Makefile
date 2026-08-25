APP := build/Build/Products/Debug/MidiFilter.app

.PHONY: project build run test clean demo demo-loop firmware-test

project:
	xcodegen generate

build: project
	xcodebuild -project MidiFilter.xcodeproj -scheme MidiFilter \
	           -destination 'platform=macOS' -configuration Debug \
	           -derivedDataPath build build

run: build
	@pkill -x MidiFilter || true
	open $(APP)

test:
	cd Packages/MidiFilterCore && swift test

clean:
	rm -rf build MidiFilter.xcodeproj

# Simuliert eine EZ-AG gegen die laufende App (kein Instrument nötig).
demo:
	swift Tools/fake-ezag.swift

demo-loop:
	swift Tools/fake-ezag.swift loop

# C++-Portierung für den ESP32, auf dem Rechner getestet (kein Board nötig).
firmware-test:
	clang++ -std=c++17 -Wall -Wextra -O1 -fno-exceptions -fno-rtti \
	        Firmware/src/midi_filter.cpp Firmware/src/midi_event.cpp Firmware/test/test_firmware.cpp -o build/fwtest
	./build/fwtest
