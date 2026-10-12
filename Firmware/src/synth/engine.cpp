// Die Klangerzeugung, siehe engine.h.
//
// Pro Block (128 Samples, 2,9 ms) auf Kern 1:
//   1. Befehle aus der Warteschlange übernehmen — Parameter, Noten, Pitchbend
//   2. Modulation rechnen — LFOs, Hüllkurve, Glide — und die SID-Register schreiben
//   3. reSID die 128 Samples rechnen lassen
//   4. Ein- und Ausblenden, Pegel messen, an I2S und ans Mithören geben
//
// Die Modulation läuft damit wie auf der SidStation mit einigen hundert Hertz; schneller
// ändert ein echter SID seine Register auch nicht.

#include "engine.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>
#include <string.h>

#include "sid.h"

namespace synth {

void PreviewRing::write(const int16_t* samples, int count) {
    uint32_t w = write_.load(std::memory_order_relaxed);
    for (int s = 0; s + 1 < count; s += 2) {
        data_[w++ % kSize] = static_cast<int16_t>((samples[s] + samples[s + 1]) / 2);
    }
    write_.store(w, std::memory_order_release);
}

namespace engine {
namespace {

using P = ParamId;

constexpr int kBlock = 128;
constexpr int kDmaBuffers = 4;  // 4 × 128 Samples = 11,6 ms Vorrat
constexpr float kBlockSeconds = static_cast<float>(kBlock) / kSampleRate;
constexpr double kSidClockPal = 985248.0;
constexpr float kFreqScale = 16777216.0f / 985248.0f;  // Hz → SID-Frequenzregister
constexpr int32_t kUnity = 32768;
constexpr int32_t kFadeStep = kUnity / 220;  // 5 ms bei 44,1 kHz

enum class CommandType : uint8_t { SetParam, NoteOn, NoteOff, Bend, AllOff, Mute };

struct Command {
    CommandType type;
    uint8_t a;
    int16_t b;
};

namespace reg {
constexpr uint8_t kFreqLo = 0x00, kFreqHi = 0x01, kPwLo = 0x02, kPwHi = 0x03;
constexpr uint8_t kControl = 0x04, kAttackDecay = 0x05, kSustainRelease = 0x06;
constexpr uint8_t kFcLo = 0x15, kFcHi = 0x16, kResFilt = 0x17, kModeVol = 0x18;
constexpr uint8_t kCount = 0x19;
}  // namespace reg

// Steuerbits je Wellenform, in der Reihenfolge von SYNTH_WAVES.
constexpr uint8_t kWaveBits[] = {0x00, 0x10, 0x20, 0x40, 0x80, 0x30, 0x50, 0x60, 0x70};
// Filtertypen in der Reihenfolge der Auswahl f.mode.
constexpr uint8_t kModeBits[] = {0x00, 0x10, 0x20, 0x40, 0x50, 0x30, 0x60, 0x70};
constexpr uint8_t kGate = 0x01, kSync = 0x02, kRing = 0x04;

struct OscIds {
    P wave, semi, fine, pw, a, d, s, r, sync, ring, filt;
};
constexpr OscIds kOsc[3] = {
    {P::O1_WAVE, P::O1_SEMI, P::O1_FINE, P::O1_PW, P::O1_A, P::O1_D, P::O1_S, P::O1_R,
     P::O1_SYNC, P::O1_RING, P::O1_FILT},
    {P::O2_WAVE, P::O2_SEMI, P::O2_FINE, P::O2_PW, P::O2_A, P::O2_D, P::O2_S, P::O2_R,
     P::O2_SYNC, P::O2_RING, P::O2_FILT},
    {P::O3_WAVE, P::O3_SEMI, P::O3_FINE, P::O3_PW, P::O3_A, P::O3_D, P::O3_S, P::O3_R,
     P::O3_SYNC, P::O3_RING, P::O3_FILT},
};

struct LfoIds {
    P wave, rate, delay, pitch, target, pw, cut;
};
constexpr LfoIds kLfo[2] = {
    {P::L1_WAVE, P::L1_RATE, P::L1_DELAY, P::L1_PITCH, P::L1_PTGT, P::L1_PW, P::L1_CUT},
    {P::L2_WAVE, P::L2_RATE, P::L2_DELAY, P::L2_PITCH, P::L2_PTGT, P::L2_PW, P::L2_CUT},
};

// MARK: - Zustand der Audio-Aufgabe, nur sie fasst das an

reSID::SID g_sid;
int16_t g_values[kParamCount];
int g_chip = -1;
uint8_t g_shadow[reg::kCount];
bool g_shadow_valid[reg::kCount];

struct Lfo {
    float phase = 0;
    float value = 0;
    uint32_t random = 0x9E3779B9;
};
Lfo g_lfo[2];

enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
struct Envelope {
    Stage stage = Stage::Idle;
    float level = 0;
};
Envelope g_env;

struct Voice {
    uint8_t held[8] = {};  // gedrückte Noten, die letzte oben
    uint8_t held_count = 0;
    uint8_t note = 60;
    uint8_t velocity = 100;
    bool gate = false;
    bool retrigger = false;  // einen Block Gate aus, damit die SID-Hüllkurve neu startet
    bool has_pitch = false;  // vor der ersten Note gibt es nichts, von dem man gleitet
    float pitch = 60;        // klingende Tonhöhe in Halbtönen, gleitet zur Note
    float since_note = 0;    // Sekunden seit dem Anschlag, für den LFO-Einsatz
};
Voice g_voice;
float g_bend = 0;  // -1..1

int32_t g_gain = 0;
int32_t g_gain_target = kUnity;

// MARK: - Geteilt mit den anderen Aufgaben

QueueHandle_t g_commands = nullptr;
PreviewRing g_preview;
std::atomic<bool> g_silent{false};
std::atomic<uint32_t> g_dropped{0};
std::atomic<int32_t> g_peak_hold{0};
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
EngineStats g_stats;

// MARK: - Hilfen

inline int16_t v(P id) { return g_values[static_cast<size_t>(id)]; }
inline float value_of(P id) { return curve(param(id), v(id)); }

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

void push(CommandType type, uint8_t a, int16_t b) {
    if (g_commands == nullptr) return;
    const Command command{type, a, b};
    if (xQueueSend(g_commands, &command, 0) != pdTRUE) g_dropped.fetch_add(1);
}

/// Schreibt nur, was sich geändert hat — reSID reagiert auf jeden Schreibzugriff.
void write_reg(uint8_t address, uint8_t value) {
    if (g_shadow_valid[address] && g_shadow[address] == value) return;
    g_shadow[address] = value;
    g_shadow_valid[address] = true;
    g_sid.write(address, value);
}

// MARK: - Noten

void forget(uint8_t note) {
    Voice& voice = g_voice;
    for (uint8_t i = 0; i < voice.held_count; ++i) {
        if (voice.held[i] != note) continue;
        memmove(&voice.held[i], &voice.held[i + 1], voice.held_count - i - 1);
        --voice.held_count;
        return;
    }
}

void hold(uint8_t note) {
    Voice& voice = g_voice;
    forget(note);
    if (voice.held_count == sizeof(voice.held)) {
        memmove(&voice.held[0], &voice.held[1], voice.held_count - 1);
        --voice.held_count;
    }
    voice.held[voice.held_count++] = note;
}

/// `attack`: neuer Anschlag mit Hüllkurve. Sonst nur die Tonhöhe wechseln (Legato).
void play(uint8_t note, uint8_t velocity, bool attack) {
    Voice& voice = g_voice;
    if (!voice.has_pitch || value_of(P::P_GLIDE) <= 0.0f) voice.pitch = note;
    voice.has_pitch = true;
    voice.note = note;
    if (!attack) return;
    voice.velocity = velocity;
    if (voice.gate) voice.retrigger = true;
    voice.gate = true;
    voice.since_note = 0;
    g_env.stage = Stage::Attack;
}

void press(uint8_t note, uint8_t velocity) {
    const bool legato = g_voice.gate && v(P::P_LEGATO) != 0;
    hold(note);
    play(note, velocity, !legato);
}

void release(uint8_t note) {
    Voice& voice = g_voice;
    forget(note);
    if (!voice.gate || note != voice.note) return;
    if (voice.held_count > 0) {
        // Zurück zur Note, die noch gedrückt ist — ohne neuen Anschlag.
        play(voice.held[voice.held_count - 1], voice.velocity, false);
        return;
    }
    voice.gate = false;
    g_env.stage = Stage::Release;
}

void apply(const Command& command) {
    switch (command.type) {
        case CommandType::SetParam:
            if (command.a < kParamCount) g_values[command.a] = command.b;
            break;
        case CommandType::NoteOn:
            if (command.b == 0) {
                release(command.a);
            } else {
                press(command.a, static_cast<uint8_t>(command.b));
            }
            break;
        case CommandType::NoteOff:
            release(command.a);
            break;
        case CommandType::Bend:
            g_bend = command.b / 8192.0f;
            break;
        case CommandType::AllOff:
            g_voice.held_count = 0;
            if (g_voice.gate) {
                g_voice.gate = false;
                g_env.stage = Stage::Release;
            }
            break;
        case CommandType::Mute:
            g_gain_target = command.a != 0 ? 0 : kUnity;
            break;
    }
}

// MARK: - Modulation

/// Rechnet beide LFOs einen Block weiter. `scale` ist der Einsatz nach der
/// Verzögerung: 0 vor der Einsatzzeit, dann in 300 ms auf 1.
void update_lfos(float scale[2]) {
    for (int i = 0; i < 2; ++i) {
        const LfoIds& ids = kLfo[i];
        Lfo& lfo = g_lfo[i];
        lfo.phase += value_of(ids.rate) * kBlockSeconds;
        const bool wrapped = lfo.phase >= 1.0f;
        if (wrapped) lfo.phase -= floorf(lfo.phase);
        switch (v(ids.wave)) {
            case 0:  // Dreieck
                lfo.value = 1.0f - 4.0f * fabsf(lfo.phase - 0.5f);
                break;
            case 1:  // Sägezahn
                lfo.value = 2.0f * lfo.phase - 1.0f;
                break;
            case 2:  // Rechteck
                lfo.value = lfo.phase < 0.5f ? 1.0f : -1.0f;
                break;
            default:  // Zufall: ein neuer Wert je Periode
                if (wrapped) {
                    lfo.random ^= lfo.random << 13;
                    lfo.random ^= lfo.random >> 17;
                    lfo.random ^= lfo.random << 5;
                    lfo.value = lfo.random / 2147483648.0f - 1.0f;
                }
                break;
        }
        const float delay = value_of(ids.delay) / 1000.0f;
        scale[i] = delay <= 0.0f ? 1.0f : clampf((g_voice.since_note - delay) / 0.3f, 0.0f, 1.0f);
    }
}

void update_envelope() {
    Envelope& env = g_env;
    const float sustain = v(P::ME_S) / 100.0f;
    switch (env.stage) {
        case Stage::Attack: {
            const float time = value_of(P::ME_A) / 1000.0f;
            env.level = time <= 0.0f ? 1.0f : env.level + kBlockSeconds / time;
            if (env.level >= 1.0f) {
                env.level = 1.0f;
                env.stage = Stage::Decay;
            }
            break;
        }
        case Stage::Decay: {
            const float time = value_of(P::ME_D) / 1000.0f;
            env.level = time <= 0.0f
                            ? sustain
                            : sustain + (env.level - sustain) * expf(-3.0f * kBlockSeconds / time);
            if (fabsf(env.level - sustain) < 0.001f) {
                env.level = sustain;
                env.stage = Stage::Sustain;
            }
            break;
        }
        case Stage::Sustain:
            env.level = sustain;
            break;
        case Stage::Release: {
            const float time = value_of(P::ME_R) / 1000.0f;
            env.level = time <= 0.0f ? 0.0f : env.level * expf(-3.0f * kBlockSeconds / time);
            if (env.level < 0.0005f) {
                env.level = 0.0f;
                env.stage = Stage::Idle;
            }
            break;
        }
        case Stage::Idle:
            break;
    }
}

void update_glide() {
    Voice& voice = g_voice;
    const float time = value_of(P::P_GLIDE) / 1000.0f;
    if (time <= 0.0f) {
        voice.pitch = voice.note;
    } else {
        voice.pitch += (voice.note - voice.pitch) * (1.0f - expf(-3.0f * kBlockSeconds / time));
    }
    voice.since_note += kBlockSeconds;
}

void write_registers(const float lfo_scale[2]) {
    Voice& voice = g_voice;
    const float base = voice.pitch + v(P::P_TRANSP) + g_bend * v(P::P_BEND);
    const bool gate = voice.gate && !voice.retrigger;
    const int mode = v(P::F_MODE);
    const int env_target = v(P::ME_PTGT);
    uint8_t routing = 0;

    for (int o = 0; o < 3; ++o) {
        const OscIds& ids = kOsc[o];
        float semis = base + v(ids.semi) + v(ids.fine) / 100.0f;
        float pw = v(ids.pw);
        for (int l = 0; l < 2; ++l) {
            const LfoIds& lfo = kLfo[l];
            const float mod = g_lfo[l].value * lfo_scale[l];
            const int target = v(lfo.target);
            if (target == 0 || target == o + 1) semis += value_of(lfo.pitch) / 100.0f * mod;
            pw += v(lfo.pw) / 100.0f * 2048.0f * mod;
        }
        if (env_target == 0 || env_target == o + 1) semis += v(P::ME_PITCH) * g_env.level;

        const float hz = 440.0f * exp2f((semis - 69.0f) / 12.0f);
        const long freq = lroundf(clampf(hz * kFreqScale, 0.0f, 65535.0f));
        const long pulse = lroundf(clampf(pw, 8.0f, 4087.0f));

        const uint8_t wave = kWaveBits[v(ids.wave)];
        uint8_t control = wave;
        if (v(ids.sync) != 0) control |= kSync;
        if (v(ids.ring) != 0) control |= kRing;
        if (gate && wave != 0) control |= kGate;

        const uint8_t at = static_cast<uint8_t>(o * 7);
        write_reg(at + reg::kFreqLo, freq & 0xFF);
        write_reg(at + reg::kFreqHi, (freq >> 8) & 0xFF);
        write_reg(at + reg::kPwLo, pulse & 0xFF);
        write_reg(at + reg::kPwHi, (pulse >> 8) & 0x0F);
        write_reg(at + reg::kAttackDecay, static_cast<uint8_t>((v(ids.a) << 4) | v(ids.d)));
        write_reg(at + reg::kSustainRelease, static_cast<uint8_t>((v(ids.s) << 4) | v(ids.r)));
        write_reg(at + reg::kControl, control);
        if (v(ids.filt) != 0) routing |= static_cast<uint8_t>(1 << o);
    }
    voice.retrigger = false;

    float cutoff = v(P::F_CUT);
    cutoff += v(P::F_ENV) / 100.0f * 2047.0f * g_env.level;
    cutoff += v(P::F_KEY) / 100.0f * (voice.pitch - 60.0f) * 16.0f;
    cutoff += v(P::F_VEL) / 100.0f * (voice.velocity - 64) / 63.0f * 1024.0f;
    for (int l = 0; l < 2; ++l) {
        cutoff += v(kLfo[l].cut) / 100.0f * 1024.0f * g_lfo[l].value * lfo_scale[l];
    }
    const long fc = lroundf(clampf(cutoff, 0.0f, 2047.0f));
    write_reg(reg::kFcLo, fc & 0x07);
    write_reg(reg::kFcHi, (fc >> 3) & 0xFF);

    // Ohne Filtertyp wären gefilterte Stimmen stumm — dann alle am Filter vorbei.
    if (mode == 0) routing = 0;
    write_reg(reg::kResFilt, static_cast<uint8_t>((v(P::F_RES) << 4) | routing));
    write_reg(reg::kModeVol, static_cast<uint8_t>(kModeBits[mode] | v(P::P_VOL)));
}

void update_chip() {
    if (g_chip == v(P::P_CHIP)) return;
    g_chip = v(P::P_CHIP);
    g_sid.set_chip_model(g_chip == 1 ? reSID::MOS8580 : reSID::MOS6581);
}

// MARK: - Audio-Aufgabe

void i2s_begin() {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = kDmaBuffers;
    config.dma_buf_len = kBlock;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;  // bei Aussetzern Stille statt Wiederholung
    i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = kI2sBckPin;
    pins.ws_io_num = kI2sWsPin;
    pins.data_out_num = kI2sDoutPin;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
}

void audio_task(void*) {
    static short mono[kBlock];
    static int16_t frame[kBlock * 2];

    const double block_cycles = getCpuFrequencyMhz() * 1e6 * kBlock / kSampleRate;
    const uint32_t blocks_per_second = kSampleRate / kBlock;
    const uint32_t start_ms = millis();
    bool settled = false;  // die Lücken beim Anlaufen zählen nicht

    uint32_t blocks = 0;
    uint64_t cycles_sum = 0;
    uint32_t cycles_max = 0;
    uint32_t gap_max = 0;
    uint32_t worst_gap = 0;
    int32_t peak = 0;
    uint32_t last_us = micros();

    for (;;) {
        const uint32_t now_us = micros();
        const uint32_t gap = now_us - last_us;
        last_us = now_us;
        if (settled && gap > gap_max) gap_max = gap;

        const uint32_t t0 = ESP.getCycleCount();
        Command command;
        while (xQueueReceive(g_commands, &command, 0) == pdTRUE) apply(command);
        update_chip();
        float lfo_scale[2];
        update_lfos(lfo_scale);
        update_envelope();
        update_glide();
        write_registers(lfo_scale);

        reSID::cycle_count delta = kBlock * 32;  // mehr als nötig: endet nach kBlock Samples
        g_sid.clock(delta, mono, kBlock);

        for (int s = 0; s < kBlock; ++s) {
            if (g_gain < g_gain_target) {
                g_gain = g_gain + kFadeStep > g_gain_target ? g_gain_target : g_gain + kFadeStep;
            } else if (g_gain > g_gain_target) {
                g_gain = g_gain - kFadeStep < g_gain_target ? g_gain_target : g_gain - kFadeStep;
            }
            const int32_t out = (static_cast<int32_t>(mono[s]) * g_gain) >> 15;
            const int32_t magnitude = out < 0 ? -out : out;
            if (magnitude > peak) peak = magnitude;
            mono[s] = static_cast<short>(out);
            frame[2 * s] = static_cast<int16_t>(out);
            frame[2 * s + 1] = static_cast<int16_t>(out);
        }
        g_silent.store(g_gain == 0 && g_gain_target == 0);
        if (peak > g_peak_hold.load(std::memory_order_relaxed)) g_peak_hold.store(peak);
        const uint32_t cycles = ESP.getCycleCount() - t0;

        if (g_preview.enabled()) g_preview.write(mono, kBlock);

        size_t written = 0;
        i2s_write(I2S_NUM_0, frame, sizeof(frame), &written, portMAX_DELAY);
        if (!settled && millis() - start_ms > 1000) settled = true;

        ++blocks;
        cycles_sum += cycles;
        if (cycles > cycles_max) cycles_max = cycles;
        if (blocks >= blocks_per_second) {
            if (gap_max > worst_gap) worst_gap = gap_max;
            EngineStats stats;
            stats.load_avg = static_cast<float>(100.0 * cycles_sum / blocks / block_cycles);
            stats.load_max = static_cast<float>(100.0 * cycles_max / block_cycles);
            stats.max_gap_us = gap_max;
            stats.worst_gap_us = worst_gap;
            stats.peak = static_cast<int16_t>(peak > 32767 ? 32767 : peak);
            stats.dropped_commands = g_dropped.load();
            portENTER_CRITICAL(&g_stats_mux);
            g_stats = stats;
            portEXIT_CRITICAL(&g_stats_mux);
            blocks = 0;
            cycles_sum = 0;
            cycles_max = 0;
            gap_max = 0;
            peak = 0;
        }
    }
}

}  // namespace

void begin(const Patch& patch) {
    memcpy(g_values, patch.values, sizeof(g_values));
    g_commands = xQueueCreate(256, sizeof(Command));

    update_chip();
    g_sid.set_sampling_parameters(kSidClockPal, reSID::SAMPLE_FAST, kSampleRate);
    g_sid.reset();

    i2s_begin();
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 18, nullptr, 1);
}

void set_param(size_t index, int16_t value) {
    if (index < kParamCount) {
        push(CommandType::SetParam, static_cast<uint8_t>(index), clamp_value(index, value));
    }
}

void set_param(ParamId id, int16_t value) { set_param(static_cast<size_t>(id), value); }

void load(const Patch& patch) {
    for (size_t i = 0; i < kParamCount; ++i) set_param(i, patch.values[i]);
}

void note_on(uint8_t note, uint8_t velocity) {
    push(CommandType::NoteOn, note & 0x7F, velocity & 0x7F);
}

void note_off(uint8_t note) { push(CommandType::NoteOff, note & 0x7F, 0); }

void pitch_bend(int value) {
    if (value < -8192) value = -8192;
    if (value > 8191) value = 8191;
    push(CommandType::Bend, 0, static_cast<int16_t>(value));
}

void all_notes_off() { push(CommandType::AllOff, 0, 0); }

void set_muted(bool muted) { push(CommandType::Mute, muted ? 1 : 0, 0); }

bool silent() { return g_silent.load(); }

EngineStats stats() {
    portENTER_CRITICAL(&g_stats_mux);
    const EngineStats stats = g_stats;
    portEXIT_CRITICAL(&g_stats_mux);
    return stats;
}

int16_t take_peak() {
    const int32_t peak = g_peak_hold.exchange(0);
    return static_cast<int16_t>(peak > 32767 ? 32767 : peak);
}

PreviewRing& preview() { return g_preview; }

}  // namespace engine
}  // namespace synth
