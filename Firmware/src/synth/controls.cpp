#include "controls.h"

#include <Adafruit_seesaw.h>
#include <Arduino.h>
#include <Wire.h>
#include <seesaw_neopixel.h>

#include <atomic>

namespace synth {
namespace controls {
namespace {

constexpr uint8_t kAddress = 0x49;
constexpr uint16_t kProduct = 5752;
constexpr uint8_t kNeoPixelPin = 18;
constexpr uint8_t kSwitchPins[kKnobs] = {12, 14, 17, 9};
constexpr int kEncoderSign = -1;         // das Board zählt im Uhrzeigersinn rückwärts
constexpr int32_t kPlausibleDelta = 40;  // mehr Rasten in 20 ms ist ein Lesefehler
constexpr uint32_t kPollMs = 20;
constexpr uint32_t kSearchMs = 2000;
constexpr uint32_t kCheckMs = 1000;

Adafruit_seesaw g_seesaw(&Wire);
seesaw_NeoPixel g_pixels(kKnobs, kNeoPixelPin, NEO_GRB + NEO_KHZ800, &Wire);
QueueHandle_t g_events = nullptr;
std::atomic<bool> g_present{false};
std::atomic<uint32_t> g_led[kKnobs];
std::atomic<bool> g_led_dirty{true};

int32_t g_positions[kKnobs];
bool g_pressed[kKnobs];

uint32_t switch_mask() {
    uint32_t mask = 0;
    for (uint8_t pin : kSwitchPins) mask |= 1ul << pin;
    return mask;
}

bool is_our_board() { return ((g_seesaw.getVersion() >> 16) & 0xFFFF) == kProduct; }

bool connect() {
    if (!g_seesaw.begin(kAddress) || !g_pixels.begin(kAddress) || !is_our_board()) return false;
    g_seesaw.pinModeBulk(switch_mask(), INPUT_PULLUP);
    for (int k = 0; k < kKnobs; ++k) {
        g_positions[k] = g_seesaw.getEncoderPosition(k);
        g_pressed[k] = false;
    }
    g_pixels.setBrightness(60);
    g_led_dirty.store(true);
    return true;
}

void push(Event::Type type, int knob, int delta) {
    const Event event{type, static_cast<uint8_t>(knob), static_cast<int16_t>(delta)};
    xQueueSend(g_events, &event, 0);
}

/// Eine Abfrage. false, wenn das Board offensichtlich nicht mehr antwortet.
bool read_once() {
    for (int k = 0; k < kKnobs; ++k) {
        const int32_t position = g_seesaw.getEncoderPosition(k);
        const int32_t delta = position - g_positions[k];
        g_positions[k] = position;
        if (delta != 0 && delta >= -kPlausibleDelta && delta <= kPlausibleDelta) {
            push(Event::Type::Turn, k, kEncoderSign * delta);
        }
    }

    // Ohne Board liest der Bus lauter Nullen — das sähe aus wie vier gedrückte Taster
    // auf einmal. Das macht niemand mit zwei Händen, also gilt es als abgezogen.
    const uint32_t buttons = g_seesaw.digitalReadBulk(switch_mask());
    if ((buttons & switch_mask()) == 0) return false;
    for (int k = 0; k < kKnobs; ++k) {
        const bool down = (buttons & (1ul << kSwitchPins[k])) == 0;  // gedrückt = LOW
        if (down && !g_pressed[k]) push(Event::Type::Press, k, 0);
        g_pressed[k] = down;
    }

    if (g_led_dirty.exchange(false)) {
        for (int k = 0; k < kKnobs; ++k) g_pixels.setPixelColor(k, g_led[k].load());
        g_pixels.show();
    }
    return true;
}

void task(void*) {
    uint32_t next_check_ms = 0;
    for (;;) {
        if (!g_present.load()) {
            if (!connect()) {
                vTaskDelay(pdMS_TO_TICKS(kSearchMs));
                continue;
            }
            g_present.store(true);
            Serial.println("ENC   Encoder-Board gefunden");
        }
        bool ok = read_once();
        if (ok && static_cast<int32_t>(millis() - next_check_ms) >= 0) {
            next_check_ms = millis() + kCheckMs;
            ok = is_our_board();
        }
        if (!ok) {
            g_present.store(false);
            Serial.println("ENC   Encoder-Board weg");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

void begin() {
    g_events = xQueueCreate(32, sizeof(Event));
    for (auto& led : g_led) led.store(0);
    Wire.begin(kSdaPin, kSclPin);
    Wire.setClock(400000);
    xTaskCreatePinnedToCore(task, "controls", 4096, nullptr, 1, nullptr, 0);
}

bool poll(Event& event) { return g_events != nullptr && xQueueReceive(g_events, &event, 0) == pdTRUE; }

bool present() { return g_present.load(); }

void set_led(int knob, uint8_t red, uint8_t green, uint8_t blue) {
    if (knob < 0 || knob >= kKnobs) return;
    const uint32_t color = (static_cast<uint32_t>(red) << 16) | (green << 8) | blue;
    if (g_led[knob].exchange(color) != color) g_led_dirty.store(true);
}

}  // namespace controls
}  // namespace synth
