#!/bin/sh
# Baut SID-Synth für den Mac zum Weitergeben: Developer-ID-Signatur, Beglaubigung
# (Notarisierung) durch Apple, dann ein DMG zum Hineinziehen in „Programme“.
#
#   make synth-dmg            Ergebnis: build/SIDSynth-<Version>.dmg
#   make synth-dmg RESUME=1   nur weiter warten — wenn Apple beim ersten Mal länger braucht
#
# Braucht das Zertifikat „Developer ID Application“ und das Entwicklerkonto in Xcode.
# Apple prüft die App meist in wenigen Minuten; das Skript wartet so lange.
set -eu

ARCHIVE=build/SIDSynth.xcarchive
UPLOAD=build/notary-upload
DONE=build/notarized

rm -rf "$DONE"
if [ "${RESUME:-0}" != 1 ]; then
    rm -rf "$ARCHIVE" "$UPLOAD"
    xcodebuild archive -project MidiFilter.xcodeproj -scheme SIDSynth \
        -destination 'generic/platform=macOS' -archivePath "$ARCHIVE" -quiet

    echo "== Signieren und an Apple schicken"
    xcodebuild -exportArchive -archivePath "$ARCHIVE" \
        -exportOptionsPlist SIDSynth/Release/ExportOptions.plist \
        -exportPath "$UPLOAD" -allowProvisioningUpdates -quiet
fi

echo "== Warten auf die Beglaubigung"
for i in $(seq 1 60); do
    if xcodebuild -exportNotarizedApp -archivePath "$ARCHIVE" -exportPath "$DONE" >/dev/null 2>&1; then
        break
    fi
    sleep 20
done
APP="$DONE/SIDSynth.app"
[ -d "$APP" ] || {
    echo "Beglaubigung noch nicht fertig oder abgelehnt. Später weiter mit:"
    echo "  make synth-dmg RESUME=1     (Stand in Xcode → Window → Organizer)"
    exit 1
}
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose "$APP"

VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist")
DMG="build/SIDSynth-$VERSION.dmg"
STAGE=build/dmg
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/SIDSynth.app"
ln -s /Applications "$STAGE/Programme"
hdiutil create -volname "SID-Synth" -srcfolder "$STAGE" -format UDZO "$DMG" -quiet
# Die Signatur des DMG selbst ist nur eine Zugabe — geprüft wird die beglaubigte App darin.
# Scheitert sie (fragt macOS nach dem Schlüsselbund, geht das im Terminal nicht), egal.
codesign --sign "Developer ID Application" --timestamp "$DMG" 2>/dev/null \
    || echo "   (DMG ohne eigene Signatur — die App darin ist beglaubigt, das reicht)"
rm -rf "$STAGE"
echo "== Fertig: $DMG"
