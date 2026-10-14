// Das Textprotokoll des Web-Editors, wie handle() und restore_presets() in synth_main.cpp
// der Box und SynthController.swift der Mac-App. Alles hier läuft auf dem Hauptthread.
//
// Gespeichert wird in den Ordner der Mac-App (presetFolder()), im Format der Box — App und
// Plug-in teilen sich damit ihre Klänge.

#include <math.h>

#include "SIDSynthProcessor.h"

using namespace synth;
using core::Command;
using core::CommandType;

bool patchFromJson(const juce::var& json, Patch& patch);
juce::var patchToJson(const Patch& patch);

namespace {

bool samePatch(const Patch& a, const Patch& b) {
    return memcmp(a.values, b.values, sizeof(a.values)) == 0 && strcmp(a.name, b.name) == 0;
}

juce::String quoted(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

}  // namespace

void SIDSynthProcessor::setEditorSender(std::function<void(const juce::String&)> sender) {
    sender_ = std::move(sender);
    for (size_t i = 0; i < kParamCount; ++i) sent_[i] = static_cast<int16_t>(currentValue(i));
}

void SIDSynthProcessor::send(const juce::String& line) {
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        juce::MessageManager::callAsync([this, line] { send(line); });
        return;
    }
    if (sender_) sender_(line);
}

// MARK: - Ausgabe

bool SIDSynthProcessor::isDirty() const {
    const Slot& stored = slots_[static_cast<size_t>(program_)];
    return !stored.used || !samePatch(stored.patch, currentPatch());
}

juce::String SIDSynthProcessor::stateMessage() const {
    const Patch edit = currentPatch();
    juce::String s = "state {\"slot\":" + juce::String(program_) + ",\"start\":" + juce::String(start_) +
                     ",\"thru\":" + juce::String(thru_ ? 1 : 0) + ",\"ezag\":" + juce::String(ezag_ ? 1 : 0) +
                     ",\"dirty\":" + (isDirty() ? "true" : "false") +
                     ",\"name\":" + quoted(juce::String::fromUTF8(edit.name)) + ",\"v\":[";
    for (size_t i = 0; i < kParamCount; ++i) s << (i > 0 ? "," : "") << edit.values[i];
    s << "],\"names\":[";
    for (size_t slot = 0; slot < slots_.size(); ++slot) {
        s << (slot > 0 ? "," : "") << quoted(slots_[slot].used ? juce::String::fromUTF8(slots_[slot].patch.name) : "");
    }
    s << "],\"used\":[";
    for (size_t slot = 0; slot < slots_.size(); ++slot) s << (slot > 0 ? "," : "") << (slots_[slot].used ? 1 : 0);
    s << "],\"own\":[";
    for (size_t slot = 0; slot < slots_.size(); ++slot) s << (slot > 0 ? "," : "") << (own_[slot] ? 1 : 0);
    s << "],\"knobs\":[";
    for (size_t k = 0; k < knobs_.size(); ++k) s << (k > 0 ? "," : "") << quoted(knobs_[k]);
    s << "],\"enc\":false}";
    return s;
}

/// Wie status_line() der Box. `usb` heißt hier: es kam schon MIDI aus der DAW.
juce::String SIDSynthProcessor::statusLine() {
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double elapsed = lastStat_ > 0 ? now - lastStat_ : 1.0;
    lastStat_ = now;
    const double load = 100.0 * busySeconds_.exchange(0) / elapsed;
    const int peak = std::min(32767, peak_.exchange(0));
    return juce::String::formatted("{\"load\":%.1f,\"max\":%.1f,\"gap\":0,\"peak\":%d,\"usb\":%s,"
                                   "\"midi\":%u,\"heap\":0,\"drop\":0}",
                                   load, load, peak, midiIn_.load() > 0 ? "true" : "false",
                                   static_cast<unsigned>(midiIn_.load()));
}

void SIDSynthProcessor::broadcastState() { send(stateMessage()); }

/// Was die DAW (Automation, eigene Regler) geändert hat, an den Editor — wie die `set`-Zeilen
/// der Box.
void SIDSynthProcessor::sendChangedParameters() {
    if (!sender_) return;
    std::vector<size_t> changed;
    for (size_t i = 0; i < kParamCount; ++i) {
        const auto value = static_cast<int16_t>(currentValue(i));
        if (value == sent_[i]) continue;
        sent_[i] = value;
        changed.push_back(i);
    }
    if (changed.empty()) return;
    const juce::String dirty = isDirty() ? " 1" : " 0";
    for (size_t i : changed) send("set " + juce::String(kParams[i].key) + " " + juce::String(sent_[i]) + dirty);
}

// MARK: - Befehle

void SIDSynthProcessor::pushCommand(const Command& command) {
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 > 0) fifoItems_[static_cast<size_t>(scope.startIndex1)] = command;
}

/// Wie apply_param(): Wert setzen, der DAW melden, Echo an den Editor.
void SIDSynthProcessor::setParam(size_t index, int value) {
    const int16_t v = clamp_value(index, value);
    auto* p = params_[index];
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(v));
    p->endChangeGesture();
    sent_[index] = v;
    send("set " + juce::String(kParams[index].key) + " " + juce::String(v) + (isDirty() ? " 1" : " 0"));
}

void SIDSynthProcessor::loadProgram(int slot) {
    if (slot < 0 || slot >= presets::kSlots) return;
    loadSlot(slot);
    if (!slots_[static_cast<size_t>(slot)].used) return;
    setCurrentProgram(slot);
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

void SIDSynthProcessor::saveProgram(int slot) {
    if (slot < 0 || slot >= presets::kSlots) return;
    const Patch patch = currentPatch();
    const juce::File folder = presetFolder();
    folder.createDirectory();
    const juce::File file = folder.getChildFile(juce::String::formatted("patch-%02d.json", slot));
    // replaceWithText schreibt erst eine Hilfsdatei und benennt dann um, wie die Box.
    if (!file.replaceWithText(juce::JSON::toString(patchToJson(patch), true), false, false, "\n")) {
        broadcastState();
        notify("Speichern fehlgeschlagen");
        return;
    }
    Slot& s = slots_[static_cast<size_t>(slot)];
    s.used = true;
    s.patch = patch;
    own_[static_cast<size_t>(slot)] = true;
    program_ = slot;
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
    broadcastState();
    notify("Gespeichert auf Platz " + juce::String(slot + 1));
}

int SIDSynthProcessor::step(int from, int direction) const {
    for (int i = 1; i <= presets::kSlots; ++i) {
        const int slot = ((from + direction * i) % presets::kSlots + presets::kSlots) % presets::kSlots;
        if (slots_[static_cast<size_t>(slot)].used) return slot;
    }
    return from;
}

void SIDSynthProcessor::handleCommand(const juce::String& line) {
    const juce::String command = line.upToFirstOccurrenceOf(" ", false, false);
    const juce::String args = line.fromFirstOccurrenceOf(" ", false, false);

    if (command == "ping") {
        send("pong " + args);
    } else if (command == "audio") {
        // nur für das Vorhören im Browser der Box
    } else if (command == "set") {
        const int index = find_param(args.upToFirstOccurrenceOf(" ", false, false).toRawUTF8());
        if (index < 0) {
            notify("Unbekannter Parameter " + args.upToFirstOccurrenceOf(" ", false, false));
            return;
        }
        setParam(static_cast<size_t>(index), args.fromFirstOccurrenceOf(" ", false, false).getIntValue());
    } else if (command == "on") {
        const int note = args.getIntValue();
        const juce::String velocity = args.fromFirstOccurrenceOf(" ", false, false);
        pushCommand({CommandType::NoteOn, static_cast<uint8_t>(note & 0x7F),
                     static_cast<int16_t>((velocity.isEmpty() ? 100 : velocity.getIntValue()) & 0x7F)});
    } else if (command == "off") {
        pushCommand({CommandType::NoteOff, static_cast<uint8_t>(args.getIntValue() & 0x7F), 0});
    } else if (command == "name") {
        Patch patch;
        set_name(patch, args.toRawUTF8());
        name_ = juce::String::fromUTF8(patch.name);
        broadcastState();
    } else if (command == "load") {
        loadProgram(args.getIntValue());
    } else if (command == "next") {
        loadProgram(step(program_, 1));
    } else if (command == "prev") {
        loadProgram(step(program_, -1));
    } else if (command == "revert") {
        loadProgram(program_);
    } else if (command == "save") {
        saveProgram(args.isEmpty() ? program_ : args.getIntValue());
    } else if (command == "erase") {
        const int slot = args.getIntValue();
        if (slot < 0 || slot >= presets::kSlots || !own_[static_cast<size_t>(slot)]) return;
        const bool ok = presetFolder().getChildFile(juce::String::formatted("patch-%02d.json", slot)).deleteFile();
        loadSlot(slot);  // zeigt wieder den Werksklang oder ist leer
        broadcastState();
        notify(ok ? "Platz " + juce::String(slot + 1) + " gelöscht" : juce::String("Löschen fehlgeschlagen"));
    } else if (command == "init") {
        Patch patch;
        init_patch(patch, "Neuer Klang");
        applyPatch(patch);
        broadcastState();
    } else if (command == "panic") {
        pushCommand({CommandType::AllOff, 0, 0});
    } else if (command == "thru") {
        thru_ = args.getIntValue() != 0;
        broadcastState();
    } else if (command == "ezag") {
        ezag_ = args.getIntValue() != 0;
        broadcastState();
    } else if (command == "start") {
        start_ = args.getIntValue();
        broadcastState();
    } else if (command == "knob") {
        const int knob = args.getIntValue() - 1;
        const juce::String key = args.fromFirstOccurrenceOf(" ", false, false);
        if (knob >= 0 && knob < 3 && find_param(key.toRawUTF8()) >= 0) knobs_[static_cast<size_t>(knob)] = key;
        broadcastState();
    } else if (command == "state") {
        broadcastState();
    } else if (command.isNotEmpty()) {
        notify("Unbekannter Befehl " + command);
    }
}

// MARK: - Sicherung

juce::String SIDSynthProcessor::backupJson() const {
    juce::String out = "{\"fmt\":1,\"presets\":[";
    bool first = true;
    for (size_t slot = 0; slot < slots_.size(); ++slot) {
        if (!slots_[slot].used) continue;
        juce::var entry = patchToJson(slots_[slot].patch);
        auto* object = entry.getDynamicObject();
        juce::NamedValueSet values = object->getProperties();
        auto* ordered = new juce::DynamicObject();  // "slot" zuerst, wie auf der Box
        ordered->setProperty("slot", static_cast<int>(slot) + 1);
        for (const auto& property : values) ordered->setProperty(property.name, property.value);
        out << (first ? "" : ",") << juce::JSON::toString(juce::var(ordered), true);
        first = false;
    }
    return out + "]}";
}

/// Wie restore_presets() der Box: nur Plätze schreiben, die sich ändern; der Klang im Editor
/// folgt seinem Platz, außer er hat ungespeicherte Änderungen.
void SIDSynthProcessor::restorePresets(const juce::String& text) {
    const juce::var doc = juce::JSON::parse(text);
    const juce::var list = doc["presets"];
    if (!doc.isObject() || static_cast<int>(doc.getProperty("fmt", 1)) != 1 || !list.isArray()) {
        notify("Datei nicht lesbar – erwartet wird eine Sicherung aus „Alle Presets als Datei sichern“");
        return;
    }
    std::array<bool, presets::kSlots> found{};
    std::array<Patch, presets::kSlots> incoming{};
    int count = 0;
    for (const auto& entry : *list.getArray()) {
        const int slot = static_cast<int>(entry.getProperty("slot", 0)) - 1;
        if (slot < 0 || slot >= presets::kSlots || !entry["v"].isObject()) continue;
        patchFromJson(entry, incoming[static_cast<size_t>(slot)]);
        if (!found[static_cast<size_t>(slot)]) ++count;
        found[static_cast<size_t>(slot)] = true;  // kommt ein Platz zweimal vor, gilt der letzte
    }
    if (count == 0) {
        notify("Keine Presets in der Datei");
        return;
    }

    const bool edited = isDirty();
    const double start = juce::Time::getMillisecondCounterHiRes();
    int toWrite = 0, written = 0;
    bool changedCurrent = false;
    const juce::File folder = presetFolder();
    folder.createDirectory();
    for (int slot = 0; slot < presets::kSlots; ++slot) {
        const auto i = static_cast<size_t>(slot);
        if (!found[i] || (slots_[i].used && samePatch(slots_[i].patch, incoming[i]))) continue;
        ++toWrite;
        if (slot == program_) changedCurrent = true;
        const juce::File file = folder.getChildFile(juce::String::formatted("patch-%02d.json", slot));
        if (file.replaceWithText(juce::JSON::toString(patchToJson(incoming[i]), true), false, false, "\n")) {
            ++written;
            slots_[i].used = true;
            slots_[i].patch = incoming[i];
            own_[i] = true;
        }
    }

    if (changedCurrent && !edited) {
        loadProgram(program_);
    } else {
        broadcastState();
    }
    juce::String message = juce::String(written) + (written == 1 ? " Preset" : " Presets") + " geladen";
    if (count > toWrite) message << ", " << (count - toWrite) << " unverändert";
    if (written < toWrite) message << ", " << (toWrite - written) << " fehlgeschlagen";
    if (toWrite > 0) message << " (" << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - start) << " ms)";
    notify(message);
}
