#include "ParamText.h"

#include <math.h>
#include <stdio.h>

namespace synth {
namespace {

// Zeiten der SID-Hüllkurve laut Datenblatt, in ms — wie SID_A/SID_D in index.html.
constexpr int kSidAttack[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
constexpr int kSidDecay[16] = {6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000};

/// Zahl mit Komma statt Punkt, wie num() im Editor.
std::string num(double x, int decimals) {
    char text[32];
    snprintf(text, sizeof(text), "%.*f", decimals, x);
    for (char* c = text; *c != '\0'; ++c) {
        if (*c == '.') *c = ',';
    }
    return text;
}

std::string sign(int v) { return (v > 0 ? "+" : "") + std::to_string(v); }

std::string ms(double x) {
    if (x < 1000) return std::to_string(static_cast<int>(lround(x))) + " ms";
    return num(x / 1000, x < 10000 ? 2 : 1) + " s";
}

std::string choice(const char* choices, int index) {
    std::string item;
    int i = 0;
    for (const char* c = choices;; ++c) {
        if (*c == '|' || *c == '\0') {
            if (i == index) return item;
            if (*c == '\0') return std::to_string(index);
            item.clear();
            ++i;
        } else {
            item += *c;
        }
    }
}

}  // namespace

std::string param_text(const Param& p, int v) {
    if (p.kind == Kind::Toggle) return v != 0 ? "an" : "aus";
    if (p.kind == Kind::Choice) return choice(p.choices, v);
    switch (p.format) {
        case Format::Semitones: return sign(v) + " HT";
        case Format::Cents: return sign(v) + " ct";
        case Format::Percent: return (p.min < 0 ? sign(v) : std::to_string(v)) + " %";
        case Format::Pulse: return num(v / 40.95, 0) + " %";
        case Format::SidAttack: return ms(kSidAttack[v & 15]);
        case Format::SidDecay: return ms(kSidDecay[v & 15]);
        case Format::Exp:
        case Format::Exp0: {
            const double x = curve(p, static_cast<int16_t>(v));
            const std::string unit = p.unit;
            if (unit == "ms") return ms(x);
            if (x == 0) return "aus";
            if (unit == "Hz") return num(x, x < 1 ? 2 : x < 10 ? 1 : 0) + " Hz";
            return std::to_string(static_cast<int>(lround(x))) + " " + unit;
        }
        case Format::Int:
            break;
    }
    return std::to_string(v);
}

}  // namespace synth
