// Die Hülle um die Klangerzeugung der Box, siehe sid_host.h.
//
// Was auf der Box engine.cpp ist — Warteschlange, Blockschleife, Lastmessung —, steht
// hier für den Audio-Callback von macOS. Die Rechnung selbst kommt unverändert aus
// Firmware/src/synth/engine_core.cpp.

#include "sid_host.h"

#include <mach/mach_time.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>

#include "engine_core.h"
#include "ezag_config.h"
#include "factory.h"
#include "midi_filter.h"
#include "params.h"
#include "schema.h"

namespace {

using namespace synth;
using core::Command;
using core::CommandType;
using core::kBlock;

// MARK: - Warteschlange
//
// Mehrere Schreiber (Hauptthread, MIDI-Thread), ein Leser (Audio-Callback). Die
// Schreiber stellen sich untereinander an; der Leser wartet nie auf ein Lock.

class CommandRing {
public:
    static constexpr uint32_t kSize = 256;  // wie die Warteschlange der Box

    bool push(const Command& command) {
        std::lock_guard<std::mutex> lock(writers_);
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kSize) return false;
        items_[head % kSize] = command;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(Command& command) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        command = items_[tail % kSize];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    Command items_[kSize] = {};
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::mutex writers_;
};

CommandRing g_commands;
std::atomic<uint32_t> g_dropped{0};

void push(CommandType type, uint8_t a, int16_t b) {
    if (!g_commands.push(Command{type, a, b})) g_dropped.fetch_add(1);
}

// MARK: - Zustand des Audio-Callbacks

int16_t g_block[kBlock];
int g_position = kBlock;  // kBlock = nichts mehr übrig, nächster Block fällig

double g_block_ns = 0;  // so lange dauert ein Block in Echtzeit
double g_ticks_to_ns = 1;
uint32_t g_blocks = 0;
double g_ns_sum = 0;
double g_ns_max = 0;
int32_t g_peak = 0;

std::atomic<float> g_load_avg{0};
std::atomic<float> g_load_max{0};
std::atomic<int32_t> g_peak_second{0};
std::atomic<int32_t> g_peak_hold{0};

void render_one_block() {
    const uint64_t t0 = mach_absolute_time();
    Command command;
    while (g_commands.pop(command)) core::apply(command);
    const int32_t peak = core::render_block(g_block);
    const double ns = (mach_absolute_time() - t0) * g_ticks_to_ns;

    if (peak > g_peak) g_peak = peak;
    if (peak > g_peak_hold.load(std::memory_order_relaxed)) g_peak_hold.store(peak);
    ++g_blocks;
    g_ns_sum += ns;
    if (ns > g_ns_max) g_ns_max = ns;
    if (g_blocks >= kSampleRate / kBlock) {
        g_load_avg.store(static_cast<float>(100.0 * g_ns_sum / g_blocks / g_block_ns));
        g_load_max.store(static_cast<float>(100.0 * g_ns_max / g_block_ns));
        g_peak_second.store(g_peak > 32767 ? 32767 : g_peak);
        g_blocks = 0;
        g_ns_sum = 0;
        g_ns_max = 0;
        g_peak = 0;
    }
}

// MARK: - MIDI

std::mutex g_midi_mutex;  // Filter und Schalter, MIDI-Thread gegen Hauptthread
midifilter::Filter g_filter(ezag_config());
bool g_ezag = false;
std::atomic<uint32_t> g_midi_in{0};

uint32_t now_us() {
    return static_cast<uint32_t>(mach_absolute_time() * g_ticks_to_ns / 1000.0);
}

/// Wie play() in synth_main.cpp. Liefert die Nummer eines Program Change, sonst -1.
int play(const midifilter::Event& event) {
    switch (event.kind) {
        case midifilter::MessageKind::NoteOn:
            if (event.velocity() == 0) {
                sid_note_off(event.note());
            } else {
                sid_note_on(event.note(), event.velocity());
            }
            break;
        case midifilter::MessageKind::NoteOff:
            sid_note_off(event.note());
            break;
        case midifilter::MessageKind::PitchBend:
            sid_pitch_bend(((event.data2 << 7) | event.data1) - 8192);
            break;
        case midifilter::MessageKind::ProgramChange:
            return event.data1;
        case midifilter::MessageKind::ControlChange:
            if (event.data1 == 120 || event.data1 == 123) sid_all_notes_off();
            break;
        default:
            break;
    }
    return -1;
}

struct StringOut {
    std::string text;
    void print(const char* s) { text += s; }
    void print(char c) { text += c; }
};

const Param* param_at(int index) {
    return index >= 0 && static_cast<size_t>(index) < kParamCount ? &kParams[index] : nullptr;
}

struct Clock {
    Clock() {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        g_ticks_to_ns = static_cast<double>(info.numer) / info.denom;
        g_block_ns = 1e9 * kBlock / kSampleRate;
    }
};
const Clock g_clock;

}  // namespace

// MARK: - Parametertabelle

int sid_param_count(void) { return static_cast<int>(kParamCount); }

const char* sid_param_key(int index) {
    const Param* p = param_at(index);
    return p != nullptr ? p->key : "";
}

int sid_param_default(int index) {
    const Param* p = param_at(index);
    return p != nullptr ? p->def : 0;
}

int sid_find_param(const char* key) { return find_param(key); }

int sid_clamp_value(int index, long value) {
    return param_at(index) != nullptr ? clamp_value(static_cast<size_t>(index), value) : 0;
}

int sid_param_min(int index) {
    const Param* p = param_at(index);
    return p != nullptr ? p->min : 0;
}

int sid_param_max(int index) {
    const Param* p = param_at(index);
    return p != nullptr ? p->max : 0;
}

bool sid_param_is_toggle(int index) {
    const Param* p = param_at(index);
    return p != nullptr && p->kind == Kind::Toggle;
}

int sid_slots(void) { return presets::kSlots; }

int sid_name_capacity(void) { return static_cast<int>(kNameLength); }

void sid_set_name(const char* text, char* out) {
    Patch patch;
    set_name(patch, text);
    memcpy(out, patch.name, kNameLength);
}

bool sid_factory(int slot, char* name, int16_t* values) {
    Patch patch;
    if (!presets::factory(slot, patch)) return false;
    memcpy(name, patch.name, kNameLength);
    memcpy(values, patch.values, sizeof(patch.values));
    return true;
}

const char* sid_schema_json(void) {
    static const std::string schema = [] {
        StringOut out;
        write_schema(out);
        return out.text;
    }();
    return schema.c_str();
}

// MARK: - Klangerzeugung

void sid_engine_begin(const int16_t* values) {
    Patch patch;
    init_patch(patch, "");
    for (size_t i = 0; i < kParamCount; ++i) patch.values[i] = clamp_value(i, values[i]);
    Command command;
    while (g_commands.pop(command)) {
    }
    core::begin(patch);
    g_position = kBlock;
}

void sid_set_param(int index, int value) {
    if (param_at(index) == nullptr) return;
    push(CommandType::SetParam, static_cast<uint8_t>(index),
         clamp_value(static_cast<size_t>(index), value));
}

void sid_load(const int16_t* values) {
    for (size_t i = 0; i < kParamCount; ++i) sid_set_param(static_cast<int>(i), values[i]);
}

void sid_note_on(int note, int velocity) {
    push(CommandType::NoteOn, note & 0x7F, static_cast<int16_t>(velocity & 0x7F));
}

void sid_note_off(int note) { push(CommandType::NoteOff, note & 0x7F, 0); }

void sid_pitch_bend(int value) {
    if (value < -8192) value = -8192;
    if (value > 8191) value = 8191;
    push(CommandType::Bend, 0, static_cast<int16_t>(value));
}

void sid_all_notes_off(void) { push(CommandType::AllOff, 0, 0); }

void sid_render(float* out, int frames) {
    for (int i = 0; i < frames; ++i) {
        if (g_position == kBlock) {
            render_one_block();
            g_position = 0;
        }
        out[i] = g_block[g_position++] / 32768.0f;
    }
}

SIDStats sid_stats(void) {
    SIDStats stats;
    stats.load_avg = g_load_avg.load();
    stats.load_max = g_load_max.load();
    stats.peak = g_peak_second.load();
    stats.dropped = g_dropped.load();
    stats.midi_in = g_midi_in.load();
    return stats;
}

int sid_take_peak(void) {
    const int32_t peak = g_peak_hold.exchange(0);
    return peak > 32767 ? 32767 : peak;
}

// MARK: - MIDI

int sid_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    if (status < 0x80 || status >= 0xF0) return -1;
    midifilter::Event event;
    event.channel = status & 0x0F;
    event.kind = static_cast<midifilter::MessageKind>(((status >> 4) & 0x07));
    event.data1 = data1 & 0x7F;
    event.data2 = data2 & 0x7F;
    event.time_us = now_us();
    g_midi_in.fetch_add(1);

    std::lock_guard<std::mutex> lock(g_midi_mutex);
    if (!g_ezag) return play(event);
    int program = -1;
    const midifilter::Result result = g_filter.process(event);
    for (uint8_t i = 0; i < result.count; ++i) {
        const int p = play(result.events[i]);
        if (p >= 0) program = p;
    }
    return program;
}

void sid_set_ezag(bool on) {
    std::lock_guard<std::mutex> lock(g_midi_mutex);
    // Beim Umschalten nichts hängen lassen: der Filter kennt die alten Noten nicht.
    sid_all_notes_off();
    g_filter.reset();
    g_ezag = on;
}
