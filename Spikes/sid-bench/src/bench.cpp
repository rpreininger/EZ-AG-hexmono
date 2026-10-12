// Messfirmware: wie viel Rechenzeit braucht reSID 0.16 auf dem ESP32-S3?
//
// Spielt ein Arpeggio auf allen drei Stimmen, alle durch das Filter, mit Resonanz,
// Cutoff-Sweep und Pulsbreitenmodulation — also den teuersten Fall, den ein Patch
// erzeugen kann. Für jede Einstellung wird gemessen, wie lange das Berechnen eines
// Audioblocks dauert, im Verhältnis zu der Zeit, die dieser Block später klingt.
// 100 % hiesse: ein Kern schafft es gerade so in Echtzeit.
//
// Die Ausgabe geht per I2S an GPIO 12/13/14 (BCK/WS/DOUT). Ohne DAC läuft der Takt
// trotzdem und gibt das Tempo vor; mit einem PCM5102A hört man das Arpeggio.

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_chip_info.h>
#include <math.h>

#include "sid.h"

namespace {

// MARK: - Verdrahtung

constexpr int kI2sBckPin = 12;
constexpr int kI2sWsPin = 13;
constexpr int kI2sDoutPin = 14;

// MARK: - Messaufbau

constexpr int kBlock = 128;                   // Samples pro Block und Kanal
constexpr double kSidClockPal = 985248.0;     // C64 PAL
constexpr uint32_t kWarmupMs = 500;
constexpr uint32_t kMeasureMs = 3000;

struct Scenario {
    const char* name;
    reSID::chip_model model;
    reSID::sampling_method method;
    uint32_t rate;
    uint8_t instances;
};

constexpr Scenario kScenarios[] = {
    {"1x 6581 FAST         44,1 kHz", reSID::MOS6581, reSID::SAMPLE_FAST, 44100, 1},
    {"1x 8580 FAST         44,1 kHz", reSID::MOS8580, reSID::SAMPLE_FAST, 44100, 1},
    {"1x 6581 FAST         48   kHz", reSID::MOS6581, reSID::SAMPLE_FAST, 48000, 1},
    {"1x 6581 FAST         96   kHz", reSID::MOS6581, reSID::SAMPLE_FAST, 96000, 1},
    {"1x 6581 INTERPOLATE  44,1 kHz", reSID::MOS6581, reSID::SAMPLE_INTERPOLATE, 44100, 1},
    {"1x 8580 INTERPOLATE  48   kHz", reSID::MOS8580, reSID::SAMPLE_INTERPOLATE, 48000, 1},
    {"2x 6581 FAST         44,1 kHz", reSID::MOS6581, reSID::SAMPLE_FAST, 44100, 2},
    {"2x 6581 FAST         96   kHz", reSID::MOS6581, reSID::SAMPLE_FAST, 96000, 2},
    {"2x 6581 INTERPOLATE  44,1 kHz", reSID::MOS6581, reSID::SAMPLE_INTERPOLATE, 44100, 2},
};
constexpr int kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);

struct Measurement {
    uint32_t blocks = 0;
    uint64_t render_cycles = 0;
    uint32_t max_block_cycles = 0;
    uint32_t short_blocks = 0;  // clock() lieferte weniger Samples als verlangt
    int16_t peak = 0;           // Beweis, dass wirklich Klang entsteht
};

// MARK: - Zustand

reSID::SID g_sid[2];
uint16_t g_note_freq[128];

Measurement g_results[kScenarioCount];
volatile int g_finished = -1;  // Index des zuletzt fertig gemessenen Szenarios
volatile uint32_t g_round = 0;
UBaseType_t g_audio_stack_free = 0;

// MARK: - SID-Register

namespace reg {
constexpr uint8_t kFreqLo = 0x00, kFreqHi = 0x01, kPwLo = 0x02, kPwHi = 0x03;
constexpr uint8_t kControl = 0x04, kAttackDecay = 0x05, kSustainRelease = 0x06;
constexpr uint8_t kFcLo = 0x15, kFcHi = 0x16, kResFilt = 0x17, kModeVol = 0x18;
}  // namespace reg

constexpr uint8_t kGate = 0x01, kTri = 0x10, kSaw = 0x20, kPulse = 0x40;

void write_voice(reSID::SID& sid, int voice, uint16_t freq, uint16_t pw, uint8_t control) {
    const uint8_t base = static_cast<uint8_t>(voice * 7);
    sid.write(base + reg::kFreqLo, freq & 0xFF);
    sid.write(base + reg::kFreqHi, freq >> 8);
    sid.write(base + reg::kPwLo, pw & 0xFF);
    sid.write(base + reg::kPwHi, (pw >> 8) & 0x0F);
    sid.write(base + reg::kControl, control);
}

void init_sid(reSID::SID& sid, const Scenario& scenario) {
    sid.reset();
    sid.set_chip_model(scenario.model);
    sid.set_sampling_parameters(kSidClockPal, scenario.method, scenario.rate);
    for (int voice = 0; voice < 3; ++voice) {
        const uint8_t base = static_cast<uint8_t>(voice * 7);
        sid.write(base + reg::kAttackDecay, 0x09);
        sid.write(base + reg::kSustainRelease, 0xA6);
    }
    sid.write(reg::kResFilt, (12 << 4) | 0x07);  // Resonanz 12, alle drei Stimmen ins Filter
    sid.write(reg::kModeVol, 0x30 | 0x0F);       // Tief- und Bandpass, volle Lautstärke
}

/// Das „Synth-Engine"-Stück: Registerwerte für den nächsten Block, etwa 340 mal pro
/// Sekunde. Steht mit in der Messung, ist aber gegenüber reSID vernachlässigbar.
void update_registers(reSID::SID& sid, uint32_t sample_pos, uint32_t rate, int instance) {
    static constexpr uint8_t kPattern[] = {45, 52, 57, 60, 64, 60, 57, 52};
    const uint32_t step_samples = rate / 8;  // 125 ms pro Ton
    const uint32_t step = sample_pos / step_samples;
    const uint32_t in_step = sample_pos % step_samples;
    const bool gate = in_step < step_samples * 3 / 4;

    const uint8_t note = kPattern[step % sizeof(kPattern)];
    const float t = static_cast<float>(sample_pos) / static_cast<float>(rate);
    const uint16_t pw = static_cast<uint16_t>(2048 + 1600 * sinf(t * 2.0f * PI * 0.7f));
    const uint16_t cutoff = static_cast<uint16_t>(
        900 + 700 * sinf(t * 2.0f * PI * 0.5f + static_cast<float>(instance)));

    const uint8_t g = gate ? kGate : 0;
    write_voice(sid, 0, g_note_freq[note], pw, kSaw | g);
    write_voice(sid, 1, g_note_freq[note + 7], pw, kPulse | g);
    write_voice(sid, 2, g_note_freq[note + 12] + 3, 4095 - pw, kPulse | kSaw | g);
    sid.write(reg::kFcLo, cutoff & 0x07);
    sid.write(reg::kFcHi, (cutoff >> 3) & 0xFF);
}

// MARK: - I2S

void i2s_begin(uint32_t rate) {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 6;
    config.dma_buf_len = kBlock;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = kI2sBckPin;
    pins.ws_io_num = kI2sWsPin;
    pins.data_out_num = kI2sDoutPin;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
}

// MARK: - Audio-Aufgabe (Kern 1)

void audio_task(void*) {
    static short mono[2][kBlock];
    static int16_t frame[kBlock * 2];

    for (;;) {
        for (int index = 0; index < kScenarioCount; ++index) {
            const Scenario& scenario = kScenarios[index];
            for (int i = 0; i < scenario.instances; ++i) init_sid(g_sid[i], scenario);
            i2s_set_sample_rates(I2S_NUM_0, scenario.rate);
            i2s_zero_dma_buffer(I2S_NUM_0);

            Measurement m;
            uint32_t sample_pos = 0;
            const uint32_t start_ms = millis();

            for (;;) {
                const uint32_t elapsed = millis() - start_ms;
                if (elapsed >= kWarmupMs + kMeasureMs) break;

                const uint32_t t0 = ESP.getCycleCount();
                bool short_block = false;
                for (int i = 0; i < scenario.instances; ++i) {
                    update_registers(g_sid[i], sample_pos, scenario.rate, i);
                    reSID::cycle_count delta = kBlock * 32;  // mehr als genug, clock() hört nach kBlock auf
                    const int produced = g_sid[i].clock(delta, mono[i], kBlock);
                    if (produced < kBlock) short_block = true;
                }
                for (int s = 0; s < kBlock; ++s) {
                    const short right = scenario.instances > 1 ? mono[1][s] : mono[0][s];
                    frame[2 * s] = right;
                    frame[2 * s + 1] = mono[0][s];
                }
                const uint32_t cycles = ESP.getCycleCount() - t0;
                for (int s = 0; s < kBlock; ++s) {
                    const int16_t level = static_cast<int16_t>(abs(mono[0][s]));
                    if (level > m.peak) m.peak = level;
                }

                if (elapsed >= kWarmupMs) {
                    ++m.blocks;
                    m.render_cycles += cycles;
                    if (cycles > m.max_block_cycles) m.max_block_cycles = cycles;
                    if (short_block) ++m.short_blocks;
                }
                sample_pos += kBlock;

                size_t written = 0;
                i2s_write(I2S_NUM_0, frame, sizeof(frame), &written, portMAX_DELAY);
            }

            g_results[index] = m;
            g_audio_stack_free = uxTaskGetStackHighWaterMark(nullptr);
            g_finished = index;
        }
        ++g_round;
    }
}

void print_header() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.println();
    Serial.println("=== reSID-Messung auf dem ESP32-S3 ===");
    Serial.printf("%d Kerne, %u MHz\n", chip.cores, static_cast<unsigned>(getCpuFrequencyMhz()));
    Serial.printf("Flash %u MB, PSRAM %u KB (frei %u KB), interner Heap frei %u KB\n",
                  static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                  static_cast<unsigned>(ESP.getPsramSize() / 1024),
                  static_cast<unsigned>(ESP.getFreePsram() / 1024),
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    Serial.printf("Block %d Samples, Messdauer %u ms je Szenario, sizeof(SID) = %u Byte\n",
                  kBlock, static_cast<unsigned>(kMeasureMs),
                  static_cast<unsigned>(sizeof(reSID::SID)));
    Serial.println("Last = Rechenzeit pro Block / Klangdauer des Blocks, für einen Kern");
    Serial.println();
}

void print_result(int index) {
    const Scenario& scenario = kScenarios[index];
    const Measurement& m = g_results[index];
    if (m.blocks == 0) return;
    const double cpu_hz = getCpuFrequencyMhz() * 1e6;
    const double block_cycles = cpu_hz * kBlock / scenario.rate;
    const double avg = static_cast<double>(m.render_cycles) / m.blocks;
    Serial.printf("%s   Last %5.1f %%  (Spitze %5.1f %%)   %4.1f us/Sample   Pegel %5.1f %%   "
                  "%u Bloecke%s\n",
                  scenario.name, 100.0 * avg / block_cycles,
                  100.0 * m.max_block_cycles / block_cycles,
                  avg / kBlock / (cpu_hz / 1e6), 100.0 * m.peak / 32768.0,
                  static_cast<unsigned>(m.blocks), m.short_blocks ? "  KURZE BLOECKE!" : "");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);

    for (int note = 0; note < 128; ++note) {
        const double hz = 440.0 * pow(2.0, (note - 69) / 12.0);
        const double value = hz * 16777216.0 / kSidClockPal;
        g_note_freq[note] = value > 65535.0 ? 65535 : static_cast<uint16_t>(value);
    }

    print_header();
    i2s_begin(kScenarios[0].rate);
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 18, nullptr, 1);
}

void loop() {
    // Die überlasteten Szenarien lassen dieser Schleife keine Rechenzeit — es können
    // also mehrere auf einmal fertig geworden sein. Alle nachholen.
    static int printed = -1;
    static uint32_t printed_round = 0;
    const int finished = g_finished;
    while (printed != finished) {
        printed = (printed + 1) % kScenarioCount;
        print_result(printed);
        if (printed == kScenarioCount - 1) {
            Serial.printf("--- Runde %u fertig, Stack der Audio-Aufgabe frei: %u Byte ---\n\n",
                          static_cast<unsigned>(++printed_round),
                          static_cast<unsigned>(g_audio_stack_free));
        }
    }
    delay(20);
}
