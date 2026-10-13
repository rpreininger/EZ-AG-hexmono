// Die Hülle der Klangerzeugung auf der Box, siehe engine.h.
//
// Die Klangrechnung selbst steht in engine_core.cpp — dieselbe Datei übersetzt die
// Mac-App. Hier nur, was der ESP32 dazu braucht. Pro Block (128 Samples, 2,9 ms) auf
// Kern 1:
//   1. Befehle aus der Warteschlange übernehmen — Parameter, Noten, Pitchbend
//   2. core::render_block(): Modulation, SID-Register, reSID, Ein- und Ausblenden
//   3. Last und Pegel messen, an I2S und ans Mithören geben

#include "engine.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "engine_core.h"

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

using core::Command;
using core::CommandType;
using core::kBlock;

constexpr int kDmaBuffers = 4;  // 4 × 128 Samples = 11,6 ms Vorrat

// MARK: - Geteilt mit den anderen Aufgaben

QueueHandle_t g_commands = nullptr;
PreviewRing g_preview;
std::atomic<bool> g_silent{false};
std::atomic<uint32_t> g_dropped{0};
std::atomic<int32_t> g_peak_hold{0};
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
EngineStats g_stats;

void push(CommandType type, uint8_t a, int16_t b) {
    if (g_commands == nullptr) return;
    const Command command{type, a, b};
    if (xQueueSend(g_commands, &command, 0) != pdTRUE) g_dropped.fetch_add(1);
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
        while (xQueueReceive(g_commands, &command, 0) == pdTRUE) core::apply(command);
        const int32_t block_peak = core::render_block(mono);
        if (block_peak > peak) peak = block_peak;
        for (int s = 0; s < kBlock; ++s) {
            frame[2 * s] = mono[s];
            frame[2 * s + 1] = mono[s];
        }
        g_silent.store(core::silent());
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
    g_commands = xQueueCreate(256, sizeof(Command));
    core::begin(patch);

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
