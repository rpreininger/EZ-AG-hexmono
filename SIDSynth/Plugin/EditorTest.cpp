// Öffnet das Plug-in-Fenster mit dem echten Web-Editor und bedient ihn über das JavaScript
// der Seite: Aufbau aus dem Schema, Zustand, Wert ändern, Speichern, Löschen, Sicherung
// laden. Schreibt nur in einen Ordner unter /tmp, nicht in die echten Presets.

#include <juce_gui_extra/juce_gui_extra.h>

#include "SIDSynthEditor.h"
#include "SIDSynthProcessor.h"

namespace {

int failures = 0;

void check(bool ok, const juce::String& what) {
    if (!ok) {
        ++failures;
        std::printf("FEHLER: %s\n", what.toRawUTF8());
    }
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

/// Führt JavaScript aus und wartet auf das Ergebnis. Der Text ist UTF-8 (Umlaute).
juce::var evaluate(SIDSynthEditor& editor, const char* js) {
    juce::var result;
    bool done = false;
    editor.browser().evaluateJavascript(juce::String::fromUTF8(js), [&](juce::WebBrowserComponent::EvaluationResult r) {
        if (const auto* value = r.getResult()) result = *value;
        done = true;
    });
    for (int i = 0; i < 100 && !done; ++i) pump(20);
    return result;
}

int paramIndex(SIDSynthProcessor& p, const juce::String& name) {
    for (int i = 0; i < p.getParameters().size(); ++i) {
        if (p.getParameters()[i]->getName(64) == name) return i;
    }
    return -1;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("sidsynth-editortest-" + juce::String(juce::Time::currentTimeMillis()));
    folder.createDirectory();
    setenv("SIDSYNTH_PRESET_DIR", folder.getFullPathName().toRawUTF8(), 1);

    {
        SIDSynthProcessor processor;
        processor.prepareToPlay(44100, 512);
        std::unique_ptr<juce::AudioProcessorEditor> base(processor.createEditor());
        auto& editor = *dynamic_cast<SIDSynthEditor*>(base.get());
        juce::DocumentWindow window("SID-Synth Test", juce::Colours::black, 0);
        window.setContentNonOwned(&editor, true);
        window.setVisible(true);

        // Seite laden lassen: Schema holen, Regler bauen, Zustand empfangen
        juce::var page;
        for (int i = 0; i < 50; ++i) {
            pump(100);
            page = evaluate(editor, "JSON.stringify({rows: document.querySelectorAll('.row').length,"
                                    " slots: document.getElementById('slot').options.length,"
                                    " name: document.getElementById('name').value})");
            if (page.toString().contains("\"slots\":32")) break;
        }
        const juce::var info = juce::JSON::parse(page.toString());
        std::printf("Seite: %s\n", page.toString().toRawUTF8());
        // je Parameter eine Zeile, dazu die Einstellungen
        check(static_cast<int>(info["rows"]) >= static_cast<int>(synth::kParamCount), "Regler aus dem Schema gebaut");
        check(static_cast<int>(info["slots"]) == 32, "32 Plätze im Zustand");
        check(info["name"].toString() == juce::String::fromUTF8("Sägezahn-Lead"), "Name des Klangs: " + info["name"].toString());

        // Wert ändern wie mit dem Regler
        evaluate(editor, "cmd('set f.cut 500'); 1");
        pump(300);
        const int cut = paramIndex(processor, "Filter Cutoff");
        const float value = processor.getParameters()[cut]->getValue();
        check(std::abs(value * 2047 - 500) < 0.6f, "Cutoff 500 beim Plug-in angekommen");
        const juce::var shown = evaluate(editor, "st.v[byKey['f.cut']] + ' ' + st.dirty");
        check(shown.toString() == "500 true", "Editor zeigt 500 und „geändert“: " + shown.toString());

        // Speichern auf Platz 21, Name mit Umlaut
        evaluate(editor, "cmd('name Test ü'); cmd('save 20'); 1");
        pump(500);
        const juce::File saved = folder.getChildFile("patch-20.json");
        check(saved.existsAsFile(), "Datei patch-20.json geschrieben");
        const juce::var json = juce::JSON::parse(saved);
        check(json["name"].toString() == juce::String::fromUTF8("Test ü"), "Name in der Datei: " + json["name"].toString());
        check(static_cast<int>(json["v"]["f.cut"]) == 500, "Cutoff in der Datei");
        const juce::var after = evaluate(editor, "st.slot + ' ' + st.own[20] + ' ' + st.dirty + ' ' + st.names[20]");
        check(after.toString() == juce::String::fromUTF8("20 1 false Test ü"), "Zustand nach dem Speichern: " + after.toString());
        check(processor.getCurrentProgram() == 20, "Programm der DAW folgt");

        // Löschen
        evaluate(editor, "cmd('erase 20'); 1");
        pump(300);
        check(!saved.exists(), "Datei gelöscht");

        // Sicherung laden über „Presets aus Datei laden“ (fetch-POST, von der Brücke abgefangen)
        evaluate(editor, "fetch('/api/restore', {method: 'POST', body: JSON.stringify({fmt: 1, presets:"
                         " [{slot: 30, name: 'Von der Box', v: {'f.cut': 1234}}]})}); 1");
        pump(500);
        const juce::var restored = juce::JSON::parse(folder.getChildFile("patch-29.json"));
        check(restored["name"].toString() == "Von der Box" && static_cast<int>(restored["v"]["f.cut"]) == 1234,
              "Sicherung geladen nach Platz 30");
        const juce::var names = evaluate(editor, "st.names[29]");
        check(names.toString() == "Von der Box", "Editor kennt Platz 30: " + names.toString());

        // Klaviatur des Editors spielt: Note an, ein paar Blöcke rechnen, Pegel muss steigen
        evaluate(editor, "cmd('load 0'); cmd('on 57 100'); 1");
        pump(200);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        float peak = 0;
        for (int i = 0; i < 40; ++i) {
            processor.processBlock(buffer, midi);
            peak = std::max(peak, buffer.getMagnitude(0, 0, 512));
        }
        check(peak > 0.1f, "Note aus dem Editor klingt: " + juce::String(peak));
        std::printf("Note aus dem Editor: Spitze %.0f %%\n", peak * 100);

        window.clearContentComponent();
        processor.releaseResources();
    }
    folder.deleteRecursively();
    std::printf("Editor: %d Fehler\n", failures);
    return failures == 0 ? 0 : 1;
}
