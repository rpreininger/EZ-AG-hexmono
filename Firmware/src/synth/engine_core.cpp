// Die Klangrechnung, siehe engine_core.h.
//
// Pro Block (128 Samples, 2,9 ms):
//   1. Modulation rechnen — LFOs, Hüllkurve, Glide — und die SID-Register schreiben
//   2. reSID die 128 Samples rechnen lassen
//   3. Ein- und Ausblenden, Pegel messen
// Befehle — Parameter, Noten, Pitchbend — übernimmt die Hülle vorher mit apply().
//
// Die Modulation läuft damit wie auf der SidStation mit einigen hundert Hertz; schneller
// ändert ein echter SID seine Register auch nicht.

#include "engine_core.h"

#include <math.h>
#include <string.h>


namespace synth {
namespace core {
namespace {

using P = ParamId;

constexpr float kBlockSeconds = static_cast<float>(kBlock) / kSampleRate;
constexpr double kSidClockPal = 985248.0;
constexpr float kFreqScale = 16777216.0f / 985248.0f;  // Hz → SID-Frequenzregister
constexpr int32_t kUnity = 32768;
constexpr int32_t kFadeStep = kUnity / 220;  // 5 ms bei 44,1 kHz

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

// MARK: - Hilfen

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static_assert(reg::kCount == 0x19, "Registerzahl des SID");

}  // namespace

int16_t Core::v(ParamId id) const { return values_[static_cast<size_t>(id)]; }
float Core::value_of(ParamId id) const { return curve(param(id), v(id)); }

/// Schreibt nur, was sich geändert hat — reSID reagiert auf jeden Schreibzugriff.
void Core::write_reg(uint8_t address, uint8_t value) {
    if (shadow_valid_[address] && shadow_[address] == value) return;
    shadow_[address] = value;
    shadow_valid_[address] = true;
    sid_.write(address, value);
}

// MARK: - Noten

void Core::forget(uint8_t note) {
    Voice& voice = voice_;
    for (uint8_t i = 0; i < voice.held_count; ++i) {
        if (voice.held[i] != note) continue;
        memmove(&voice.held[i], &voice.held[i + 1], voice.held_count - i - 1);
        --voice.held_count;
        return;
    }
}

void Core::hold(uint8_t note) {
    Voice& voice = voice_;
    forget(note);
    if (voice.held_count == sizeof(voice.held)) {
        memmove(&voice.held[0], &voice.held[1], voice.held_count - 1);
        --voice.held_count;
    }
    voice.held[voice.held_count++] = note;
}

/// `attack`: neuer Anschlag mit Hüllkurve. Sonst nur die Tonhöhe wechseln (Legato).
void Core::play(uint8_t note, uint8_t velocity, bool attack) {
    Voice& voice = voice_;
    if (!voice.has_pitch || value_of(P::P_GLIDE) <= 0.0f) voice.pitch = note;
    voice.has_pitch = true;
    voice.note = note;
    if (!attack) return;
    voice.velocity = velocity;
    if (voice.gate) voice.retrigger = true;
    voice.gate = true;
    voice.since_note = 0;
    env_.stage = Stage::Attack;
}

void Core::press(uint8_t note, uint8_t velocity) {
    const bool legato = voice_.gate && v(P::P_LEGATO) != 0;
    hold(note);
    play(note, velocity, !legato);
}

void Core::release(uint8_t note) {
    Voice& voice = voice_;
    forget(note);
    if (!voice.gate || note != voice.note) return;
    if (voice.held_count > 0) {
        // Zurück zur Note, die noch gedrückt ist — ohne neuen Anschlag.
        play(voice.held[voice.held_count - 1], voice.velocity, false);
        return;
    }
    voice.gate = false;
    env_.stage = Stage::Release;
}

// MARK: - Modulation

/// Rechnet beide LFOs einen Block weiter. `scale` ist der Einsatz nach der
/// Verzögerung: 0 vor der Einsatzzeit, dann in 300 ms auf 1.
void Core::update_lfos(float scale[2]) {
    for (int i = 0; i < 2; ++i) {
        const LfoIds& ids = kLfo[i];
        Lfo& lfo = lfo_[i];
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
        scale[i] = delay <= 0.0f ? 1.0f : clampf((voice_.since_note - delay) / 0.3f, 0.0f, 1.0f);
    }
}

void Core::update_envelope() {
    Envelope& env = env_;
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

void Core::update_glide() {
    Voice& voice = voice_;
    const float time = value_of(P::P_GLIDE) / 1000.0f;
    if (time <= 0.0f) {
        voice.pitch = voice.note;
    } else {
        voice.pitch += (voice.note - voice.pitch) * (1.0f - expf(-3.0f * kBlockSeconds / time));
    }
    voice.since_note += kBlockSeconds;
}

void Core::write_registers(const float lfo_scale[2]) {
    Voice& voice = voice_;
    const float base = voice.pitch + v(P::P_TRANSP) + bend_ * v(P::P_BEND);
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
            const float mod = lfo_[l].value * lfo_scale[l];
            const int target = v(lfo.target);
            if (target == 0 || target == o + 1) semis += value_of(lfo.pitch) / 100.0f * mod;
            pw += v(lfo.pw) / 100.0f * 2048.0f * mod;
        }
        if (env_target == 0 || env_target == o + 1) semis += v(P::ME_PITCH) * env_.level;

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
    cutoff += v(P::F_ENV) / 100.0f * 2047.0f * env_.level;
    cutoff += v(P::F_KEY) / 100.0f * (voice.pitch - 60.0f) * 16.0f;
    cutoff += v(P::F_VEL) / 100.0f * (voice.velocity - 64) / 63.0f * 1024.0f;
    for (int l = 0; l < 2; ++l) {
        cutoff += v(kLfo[l].cut) / 100.0f * 1024.0f * lfo_[l].value * lfo_scale[l];
    }
    const long fc = lroundf(clampf(cutoff, 0.0f, 2047.0f));
    write_reg(reg::kFcLo, fc & 0x07);
    write_reg(reg::kFcHi, (fc >> 3) & 0xFF);

    // Ohne Filtertyp wären gefilterte Stimmen stumm — dann alle am Filter vorbei.
    if (mode == 0) routing = 0;
    write_reg(reg::kResFilt, static_cast<uint8_t>((v(P::F_RES) << 4) | routing));
    write_reg(reg::kModeVol, static_cast<uint8_t>(kModeBits[mode] | v(P::P_VOL)));
}

void Core::update_chip() {
    if (chip_ == v(P::P_CHIP)) return;
    chip_ = v(P::P_CHIP);
    sid_.set_chip_model(chip_ == 1 ? reSID::MOS8580 : reSID::MOS6581);
}

void Core::begin(const Patch& patch) {
    memcpy(values_, patch.values, sizeof(values_));
    chip_ = -1;
    memset(shadow_valid_, 0, sizeof(shadow_valid_));
    lfo_[0] = Lfo();
    lfo_[1] = Lfo();
    env_ = Envelope();
    voice_ = Voice();
    bend_ = 0;
    gain_ = 0;
    gain_target_ = kUnity;

    update_chip();
    sid_.set_sampling_parameters(kSidClockPal, reSID::SAMPLE_FAST, kSampleRate);
    sid_.reset();
}

void Core::apply(const Command& command) {
    switch (command.type) {
        case CommandType::SetParam:
            if (command.a < kParamCount) values_[command.a] = command.b;
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
            bend_ = command.b / 8192.0f;
            break;
        case CommandType::AllOff:
            voice_.held_count = 0;
            if (voice_.gate) {
                voice_.gate = false;
                env_.stage = Stage::Release;
            }
            break;
        case CommandType::Mute:
            gain_target_ = command.a != 0 ? 0 : kUnity;
            break;
    }
}

int32_t Core::render_block(int16_t out[kBlock]) {
    update_chip();
    float lfo_scale[2];
    update_lfos(lfo_scale);
    update_envelope();
    update_glide();
    write_registers(lfo_scale);

    reSID::cycle_count delta = kBlock * 32;  // mehr als nötig: endet nach kBlock Samples
    sid_.clock(delta, out, kBlock);

    int32_t peak = 0;
    for (int s = 0; s < kBlock; ++s) {
        if (gain_ < gain_target_) {
            gain_ = gain_ + kFadeStep > gain_target_ ? gain_target_ : gain_ + kFadeStep;
        } else if (gain_ > gain_target_) {
            gain_ = gain_ - kFadeStep < gain_target_ ? gain_target_ : gain_ - kFadeStep;
        }
        const int32_t sample = (static_cast<int32_t>(out[s]) * gain_) >> 15;
        const int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
        out[s] = static_cast<int16_t>(sample);
    }
    return peak;
}

bool Core::silent() const { return gain_ == 0 && gain_target_ == 0; }

}  // namespace core
}  // namespace synth
