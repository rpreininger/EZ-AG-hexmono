APP := build/Build/Products/Debug/MidiFilter.app

.PHONY: project build run test clean demo demo-loop firmware-test synth-test synth-run synth-ipad synth-dmg synth-testflight

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

# Die Klangerzeugung der Box (reSID, engine_core, Werksklänge) auf dem Mac geprüft.
SYNTH_SRC := $(wildcard Firmware/lib/reSID/src/*.cc) \
             Firmware/src/synth/params.cpp Firmware/src/synth/engine_core.cpp \
             Firmware/src/synth/factory.cpp Firmware/src/midi_filter.cpp \
             Firmware/src/midi_event.cpp SIDSynth/Core/sid_host.cpp
SYNTH_INC := -IFirmware/lib/reSID/src -IFirmware/src/synth -IFirmware/src -ISIDSynth/Core

synth-test:
	@mkdir -p build
	clang++ -std=c++17 -O2 -Wall -Wextra -DRESID_ENABLE_INTERPOLATE $(SYNTH_INC) \
	        $(SYNTH_SRC) SIDSynth/Tests/Core/core_test.cpp -o build/synthtest
	./build/synthtest
	@mkdir -p build/synthobj
	cd build/synthobj && clang++ -std=c++17 -O2 -c -DRESID_ENABLE_INTERPOLATE \
	        $(addprefix -I../../,$(subst -I,,$(SYNTH_INC))) $(addprefix ../../,$(SYNTH_SRC))
	swiftc -O -import-objc-header SIDSynth/Core/sid_host.h -lc++ \
	       SIDSynth/App/Patch.swift SIDSynth/App/BoxLink.swift SIDSynth/App/PresetStore.swift \
	       SIDSynth/App/SynthController.swift SIDSynth/Tests/BoxLinkTest/main.swift \
	       build/synthobj/*.o -o build/boxlinktest
	./build/boxlinktest

# Die Mac-App für Presets: dieselbe Klangerzeugung, derselbe Editor wie auf der Box.
SYNTH_APP := build/Build/Products/Debug/SIDSynth.app

synth-run: project
	xcodebuild -project MidiFilter.xcodeproj -scheme SIDSynth \
	           -destination 'platform=macOS' -configuration Debug \
	           -derivedDataPath build build
	@pkill -x SIDSynth || true
	open $(SYNTH_APP)

# Dieselbe App auf iPad oder iPhone, per Kabel oder WLAN gekoppelt. Gerät wählen mit
# DEVICE=<Name oder Kennung aus `xcrun devicectl list devices`>.
DEVICE ?= iPad von Ralf
SYNTH_IOS_APP := build/Build/Products/Debug-iphoneos/SIDSynth.app

synth-ipad: project
	xcodebuild -project MidiFilter.xcodeproj -scheme SIDSynth \
	           -destination 'generic/platform=iOS' -configuration Debug \
	           -derivedDataPath build -allowProvisioningUpdates build
	xcrun devicectl device install app --device "$(DEVICE)" $(SYNTH_IOS_APP)
	xcrun devicectl device process launch --device "$(DEVICE)" de.ralfpreininger.SIDSynth

# Zum Weitergeben am Mac: signiert, von Apple beglaubigt, als DMG. Siehe das Skript.
synth-dmg: project
	RESUME=$(RESUME) SIDSynth/Release/make_dmg.sh

# iPad und iPhone für Freunde: hochladen zu App Store Connect, verteilt über TestFlight.
# Die Buildnummer ist Datum und Uhrzeit, damit jeder Upload eine neue hat.
synth-testflight: project
	rm -rf build/SIDSynth-iOS.xcarchive build/testflight
	xcodebuild archive -project MidiFilter.xcodeproj -scheme SIDSynth \
	           -destination 'generic/platform=iOS' -archivePath build/SIDSynth-iOS.xcarchive \
	           -allowProvisioningUpdates CURRENT_PROJECT_VERSION=$$(date +%Y%m%d%H%M) -quiet
	xcodebuild -exportArchive -archivePath build/SIDSynth-iOS.xcarchive \
	           -exportOptionsPlist SIDSynth/Release/ExportOptions-TestFlight.plist \
	           -exportPath build/testflight -allowProvisioningUpdates
