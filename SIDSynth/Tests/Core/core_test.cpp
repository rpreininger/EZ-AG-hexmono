// Prüft die Klangerzeugung der Box auf dem Mac — denselben Code, den die App übersetzt.
//
//   make synth-test                  Prüfungen
//   build/synthtest --wav <ordner>   zusätzlich je Werksklang ein WAV (A3, 1,2 s gehalten)
//
// Die Pegel lassen sich mit tools/synth_check.py auf der Box vergleichen: dort wie hier
// je Werksklang eine Note A3 mit Velocity 100, Spitze in den ersten 0,7 s und danach.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "engine_core.h"
#include "factory.h"
#include "params.h"
#include "sid_host.h"

using namespace synth;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            fprintf(stderr, "FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/// Wie auf der Box: der Klang ist schon eine Weile geladen, bevor die Note kommt. Beim
/// 6581 macht das erste Schreiben der Lautstärke einen DC-Sprung (NOTES.md, Falle 5),
/// der sonst in die Messung fiele.
static void settle() {
    std::vector<float> silence(static_cast<size_t>(1.5 * kSampleRate));
    sid_render(silence.data(), static_cast<int>(silence.size()));
}

static std::vector<int16_t> render(const Patch& patch, double on_seconds, double off_seconds) {
    sid_engine_begin(patch.values);
    settle();
    std::vector<float> buffer(static_cast<size_t>((on_seconds + off_seconds) * kSampleRate));
    const size_t on = static_cast<size_t>(on_seconds * kSampleRate);
    sid_note_on(57, 100);
    sid_render(buffer.data(), static_cast<int>(on));
    sid_note_off(57);
    sid_render(buffer.data() + on, static_cast<int>(buffer.size() - on));
    std::vector<int16_t> out(buffer.size());
    for (size_t i = 0; i < buffer.size(); ++i) out[i] = static_cast<int16_t>(lroundf(buffer[i] * 32768.0f));
    return out;
}

static double peak_percent(const std::vector<int16_t>& samples, size_t from, size_t to) {
    int peak = 0;
    for (size_t i = from; i < to && i < samples.size(); ++i) peak = std::max(peak, abs(samples[i]));
    return peak / 327.67;
}

static void write_wav(const std::string& path, const std::vector<int16_t>& samples) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    const uint32_t data = static_cast<uint32_t>(samples.size() * 2);
    auto u32 = [f](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(kSampleRate); u32(kSampleRate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(data);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);
}

static void test_names() {
    char out[kNameLength];
    sid_set_name("Glocke ü", out);
    CHECK(strcmp(out, "Glocke ü") == 0);  // ein vollständiges Zeichen am Ende bleibt
    sid_set_name("Tab\tund\nZeilenumbruch", out);
    CHECK(strcmp(out, "TabundZeilenumbruch") == 0);
    // 22 Byte ASCII + „ä" (2 Byte) = 24 > 23: das ä passt nicht mehr und fällt ganz weg.
    sid_set_name("abcdefghijklmnopqrstuvä", out);
    CHECK(strcmp(out, "abcdefghijklmnopqrstuv") == 0);
    sid_set_name("abcdefghijklmnopqrstuvwxyz", out);
    CHECK(strlen(out) == kNameLength - 1);
}

static void test_schema() {
    const std::string schema = sid_schema_json();
    CHECK(schema.rfind("{\"slots\":32,\"params\":[", 0) == 0);
    CHECK(schema.substr(schema.size() - 2) == "]}");
    size_t keys = 0;
    for (size_t at = 0; (at = schema.find("\"key\":", at)) != std::string::npos; ++at) ++keys;
    CHECK(keys == kParamCount);
    CHECK(schema.find("\"key\":\"me.d\",\"label\":\"Decay\",\"group\":\"Hüllkurve\",\"kind\":\"range\","
                      "\"min\":0,\"max\":127,\"def\":70,\"fmt\":\"exp0\",\"lo\":1,\"hi\":10000,"
                      "\"unit\":\"ms\"") != std::string::npos);
}

/// Der Audio-Callback bekommt beliebige Längen; gerechnet wird trotzdem in 128er-Blöcken.
static void test_chunking() {
    Patch patch;
    presets::factory(0, patch);
    sid_engine_begin(patch.values);
    sid_note_on(60, 100);
    std::vector<float> a(44100);
    sid_render(a.data(), static_cast<int>(a.size()));

    sid_engine_begin(patch.values);
    sid_note_on(60, 100);
    std::vector<float> b(44100);
    const int sizes[] = {471, 512, 1, 127, 128, 129, 1024};
    size_t at = 0;
    for (int i = 0; at < b.size(); ++i) {
        const int n = std::min<int>(sizes[i % 7], static_cast<int>(b.size() - at));
        sid_render(b.data() + at, n);
        at += n;
    }
    CHECK(a == b);
}

static void test_volume_zero() {
    Patch patch;
    presets::factory(0, patch);
    patch.set(ParamId::P_VOL, 0);
    const std::vector<int16_t> s = render(patch, 1.0, 0.0);
    CHECK(peak_percent(s, kSampleRate / 2, s.size()) < 1.0);
}

static void test_factory(const char* wav_dir) {
    printf("Werksklänge, je eine Note A3 (Spitze in %% Vollaussteuerung)\n");
    for (int slot = 0; slot < presets::kSlots; ++slot) {
        Patch patch;
        if (!presets::factory(slot, patch)) continue;
        const std::vector<int16_t> s = render(patch, 1.2, 1.0);
        const size_t first = static_cast<size_t>(0.7 * kSampleRate);
        const double attack = peak_percent(s, 0, first);
        const double held = peak_percent(s, first, static_cast<size_t>(1.2 * kSampleRate));
        printf("  %02d %-24s erste 0,7 s %5.1f %%   danach gehalten %5.1f %%\n", slot + 1,
               patch.name, attack, held);
        // Auf der Box gemessen 20–47 %.
        CHECK(attack > 15.0 && attack < 55.0);
        CHECK(render(patch, 1.2, 1.0) == s);  // bei gleicher Eingabe bitgleich
        if (wav_dir != nullptr) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%02d.wav", wav_dir, slot + 1);
            write_wav(path, s);
        }
    }
}

int main(int argc, char** argv) {
    const char* wav_dir = argc == 3 && strcmp(argv[1], "--wav") == 0 ? argv[2] : nullptr;
    test_names();
    test_schema();
    test_chunking();
    test_volume_zero();
    test_factory(wav_dir);
    printf("%d Prüfungen, %d Fehler\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
