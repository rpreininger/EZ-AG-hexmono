#include "panel.h"

#include "board.h"

#if SYNTH_HAS_DISPLAY

#include <Arduino.h>
#include <AsyncUDP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

namespace synth {
namespace panel {
namespace {

AsyncUDP g_udp;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_pixels[kWidth * kHeight * 3];  // unter g_mux
uint32_t g_version = 0;                  // unter g_mux
std::atomic<uint32_t> g_last_ms{0};
std::atomic<uint32_t> g_packets{0};
std::atomic<uint32_t> g_bad{0};
std::atomic<uint32_t> g_test_until_ms{0};
TaskHandle_t g_test_task = nullptr;

/// Ein Rechteck in das Bild schreiben, was über den Rand ragt, fällt weg.
void put(int ox, int oy, int w, int h, const uint8_t* rgb) {
    const int x0 = ox < 0 ? 0 : ox;
    const int x1 = ox + w > kWidth ? kWidth : ox + w;
    if (x0 >= x1) return;
    portENTER_CRITICAL(&g_mux);
    for (int r = 0; r < h; ++r) {
        const int y = oy + r;
        if (y < 0 || y >= kHeight) continue;
        memcpy(&g_pixels[(y * kWidth + x0) * 3], rgb + (static_cast<size_t>(r) * w + (x0 - ox)) * 3,
               static_cast<size_t>(x1 - x0) * 3);
    }
    ++g_version;
    portEXIT_CRITICAL(&g_mux);
}

bool is_space(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Bis zu `max` ganze Zahlen aus einem Stück Text, z. B. „#FT: 0 3“ oder dem Nachspann.
int read_ints(const uint8_t* p, size_t n, long* out, int max) {
    char text[40];
    const size_t len = n < sizeof(text) - 1 ? n : sizeof(text) - 1;
    memcpy(text, p, len);
    text[len] = '\0';
    int count = 0;
    char* s = text;
    while (count < max) {
        char* end = nullptr;
        const long v = strtol(s, &end, 10);
        if (end == s) break;
        out[count++] = v;
        s = end;
    }
    return count;
}

/// Ein Paket der Flaschen-Taschen-Art: „P6“, Breite, Höhe, 255, ein Leerzeichen, die
/// Punkte. Der Versatz steht als Kommentar „#FT: x y [z]“ im Kopf oder als Nachspann
/// „x y [z]“ hinter den Punkten — beides kommt vor.
bool handle(const uint8_t* p, size_t n) {
    if (n < 3 || p[0] != 'P' || p[1] != '6') return false;
    size_t i = 2;
    long header[3];
    int fields = 0;
    long ox = 0, oy = 0;
    bool have_offset = false;
    while (fields < 3) {
        while (i < n && (is_space(p[i]) || p[i] == '#')) {
            if (p[i] != '#') {
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < n && p[i] != '\n') ++i;
            if (i - start > 4 && memcmp(p + start, "#FT:", 4) == 0) {
                long v[3];
                if (read_ints(p + start + 4, i - start - 4, v, 3) >= 2) {
                    ox = v[0];
                    oy = v[1];
                    have_offset = true;
                }
            }
        }
        if (i >= n || p[i] < '0' || p[i] > '9') return false;
        long v = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            v = v * 10 + (p[i++] - '0');
            if (v > 65535) return false;
        }
        header[fields++] = v;
    }
    if (i >= n || !is_space(p[i])) return false;
    ++i;  // genau ein Trennzeichen vor den Punkten
    const long w = header[0];
    const long h = header[1];
    if (header[2] != 255 || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    const size_t data = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (n - i < data) return false;
    if (!have_offset && n - i > data) {
        long v[3];
        if (read_ints(p + i + data, n - i - data, v, 3) >= 2) {
            ox = v[0];
            oy = v[1];
        }
    }
    put(static_cast<int>(ox), static_cast<int>(oy), static_cast<int>(w), static_cast<int>(h),
        p + i);
    return true;
}

void count(bool ok) {
    if (ok) {
        g_packets.fetch_add(1);
        uint32_t now = millis();
        g_last_ms.store(now != 0 ? now : 1);
    } else {
        g_bad.fetch_add(1);
    }
}

// MARK: - Testbild

/// Wie ein Bild vom Tab5: Pakete zu drei Zeilen mit „#FT: 0 y“, 25 Bilder pro Sekunde.
/// Zu sehen: ein Regenbogen, der nach links wandert, darüber ein weißer Punkt, der an
/// den Rändern abprallt, und an den vier Ecken je ein Punkt in Rot, Grün, Blau, Weiß.
void test_task(void*) {
    static uint8_t packet[64 + kWidth * 3 * 3];
    static uint8_t frame[kWidth * kHeight * 3];
    uint32_t step = 0;
    int bx = 3, by = 5, dx = 1, dy = 1;
    for (;;) {
        if (static_cast<int32_t>(millis() - g_test_until_ms.load()) >= 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int hue = ((x + static_cast<int>(step)) * 8) % 192;  // 0..191
                const int seg = hue / 64, f = (hue % 64) * 4;
                uint8_t r = 0, g = 0, b = 0;
                if (seg == 0) { r = 255 - f; g = f; }
                else if (seg == 1) { g = 255 - f; b = f; }
                else { b = 255 - f; r = f; }
                const uint8_t dim = static_cast<uint8_t>(40 + y * 6);
                uint8_t* px = &frame[(y * kWidth + x) * 3];
                px[0] = static_cast<uint8_t>(r * dim / 255);
                px[1] = static_cast<uint8_t>(g * dim / 255);
                px[2] = static_cast<uint8_t>(b * dim / 255);
            }
        }
        const uint8_t corners[4][5] = {{0, 0, 255, 0, 0},
                                       {kWidth - 1, 0, 0, 255, 0},
                                       {0, kHeight - 1, 0, 0, 255},
                                       {kWidth - 1, kHeight - 1, 255, 255, 255}};
        for (const auto& c : corners) memcpy(&frame[(c[1] * kWidth + c[0]) * 3], &c[2], 3);
        memset(&frame[(by * kWidth + bx) * 3], 255, 3);
        bx += dx;
        by += dy;
        if (bx <= 0 || bx >= kWidth - 1) dx = -dx;
        if (by <= 0 || by >= kHeight - 1) dy = -dy;

        for (int y = 0; y < kHeight; y += 3) {
            const int th = kHeight - y < 3 ? kHeight - y : 3;
            const int hl = snprintf(reinterpret_cast<char*>(packet), 64, "P6\n#FT: 0 %d\n%d %d\n255\n",
                                    y, kWidth, th);
            memcpy(packet + hl, &frame[y * kWidth * 3], static_cast<size_t>(kWidth) * th * 3);
            count(handle(packet, hl + static_cast<size_t>(kWidth) * th * 3));
        }
        ++step;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

}  // namespace

void begin() {
    if (g_udp.listen(kPort)) {
        g_udp.onPacket([](AsyncUDPPacket packet) { count(handle(packet.data(), packet.length())); });
        Serial.printf("TAFEL lauscht auf UDP %u (%dx%d)\n", kPort, kWidth, kHeight);
    } else {
        Serial.println("TAFEL UDP-Port nicht frei");
    }
}

uint32_t snapshot(uint8_t* rgb) {
    portENTER_CRITICAL(&g_mux);
    memcpy(rgb, g_pixels, sizeof(g_pixels));
    const uint32_t version = g_version;
    portEXIT_CRITICAL(&g_mux);
    return version;
}

uint32_t last_frame_ms() { return g_last_ms.load(); }

void test(uint32_t seconds) {
    if (g_test_task == nullptr) {
        xTaskCreatePinnedToCore(test_task, "paneltest", 4096, nullptr, 1, &g_test_task, 0);
    }
    g_test_until_ms.store(millis() + seconds * 1000);
}

Stats stats() {
    Stats s;
    s.packets = g_packets.load();
    s.bad = g_bad.load();
    return s;
}

}  // namespace panel
}  // namespace synth

#else  // kein Display

namespace synth {
namespace panel {
void begin() {}
uint32_t snapshot(uint8_t*) { return 0; }
uint32_t last_frame_ms() { return 0; }
void test(uint32_t) {}
Stats stats() { return Stats{}; }
}  // namespace panel
}  // namespace synth

#endif
