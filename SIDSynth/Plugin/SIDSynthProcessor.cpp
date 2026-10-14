#include "SIDSynthProcessor.h"

#include <math.h>
#include <string.h>

#include "ParamText.h"
#include "SIDSynthEditor.h"

using namespace synth;
using core::Command;
using core::CommandType;
using core::kBlock;

// Gemeinsam mit EditorProtocol.cpp
bool patchFromJson(const juce::var& json, Patch& patch);
juce::var patchToJson(const Patch& patch);

namespace {

juce::String displayName(const Param& p) { return juce::String(p.group) + " " + p.label; }

}  // namespace

/// Wie read_file() der Box: unbekannte Schlüssel übergehen, fehlende behalten die
/// Voreinstellung, jeder Wert wird auf seinen Bereich begrenzt.
bool patchFromJson(const juce::var& json, Patch& patch) {
    if (!json.isObject()) return false;
    const juce::var name = json["name"];
    init_patch(patch, name.isString() ? name.toString().toRawUTF8() : "Ohne Namen");
    const juce::var values = json["v"];
    for (size_t i = 0; i < kParamCount; ++i) {
        const juce::var value = values[kParams[i].key];
        if (value.isInt() || value.isInt64()) patch.values[i] = clamp_value(i, static_cast<long>(static_cast<juce::int64>(value)));
    }
    return true;
}

juce::var patchToJson(const Patch& patch) {
    auto* values = new juce::DynamicObject();
    for (size_t i = 0; i < kParamCount; ++i) values->setProperty(kParams[i].key, patch.values[i]);
    auto* root = new juce::DynamicObject();
    root->setProperty("fmt", 1);
    root->setProperty("name", juce::String::fromUTF8(patch.name));
    root->setProperty("v", juce::var(values));
    return juce::var(root);
}

SIDSynthProcessor::SIDSynthProcessor()
    : AudioProcessor(BusesProperties().withOutput("Ausgang", juce::AudioChannelSet::stereo(), true)) {
    for (size_t i = 0; i < kParamCount; ++i) {
        const Param& p = kParams[i];
        const juce::ParameterID id{p.key, 1};
        std::unique_ptr<juce::RangedAudioParameter> parameter;
        if (p.kind == Kind::Toggle) {
            parameter = std::make_unique<juce::AudioParameterBool>(id, displayName(p), p.def != 0);
        } else if (p.kind == Kind::Choice) {
            parameter = std::make_unique<juce::AudioParameterChoice>(
                id, displayName(p), juce::StringArray::fromTokens(juce::String::fromUTF8(p.choices), "|", ""),
                p.def);
        } else {
            parameter = std::make_unique<juce::AudioParameterInt>(
                id, displayName(p), p.min, p.max, p.def,
                juce::AudioParameterIntAttributes().withStringFromValueFunction(
                    [&p](int value, int) { return juce::String::fromUTF8(param_text(p, value).c_str()); }));
        }
        params_.push_back(parameter.get());
        addParameter(parameter.release());
    }
    for (int slot = 0; slot < presets::kSlots; ++slot) loadSlot(slot);
    if (slots_[0].used) {
        name_ = juce::String::fromUTF8(slots_[0].patch.name);
        for (size_t i = 0; i < kParamCount; ++i) {
            auto* p = params_[i];
            p->setValueNotifyingHost(p->convertTo0to1(slots_[0].patch.values[i]));
        }
    }
    core_.begin(currentPatch());
    for (size_t i = 0; i < kParamCount; ++i) applied_[i] = static_cast<int16_t>(currentValue(i));
    startTimerHz(20);
}

// MARK: - Plätze

juce::File SIDSynthProcessor::presetFolder() {
    // Nur für Tests: ein anderer Ordner, damit sie nicht die echten Presets anfassen.
    const juce::String override = juce::SystemStats::getEnvironmentVariable("SIDSYNTH_PRESET_DIR", {});
    if (override.isNotEmpty()) return juce::File(override);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Application Support/SIDSynth/Presets");
}

/// Eigener Klang der Mac-App, sonst der Werksklang — wie presets::begin() auf der Box.
void SIDSynthProcessor::loadSlot(int slot) {
    Slot& s = slots_[static_cast<size_t>(slot)];
    const juce::File file = presetFolder().getChildFile(juce::String::formatted("patch-%02d.json", slot));
    s.used = file.existsAsFile() && patchFromJson(juce::JSON::parse(file), s.patch);
    own_[static_cast<size_t>(slot)] = s.used;
    if (!s.used) s.used = presets::factory(slot, s.patch);
    if (!s.used) init_patch(s.patch, "");
}

void SIDSynthProcessor::setCurrentProgram(int index) {
    if (index < 0 || index >= presets::kSlots) return;
    loadSlot(index);  // die App könnte ihn inzwischen geändert haben
    if (!slots_[static_cast<size_t>(index)].used) return;
    program_ = index;
    applyPatch(slots_[static_cast<size_t>(index)].patch);
    broadcastState();
}

const juce::String SIDSynthProcessor::getProgramName(int index) {
    if (index < 0 || index >= presets::kSlots) return {};
    const Slot& s = slots_[static_cast<size_t>(index)];
    return juce::String(index + 1).paddedLeft('0', 2) + " " +
           (s.used ? juce::String::fromUTF8(s.patch.name) : juce::String("— leer —"));
}

void SIDSynthProcessor::applyPatch(const Patch& patch) {
    name_ = juce::String::fromUTF8(patch.name);
    for (size_t i = 0; i < kParamCount; ++i) {
        auto* p = params_[i];
        p->setValueNotifyingHost(p->convertTo0to1(patch.values[i]));
    }
}

int SIDSynthProcessor::currentValue(size_t index) const {
    auto* p = params_[index];
    return static_cast<int>(lroundf(p->convertFrom0to1(p->getValue())));
}

Patch SIDSynthProcessor::currentPatch() const {
    Patch patch;
    init_patch(patch, name_.toRawUTF8());
    for (size_t i = 0; i < kParamCount; ++i) patch.values[i] = clamp_value(i, currentValue(i));
    return patch;
}

/// Program Change aus dem Audio-Thread: die Parameter setzt der Hauptthread.
void SIDSynthProcessor::timerCallback() {
    const int program = requestedProgram_.exchange(-1);
    if (program >= 0) {
        setCurrentProgram(program);
        updateHostDisplay(ChangeDetails().withProgramChanged(true));
    }
    sendChangedParameters();
    // Wie die Box: einmal je Sekunde eine Statuszeile an den Editor.
    if (++ticks_ >= 20) {
        ticks_ = 0;
        send("stat " + statusLine());
    }
}

// MARK: - Zustand im Projekt der DAW

void SIDSynthProcessor::getStateInformation(juce::MemoryBlock& data) {
    juce::var json = patchToJson(currentPatch());
    json.getDynamicObject()->setProperty("program", program_);
    const juce::String text = juce::JSON::toString(json, true);
    data.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void SIDSynthProcessor::setStateInformation(const void* data, int size) {
    const juce::var json = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data), size));
    Patch patch;
    if (!patchFromJson(json, patch)) return;
    program_ = juce::jlimit(0, presets::kSlots - 1, static_cast<int>(json.getProperty("program", 0)));
    applyPatch(patch);
    broadcastState();
}

// MARK: - Audio

bool SIDSynthProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void SIDSynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // Beim Einschalten der Wiedergabe frisch anfangen, wie die Box beim Einschalten — auch
    // hängende Noten sind damit weg.
    core_.begin(currentPatch());
    for (size_t i = 0; i < kParamCount; ++i) applied_[i] = static_cast<int16_t>(currentValue(i));
    passthrough_ = std::abs(sampleRate - kSampleRate) < 0.5;
    ratio_ = kSampleRate / sampleRate;
    resampler_.reset();
    const size_t capacity = static_cast<size_t>(std::ceil(samplesPerBlock * ratio_)) + 4 * kBlock + 64;
    pending_.assign(capacity, 0.0f);
    pendingStart_ = pendingEnd_ = 0;
    blockPos_ = kBlock;
    setLatencySamples(passthrough_ ? 0
                                   : static_cast<int>(std::lround(juce::Interpolators::WindowedSinc::getBaseLatency() / ratio_)));
}

/// Neue Werte aus Automation oder Bedienung, wie engine::set_param() auf der Box.
void SIDSynthProcessor::syncParameters() {
    for (size_t i = 0; i < kParamCount; ++i) {
        const int16_t value = clamp_value(i, currentValue(i));
        if (value == applied_[i]) continue;
        applied_[i] = value;
        core_.apply(Command{CommandType::SetParam, static_cast<uint8_t>(i), value});
    }
}

/// Wie play() in synth_main.cpp.
void SIDSynthProcessor::handle(const juce::MidiMessage& m) {
    midiIn_.fetch_add(1);
    if (m.isNoteOn()) {
        core_.apply(Command{CommandType::NoteOn, static_cast<uint8_t>(m.getNoteNumber() & 0x7F),
                            static_cast<int16_t>(m.getVelocity() & 0x7F)});
    } else if (m.isNoteOff()) {
        core_.apply(Command{CommandType::NoteOff, static_cast<uint8_t>(m.getNoteNumber() & 0x7F), 0});
    } else if (m.isPitchWheel()) {
        core_.apply(Command{CommandType::Bend, 0, static_cast<int16_t>(m.getPitchWheelValue() - 8192)});
    } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
        core_.apply(Command{CommandType::AllOff, 0, 0});
    } else if (m.isProgramChange()) {
        requestedProgram_.store(m.getProgramChangeNumber());
    }
}

void SIDSynthProcessor::renderBlock(int16_t* block) {
    const auto start = juce::Time::getHighResolutionTicks();
    syncParameters();
    const int peak = core_.render_block(block);
    if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak);
    busySeconds_.store(busySeconds_.load(std::memory_order_relaxed) +
                       juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start));
}

void SIDSynthProcessor::produce(float* out, int count) {
    if (count <= 0) return;
    if (passthrough_) {
        for (int i = 0; i < count; ++i) {
            if (blockPos_ == kBlock) {
                renderBlock(block_);
                blockPos_ = 0;
            }
            out[i] = block_[blockPos_++] / 32768.0f;
        }
        return;
    }
    // In Stücken, damit der Vorrat nie überläuft, auch wenn die DAW mehr schickt als angekündigt.
    while (count > 0) {
        const int chunk = std::min(count, 256);
        const size_t needed = static_cast<size_t>(std::ceil(chunk * ratio_)) + 4;
        if (pendingStart_ > 0 && pending_.size() - pendingEnd_ < needed + kBlock) {
            std::memmove(pending_.data(), pending_.data() + pendingStart_,
                         (pendingEnd_ - pendingStart_) * sizeof(float));
            pendingEnd_ -= pendingStart_;
            pendingStart_ = 0;
        }
        while (pendingEnd_ - pendingStart_ < needed && pendingEnd_ + kBlock <= pending_.size()) {
            renderBlock(block_);
            for (int s = 0; s < kBlock; ++s) pending_[pendingEnd_++] = block_[s] / 32768.0f;
        }
        const int used = resampler_.process(ratio_, pending_.data() + pendingStart_, out, chunk);
        pendingStart_ = std::min(pendingEnd_, pendingStart_ + static_cast<size_t>(used));
        out += chunk;
        count -= chunk;
    }
}

void SIDSynthProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    // Noten und Panik aus dem Editor
    {
        const auto scope = fifo_.read(fifo_.getNumReady());
        for (int i = scope.startIndex1; i < scope.startIndex1 + scope.blockSize1; ++i) core_.apply(fifoItems_[static_cast<size_t>(i)]);
        for (int i = scope.startIndex2; i < scope.startIndex2 + scope.blockSize2; ++i) core_.apply(fifoItems_[static_cast<size_t>(i)]);
    }
    const int samples = buffer.getNumSamples();
    float* out = buffer.getWritePointer(0);

    // Bis zu jedem MIDI-Ereignis rechnen, dann das Ereignis übernehmen — es wirkt ab dem
    // nächsten 128er-Block, wie auf der Box.
    int position = 0;
    for (const auto meta : midi) {
        const int at = juce::jlimit(position, samples, meta.samplePosition);
        produce(out + position, at - position);
        position = at;
        handle(meta.getMessage());
    }
    produce(out + position, samples - position);

    for (int channel = 1; channel < buffer.getNumChannels(); ++channel) buffer.copyFrom(channel, 0, buffer, 0, 0, samples);
}

juce::AudioProcessorEditor* SIDSynthProcessor::createEditor() { return new SIDSynthEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SIDSynthProcessor(); }
