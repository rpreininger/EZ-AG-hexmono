// Lädt das fertige SID-Synth.vst3 wie eine DAW und prüft:
// - bei 44,1 kHz kommt bitgleich heraus, was engine_core der Box rechnet
// - bei 48 kHz kommt ein sauberer Ton mit demselben Pegel heraus
// - Programme und der im Projekt gespeicherte Zustand
//
//   SIDSynthPluginTest <Pfad zu SID-Synth.vst3>

#include <juce_audio_processors/juce_audio_processors.h>

#include <math.h>

#include <vector>

#include "engine_core.h"
#include "factory.h"
#include "params.h"

using namespace synth;

namespace {

int failures = 0;

void check(bool ok, const juce::String& what) {
    if (!ok) {
        ++failures;
        std::printf("FEHLER: %s\n", what.toRawUTF8());
    }
}

std::unique_ptr<juce::AudioPluginInstance> load(const juce::String& path, double rate, int block) {
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, path);
    if (types.isEmpty()) return nullptr;
    juce::String error;
    auto plugin = format.createInstanceFromDescription(*types[0], rate, block, error);
    if (plugin == nullptr) std::printf("Laden fehlgeschlagen: %s\n", error.toRawUTF8());
    return plugin;
}

/// Spielt nach `settle` Sekunden Stille eine Note A3 für `on` Sekunden. Liefert Kanal 0.
std::vector<float> play(juce::AudioPluginInstance& plugin, double rate, int block, double settle, double on) {
    plugin.prepareToPlay(rate, block);
    const int total = static_cast<int>((settle + on) * rate);
    const int noteAt = static_cast<int>(settle * rate);
    std::vector<float> out;
    juce::AudioBuffer<float> buffer(2, block);
    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        if (noteAt >= pos && noteAt < pos + n) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8)100), noteAt - pos);
        plugin.processBlock(buffer, midi);
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + n);
        check(std::equal(buffer.getReadPointer(0), buffer.getReadPointer(0) + n, buffer.getReadPointer(1)),
              "beide Kanäle gleich");
    }
    plugin.releaseResources();
    return out;
}

double peak(const std::vector<float>& s, size_t from) {
    float p = 0;
    for (size_t i = from; i < s.size(); ++i) p = std::max(p, std::abs(s[i]));
    return p * 100.0;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc < 2) {
        std::printf("Aufruf: SIDSynthPluginTest <SID-Synth.vst3>\n");
        return 2;
    }
    const juce::String path = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]).getFullPathName();

    // 1. 44,1 kHz: bitgleich mit engine_core, wie die Box es rechnet
    {
        auto plugin = load(path, 44100, 512);
        if (plugin == nullptr) return 1;
        check(plugin->getName() == "SID-Synth", "Name " + plugin->getName());
        // Dazu kommen die des VST3-Wrappers: Bypass und MIDI-Controller samt Pitchbend, die
        // VST3 als Parameter statt als MIDI liefert.
        check(plugin->getParameters().size() >= static_cast<int>(kParamCount), "Anzahl Parameter");
        check(plugin->getNumPrograms() == presets::kSlots, "Anzahl Programme");
        plugin->setCurrentProgram(0);  // Werksklang 1 — falls die App Platz 1 überschrieben hat, deren Klang
        const std::vector<float> got = play(*plugin, 44100, 512, 1.5, 1.2);

        // Dieselbe Rechnung direkt: der Klang, den das Plug-in gerade hat — gelesen über die
        // Parameter, so wie die DAW sie sieht —, Note ab dem nächsten 128er-Block nach ihrer
        // Position.
        Patch patch;
        init_patch(patch, "");
        size_t found = 0;
        for (auto* hosted : plugin->getParameters()) {
            for (size_t i = 0; i < kParamCount; ++i) {
                const Param& p = kParams[i];
                if (hosted->getName(256) != juce::String(p.group) + " " + p.label) continue;
                patch.values[i] = clamp_value(i, p.min + std::lround(hosted->getValue() * (p.max - p.min)));
                ++found;
            }
        }
        check(found == kParamCount, "alle Parameter beim Namen gefunden: " + juce::String(found));
        static core::Core reference;
        reference.begin(patch);
        const int noteBlock = (static_cast<int>(1.5 * 44100) + core::kBlock - 1) / core::kBlock;
        std::vector<float> want;
        int16_t block[core::kBlock];
        for (int b = 0; want.size() < got.size(); ++b) {
            if (b == noteBlock) reference.apply({core::CommandType::NoteOn, 57, 100});
            reference.render_block(block);
            for (int s = 0; s < core::kBlock; ++s) want.push_back(block[s] / 32768.0f);
        }
        want.resize(got.size());
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i] != want[i]) {
                std::printf("erste Abweichung bei Sample %zu (Block %zu, Note ab Block %d): %f statt %f\n",
                            i, i / core::kBlock, noteBlock, got[i], want[i]);
                break;
            }
        }
        check(got == want, "44,1 kHz bitgleich mit engine_core");
        std::printf("44,1 kHz: Spitze %.1f %%, %s\n", peak(got, 66150), got == want ? "bitgleich mit der Box" : "WEICHT AB");

        // 2. Programm wechseln, Zustand sichern und in einem zweiten Exemplar laden
        plugin->setCurrentProgram(5);
        juce::MemoryBlock saved;
        plugin->getStateInformation(saved);
        auto second = load(path, 44100, 512);
        second->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        bool same = true;
        for (int i = 0; i < plugin->getParameters().size(); ++i) {
            same = same && plugin->getParameters()[i]->getValue() == second->getParameters()[i]->getValue();
        }
        check(same, "Zustand übertragen");
        check(plugin->getProgramName(5).startsWith("06 "), "Programmname " + plugin->getProgramName(5));
        std::printf("Programm 6 „%s“, Zustand %s\n", plugin->getProgramName(5).toRawUTF8(), same ? "übertragen" : "FALSCH");
    }

    // 3. 48 kHz: umgerechnet, gleicher Pegel, keine Aussetzer
    {
        auto plugin = load(path, 48000, 480);
        plugin->setCurrentProgram(0);
        const std::vector<float> got = play(*plugin, 48000, 480, 1.5, 1.2);
        const double p = peak(got, 72000);
        bool finite = true;
        for (float x : got) finite = finite && std::isfinite(x);
        check(finite, "48 kHz: keine ungültigen Werte");
        check(p > 15 && p < 60, "48 kHz: Pegel " + juce::String(p));
        std::printf("48 kHz: Spitze %.1f %%\n", p);
    }

    std::printf("Plug-in: %d Fehler\n", failures);
    return failures == 0 ? 0 : 1;
}
