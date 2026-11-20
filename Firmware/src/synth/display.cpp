#include "display.h"

#include "board.h"

#if SYNTH_HAS_DISPLAY

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include <atomic>

#include "panel.h"
#include "params.h"

namespace synth {
namespace display {
namespace {

constexpr int kW = board::kLcdWidth;
constexpr int kH = board::kLcdHeight;
constexpr int kScale = (kW / panel::kWidth) < (kH / panel::kHeight) ? (kW / panel::kWidth)
                                                                      : (kH / panel::kHeight);
constexpr int kPanelX = (kW - panel::kWidth * kScale) / 2;
constexpr int kPanelY = (kH - panel::kHeight * kScale) / 2;
constexpr uint32_t kStreamTimeoutMs = 2000;  // so lange ohne Bild vom Tab5 → SID-Seite
constexpr uint32_t kSidHoldMs = 4000;        // so lange nach dem letzten Encoder-Dreh
constexpr uint32_t kColorsAtBootMs = 3000;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kBlack = rgb(0, 0, 0);
constexpr uint16_t kBack = rgb(12, 14, 22);
constexpr uint16_t kHeadBack = rgb(28, 34, 58);
constexpr uint16_t kText = rgb(235, 235, 240);
constexpr uint16_t kDim = rgb(120, 125, 140);
constexpr uint16_t kTrack = rgb(40, 44, 58);
constexpr uint16_t kAccent = rgb(140, 120, 255);  // wie die Preset-LED
constexpr uint16_t kOrange = rgb(255, 120, 0);    // ungespeicherte Änderungen
constexpr uint16_t kGreen = rgb(40, 220, 80);
constexpr uint16_t kRed = rgb(255, 40, 40);
// Die Farben der drei Regler-LEDs (synth_main.cpp).
constexpr uint16_t kKnobColor[3] = {rgb(255, 72, 0), rgb(48, 255, 96), rgb(72, 140, 255)};

Arduino_DataBus* g_bus = nullptr;
Arduino_GFX* g_gfx = nullptr;
bool g_present = false;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
View g_view;              // unter g_mux
uint32_t g_view_version;  // unter g_mux
std::atomic<uint32_t> g_sid_until_ms{0};
std::atomic<Mode> g_mode{Mode::Auto};
std::atomic<uint8_t> g_flip{kFlipSid};
std::atomic<uint32_t> g_busy_permille{0};
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_worst_us{0};

// MARK: - Text

/// Das eingebaute 5×7-Font folgt Codepage 437: die Umlaute des Editors dorthin
/// übersetzen, alles Unbekannte wird zu „?“.
void to_cp437(const char* in, char* out, size_t size) {
    size_t n = 0;
    const auto* s = reinterpret_cast<const uint8_t*>(in);
    while (*s != 0 && n + 1 < size) {
        uint8_t c = *s;
        if (c < 0x80) {
            out[n++] = static_cast<char>(c);
            ++s;
            continue;
        }
        char mapped = '?';
        if (c == 0xC3 && s[1] != 0) {
            switch (s[1]) {
                case 0xA4: mapped = static_cast<char>(0x84); break;  // ä
                case 0xB6: mapped = static_cast<char>(0x94); break;  // ö
                case 0xBC: mapped = static_cast<char>(0x81); break;  // ü
                case 0x84: mapped = static_cast<char>(0x8E); break;  // Ä
                case 0x96: mapped = static_cast<char>(0x99); break;  // Ö
                case 0x9C: mapped = static_cast<char>(0x9A); break;  // Ü
                case 0x9F: mapped = static_cast<char>(0xE1); break;  // ß
                case 0xA9: mapped = static_cast<char>(0x82); break;  // é
            }
            s += 2;
        } else if (c == 0xE2 && s[1] == 0x86 && s[2] == 0x92) {
            mapped = static_cast<char>(0x1A);  // →
            s += 3;
        } else {
            ++s;
            while ((*s & 0xC0) == 0x80) ++s;  // Rest des Zeichens überspringen
        }
        out[n++] = mapped;
    }
    out[n] = '\0';
}

/// Text in einem Feld fester Breite (`chars` Zeichen), mit Hintergrund — überschreibt
/// den alten Inhalt ohne Flackern. align: 0 links, 1 rechts.
void field(int x, int y, int size, int chars, uint16_t fg, uint16_t bg, const char* utf8,
           int align = 0) {
    char text[64];
    to_cp437(utf8, text, sizeof(text));
    int len = static_cast<int>(strlen(text));
    if (len > chars) {
        len = chars;
        text[len] = '\0';
    }
    char padded[64];
    const int pad = chars - len;
    if (align == 1) {
        memset(padded, ' ', pad);
        memcpy(padded + pad, text, len + 1);
    } else {
        memcpy(padded, text, len);
        memset(padded + len, ' ', pad);
        padded[chars] = '\0';
    }
    g_gfx->setTextSize(size);
    g_gfx->setTextColor(fg, bg);
    g_gfx->setCursor(x, y);
    g_gfx->print(padded);
}

// MARK: - Werte wie im Editor

const int kSidAttack[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
const int kSidDecay[16] = {6,   24,  48,   72,   114,  168,  204,   240,
                           300, 750, 1500, 2400, 3000, 9000, 15000, 24000};

void with_comma(char* s) {
    for (; *s != 0; ++s)
        if (*s == '.') *s = ',';
}

void fmt_ms(float x, char* out, size_t size) {
    if (x < 1000) {
        snprintf(out, size, "%d ms", static_cast<int>(lroundf(x)));
    } else {
        snprintf(out, size, x < 10000 ? "%.2f s" : "%.1f s", x / 1000.0f);
        with_comma(out);
    }
}

void format_value(const Param& p, int16_t v, char* out, size_t size) {
    if (p.kind == Kind::Toggle) {
        snprintf(out, size, "%s", v ? "an" : "aus");
        return;
    }
    if (p.kind == Kind::Choice) {
        const char* c = p.choices;
        for (int i = 0; i < v && c != nullptr; ++i) {
            c = strchr(c, '|');
            if (c != nullptr) ++c;
        }
        if (c == nullptr) c = "?";
        const char* end = strchr(c, '|');
        const size_t len = end != nullptr ? static_cast<size_t>(end - c) : strlen(c);
        snprintf(out, size, "%.*s", static_cast<int>(len), c);
        return;
    }
    switch (p.format) {
        case Format::Semitones: snprintf(out, size, "%+d HT", v); break;
        case Format::Cents: snprintf(out, size, "%+d ct", v); break;
        case Format::Percent: snprintf(out, size, p.min < 0 ? "%+d %%" : "%d %%", v); break;
        case Format::Pulse: snprintf(out, size, "%d %%", static_cast<int>(lroundf(v / 40.95f))); break;
        case Format::SidAttack: fmt_ms(kSidAttack[v & 15], out, size); break;
        case Format::SidDecay: fmt_ms(kSidDecay[v & 15], out, size); break;
        case Format::Exp:
        case Format::Exp0: {
            const float x = curve(p, v);
            if (strcmp(p.unit, "ms") == 0) {
                fmt_ms(x, out, size);
            } else if (x == 0) {
                snprintf(out, size, "aus");
            } else if (strcmp(p.unit, "Hz") == 0) {
                snprintf(out, size, x < 1 ? "%.2f Hz" : x < 10 ? "%.1f Hz" : "%.0f Hz", x);
                with_comma(out);
            } else {
                snprintf(out, size, "%d %s", static_cast<int>(lroundf(x)), p.unit);
            }
            break;
        }
        default: snprintf(out, size, "%d", v); break;
    }
}

// MARK: - SID-Seite

constexpr int kHeadH = 44;
constexpr int kRowY[3] = {54, 118, 182};
constexpr int kBarX = 12;
constexpr int kBarW = kW - 24;
constexpr int kFootY = 250;

void draw_sid_frame() {
    g_gfx->fillScreen(kBack);
    g_gfx->fillRect(0, 0, kW, kHeadH, kHeadBack);
    for (int k = 0; k < 3; ++k) g_gfx->fillRect(kBarX, kRowY[k] + 32, kBarW, 18, kTrack);
}

void draw_head(const View& v) {
    char slot[12];
    snprintf(slot, sizeof(slot), "%02d", v.slot + 1);
    field(12, 10, 3, 2, kAccent, kHeadBack, slot);
    field(66, 10, 3, 21, kText, kHeadBack, v.name);
    field(kW - 30, 10, 3, 1, kOrange, kHeadBack, v.dirty ? "*" : " ");
}

void draw_knob(int k, const View& v, bool label) {
    const Param& p = kParams[v.knob[k]];
    const int y = kRowY[k];
    if (label) {
        char text[48];
        snprintf(text, sizeof(text), "%s %s", p.group, p.label);
        field(kBarX, y + 4, 2, 24, kKnobColor[k], kBack, text);
    }
    char value[24];
    format_value(p, v.value[k], value, sizeof(value));
    field(kW - 12 - 9 * 18, y, 3, 9, kText, kBack, value, 1);

    const int span = p.max - p.min;
    int fill = span > 0 ? static_cast<int>(static_cast<long>(v.value[k] - p.min) * kBarW / span) : 0;
    if (fill < 0) fill = 0;
    if (fill > kBarW) fill = kBarW;
    g_gfx->fillRect(kBarX, y + 32, fill, 18, kKnobColor[k]);
    g_gfx->fillRect(kBarX + fill, y + 32, kBarW - fill, 18, kTrack);
}

constexpr int kMeterX = 120;
constexpr int kMeterW = 150;

void draw_foot(const View& v, const View* old) {
    if (old == nullptr || old->midi != v.midi) {
        field(kBarX, kFootY + 2, 2, 4, v.midi ? kGreen : kDim, kBack, "MIDI");
        g_gfx->fillCircle(kBarX + 62, kFootY + 9, 6, v.midi ? kGreen : kTrack);
    }
    if (old == nullptr || old->peak != v.peak) {
        const int level = static_cast<int>(static_cast<long>(v.peak) * kMeterW / 32767);
        g_gfx->fillRect(kMeterX, kFootY + 3, level, 12, v.peak >= 32400 ? kRed : kGreen);
        g_gfx->fillRect(kMeterX + level, kFootY + 3, kMeterW - level, 12, kTrack);
    }
    if (old == nullptr || strcmp(old->net, v.net) != 0 || old->encoders != v.encoders) {
        char text[40];
        snprintf(text, sizeof(text), "%s%s", v.encoders ? "" : "ENC? ", v.net);
        field(kW - 12 - 15 * 12, kFootY + 2, 2, 15, v.encoders ? kDim : kOrange, kBack, text, 1);
    }
}

void draw_sid(bool full) {
    static View drawn;
    View v;
    portENTER_CRITICAL(&g_mux);
    v = g_view;
    portEXIT_CRITICAL(&g_mux);

    if (full) {
        draw_sid_frame();
        draw_head(v);
        for (int k = 0; k < 3; ++k) draw_knob(k, v, true);
        draw_foot(v, nullptr);
        drawn = v;
        return;
    }
    if (drawn.slot != v.slot || drawn.dirty != v.dirty || strcmp(drawn.name, v.name) != 0) {
        draw_head(v);
    }
    for (int k = 0; k < 3; ++k) {
        const bool label = drawn.knob[k] != v.knob[k];
        if (label || drawn.value[k] != v.value[k]) draw_knob(k, v, label);
    }
    draw_foot(v, &drawn);
    drawn = v;
}

// MARK: - Tafel

/// false, wenn es kein neues Bild gab.
bool draw_panel(bool full) {
    static uint8_t shown[panel::kWidth * panel::kHeight * 3];
    static uint8_t now[panel::kWidth * panel::kHeight * 3];
    static uint16_t band[panel::kWidth * kScale * kScale];
    static uint32_t version = 0;

    const uint32_t v = panel::snapshot(now);
    if (!full && v == version) return false;
    version = v;
    if (full) g_gfx->fillScreen(kBlack);

    constexpr size_t kRow = panel::kWidth * 3;
    for (int y = 0; y < panel::kHeight; ++y) {
        if (!full && memcmp(&now[y * kRow], &shown[y * kRow], kRow) == 0) continue;
        for (int x = 0; x < panel::kWidth; ++x) {
            const uint8_t* px = &now[y * kRow + x * 3];
            const uint16_t c = rgb(px[0], px[1], px[2]);
            for (int s = 0; s < kScale; ++s) band[x * kScale + s] = c;
        }
        for (int line = 1; line < kScale; ++line) {
            memcpy(&band[line * panel::kWidth * kScale], band, panel::kWidth * kScale * 2);
        }
        g_gfx->draw16bitRGBBitmap(kPanelX, kPanelY + y * kScale, band, panel::kWidth * kScale, kScale);
    }
    memcpy(shown, now, sizeof(shown));
    return true;
}

// MARK: - Farbprobe

/// Vier Balken mit Namen darunter: stimmt, was dasteht, mit der Farbe überein, sind
/// Farbreihenfolge und Byte-Reihenfolge richtig eingestellt.
void draw_colors() {
    g_gfx->fillScreen(kBlack);
    const uint16_t colors[4] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), rgb(255, 255, 255)};
    const char* names[4] = {"ROT", "GRÜN", "BLAU", "WEISS"};
    const int w = kW / 4;
    for (int i = 0; i < 4; ++i) {
        g_gfx->fillRect(i * w, 0, w, 200, colors[i]);
        field(i * w + 12, 216, 3, 5, kText, kBlack, names[i]);
    }
    field(12, 250, 2, 38, kDim, kBlack, "SID-Synth  Farbprobe  480x272");
}

// MARK: - Aufgabe

/// Dreht das Display passend zum Bild, das gleich kommt (Speicherzugriff des Controllers,
/// kostet nichts beim Zeichnen). true, wenn sich die Drehung geändert hat — dann muss
/// alles neu gezeichnet werden.
bool orient(Mode mode) {
    static uint8_t applied = 0;  // begin() startet mit Drehung 0
    const uint8_t bit = mode == Mode::Sid ? kFlipSid : kFlipPanel;
    const uint8_t rotation = (g_flip.load() & bit) != 0 ? 2 : 0;
    if (rotation == applied) return false;
    g_gfx->setRotation(rotation);
    applied = rotation;
    return true;
}

void task(void*) {
    Mode shown = Mode::Colors;
    orient(Mode::Colors);
    draw_colors();
    const uint32_t colors_until = millis() + kColorsAtBootMs;
    while (static_cast<int32_t>(millis() - colors_until) < 0 && g_mode.load() == Mode::Auto) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    shown = Mode::Auto;  // erzwingt das erste volle Bild

    uint32_t view_version = 0;
    uint32_t window_ms = millis();
    uint32_t busy_us = 0;
    uint32_t frames = 0;
    for (;;) {
        const uint32_t now = millis();
        Mode want = g_mode.load();
        if (want == Mode::Auto) {
            const uint32_t last = panel::last_frame_ms();
            const bool stream = last != 0 && now - last < kStreamTimeoutMs;
            const bool held = static_cast<int32_t>(g_sid_until_ms.load() - now) > 0;
            want = (!stream || held) ? Mode::Sid : Mode::Panel;
        }
        const bool turned = orient(want);
        const bool full = turned || want != shown;
        shown = want;
        const uint32_t start_us = micros();
        bool drew = false;
        switch (want) {
            case Mode::Panel:
                drew = draw_panel(full);
                break;
            case Mode::Colors:
                if (full) draw_colors();
                drew = full;
                break;
            default: {
                portENTER_CRITICAL(&g_mux);
                const uint32_t version = g_view_version;
                portEXIT_CRITICAL(&g_mux);
                drew = full || version != view_version;
                if (drew) draw_sid(full);
                view_version = version;
                break;
            }
        }
        if (drew) {
            const uint32_t took = micros() - start_us;
            busy_us += took;
            ++frames;
            if (took > g_worst_us.load()) g_worst_us.store(took);
        }
        if (now - window_ms >= 1000) {
            g_busy_permille.store(busy_us / (now - window_ms));
            g_frames.store(frames);
            window_ms = now;
            busy_us = 0;
            frames = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(want == Mode::Panel ? 5 : 15));
    }
}

}  // namespace

void begin() {
    pinMode(board::kLcdBacklight, OUTPUT);
    digitalWrite(board::kLcdBacklight, LOW);
    g_bus = new Arduino_ESP32QSPI(board::kLcdCs, board::kLcdSck, board::kLcdD0, board::kLcdD1,
                                  board::kLcdD2, board::kLcdD3);
    g_gfx = new Arduino_NV3041A(g_bus, GFX_NOT_DEFINED, 0, true);
    if (!g_gfx->begin()) {
        Serial.println("DISPLAY startet nicht");
        return;
    }
    g_present = true;
    g_gfx->fillScreen(kBlack);
    digitalWrite(board::kLcdBacklight, HIGH);
    xTaskCreatePinnedToCore(task, "display", 6144, nullptr, 1, nullptr, 0);
    Serial.printf("DISPLAY %dx%d, Tafel %dx%d je %d Pixel\n", kW, kH, panel::kWidth,
                  panel::kHeight, kScale);
}

void set_view(const View& view) {
    portENTER_CRITICAL(&g_mux);
    if (memcmp(&g_view, &view, sizeof(View)) != 0) {
        g_view = view;
        ++g_view_version;
    }
    portEXIT_CRITICAL(&g_mux);
}

void wake() { g_sid_until_ms.store(millis() + kSidHoldMs); }

void set_mode(Mode mode) { g_mode.store(mode); }
Mode mode() { return g_mode.load(); }
bool present() { return g_present; }

void set_flip(uint8_t flags) { g_flip.store(flags & (kFlipSid | kFlipPanel)); }
uint8_t flip() { return g_flip.load(); }

Stats stats() {
    return {g_busy_permille.load() / 10.0f, static_cast<uint16_t>(g_frames.load()),
            g_worst_us.load() / 1000.0f};
}

}  // namespace display
}  // namespace synth

#else  // kein Display

namespace synth {
namespace display {
void begin() {}
void set_view(const View&) {}
void wake() {}
void set_mode(Mode) {}
Mode mode() { return Mode::Auto; }
bool present() { return false; }
void set_flip(uint8_t) {}
uint8_t flip() { return 0; }
Stats stats() { return {0, 0, 0}; }
}  // namespace display
}  // namespace synth

#endif
