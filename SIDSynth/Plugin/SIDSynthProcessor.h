// SID-Synth als VST3: dieselbe Klangerzeugung wie die Box (engine_core), je geladenem
// Exemplar ein eigener SID.
//
// Was auf der Box engine.cpp und synth_main.cpp tun, macht hier processBlock():
// - gerechnet wird wie auf der Box mit 44,1 kHz in Blöcken zu 128 Samples, die Modulation
//   läuft also genauso schnell. Läuft die DAW mit 44,1 kHz, geht das Ergebnis bitgleich
//   hinaus, sonst wird sauber auf ihre Rate umgerechnet (gefenstertes Sinc).
// - MIDI wird an der Stelle im Puffer übernommen, an der es steht, und wirkt wie auf der
//   Box ab dem nächsten 128er-Block (≤ 2,9 ms).
// - Die 65 Parameter kommen aus params.h, Automation inklusive.
// - Programme sind die 32 Plätze — Werksklänge und die Klänge der Mac-App aus
//   ~/Library/Application Support/SIDSynth/Presets, im selben Format wie auf der Box.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <functional>
#include <vector>

#include "engine_core.h"
#include "factory.h"
#include "params.h"

class SIDSynthProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    SIDSynthProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SID-Synth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return synth::presets::kSlots; }
    int getCurrentProgram() override { return program_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& data) override;
    void setStateInformation(const void* data, int size) override;

    /// Der Ordner der Mac-App, aus dem die eigenen Klänge kommen und in den das Plug-in
    /// speichert — eine Preset-Sammlung für App und Plug-in.
    static juce::File presetFolder();

    // MARK: Web-Editor (EditorProtocol.cpp), nur auf dem Hauptthread

    /// Eine Befehlszeile aus dem Editor — dieselben Befehle wie handle() in synth_main.cpp.
    void handleCommand(const juce::String& line);
    /// Alle belegten Plätze im Format von /api/backup.
    juce::String backupJson() const;
    /// Eine Sicherung laden, wie restore_presets() der Box.
    void restorePresets(const juce::String& text);
    /// Wohin Zeilen an den Editor gehen; leer, solange keiner offen ist.
    void setEditorSender(std::function<void(const juce::String&)> sender);

private:
    struct Slot {
        bool used = false;
        synth::Patch patch{};
    };

    void timerCallback() override;
    void loadSlot(int slot);

    // Editor (EditorProtocol.cpp)
    void send(const juce::String& line);
    void notify(const juce::String& text) { send("msg " + text); }
    void broadcastState();
    juce::String stateMessage() const;
    juce::String statusLine();
    bool isDirty() const;
    void setParam(size_t index, int value);
    void loadProgram(int slot);
    void saveProgram(int slot);
    int step(int from, int direction) const;
    void pushCommand(const synth::core::Command& command);
    void sendChangedParameters();
    /// Setzt alle Parameter auf diesen Klang und meldet es der DAW (Hauptthread).
    void applyPatch(const synth::Patch& patch);
    synth::Patch currentPatch() const;
    int currentValue(size_t index) const;

    void syncParameters();
    void handle(const juce::MidiMessage& message);
    void produce(float* out, int count);
    void renderBlock(int16_t* block);

    synth::core::Core core_;
    std::vector<juce::RangedAudioParameter*> params_;
    std::array<int16_t, synth::kParamCount> applied_{};
    juce::String name_ = "Init";
    std::array<Slot, synth::presets::kSlots> slots_;
    int program_ = 0;
    std::atomic<int> requestedProgram_{-1};  // Program Change aus dem Audio-Thread
    std::array<bool, synth::presets::kSlots> own_{};  // Platz hat eine Datei im Ordner

    // Editor
    std::function<void(const juce::String&)> sender_;
    std::array<int16_t, synth::kParamCount> sent_{};  // zuletzt an den Editor gemeldet
    bool thru_ = true, ezag_ = false;                  // nur für die Form des Zustands
    int start_ = 0;
    std::array<juce::String, 3> knobs_{"f.cut", "f.res", "l1.rate"};
    int ticks_ = 0;

    // Noten und Panik aus dem Editor: Hauptthread → Audio-Thread
    juce::AbstractFifo fifo_{256};
    std::array<synth::core::Command, 256> fifoItems_{};

    // Statuszeile
    std::atomic<double> busySeconds_{0};
    std::atomic<int> peak_{0};
    std::atomic<juce::uint32> midiIn_{0};
    double lastStat_ = 0;

    // 44,1 kHz → Rate der DAW
    bool passthrough_ = true;
    double ratio_ = 1.0;  // Samples bei 44,1 kHz je Sample der DAW
    juce::Interpolators::WindowedSinc resampler_;
    std::vector<float> pending_;  // fertig gerechnet, noch nicht umgesetzt
    size_t pendingStart_ = 0, pendingEnd_ = 0;
    int16_t block_[synth::core::kBlock] = {};
    int blockPos_ = synth::core::kBlock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SIDSynthProcessor)
};
