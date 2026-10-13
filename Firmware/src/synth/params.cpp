#include "params.h"

#include <math.h>
#include <string.h>

namespace synth {

#define SYNTH_PARAM_ROW(id, key, label, group, kind, min, max, def, format, choices, lo, hi, \
                        unit)                                                              \
    {key, label, group, Kind::kind, min, max, def, Format::format, choices, lo, hi, unit},

const Param kParams[kParamCount] = {SYNTH_PARAMS(SYNTH_PARAM_ROW)};

#undef SYNTH_PARAM_ROW

int find_param(const char* key) {
    for (size_t i = 0; i < kParamCount; ++i) {
        if (strcmp(kParams[i].key, key) == 0) return static_cast<int>(i);
    }
    return -1;
}

int16_t clamp_value(size_t index, long value) {
    const Param& p = kParams[index];
    if (value < p.min) return p.min;
    if (value > p.max) return p.max;
    return static_cast<int16_t>(value);
}

float curve(const Param& p, int16_t raw) {
    const float span = static_cast<float>(p.max - p.min);
    if (p.format == Format::Exp) {
        return p.lo * powf(p.hi / p.lo, (raw - p.min) / span);
    }
    if (p.format == Format::Exp0) {
        if (raw <= p.min) return 0.0f;
        return p.lo * powf(p.hi / p.lo, (raw - p.min - 1) / (span - 1.0f));
    }
    return static_cast<float>(raw);
}

int16_t raw_for(const Param& p, float value) {
    const float span = static_cast<float>(p.max - p.min);
    float position;
    if (p.format == Format::Exp) {
        position = p.min + span * logf(value / p.lo) / logf(p.hi / p.lo);
    } else if (p.format == Format::Exp0) {
        if (value <= 0.0f) return p.min;
        position = p.min + 1 + (span - 1.0f) * logf(value / p.lo) / logf(p.hi / p.lo);
    } else {
        position = value;
    }
    const long rounded = lroundf(position);
    if (rounded < p.min) return p.min;
    if (rounded > p.max) return p.max;
    return static_cast<int16_t>(rounded);
}

const char* format_name(Format format) {
    switch (format) {
        case Format::Int: return "int";
        case Format::Semitones: return "semi";
        case Format::Cents: return "cent";
        case Format::Percent: return "pct";
        case Format::Pulse: return "pulse";
        case Format::SidAttack: return "sida";
        case Format::SidDecay: return "sidd";
        case Format::Exp: return "exp";
        case Format::Exp0: return "exp0";
    }
    return "int";
}

void set_name(Patch& patch, const char* text) {
    size_t n = 0;
    for (const char* c = text; *c != '\0' && n < kNameLength - 1; ++c) {
        if (static_cast<uint8_t>(*c) >= 0x20) patch.name[n++] = *c;
    }
    // Ein beim Kürzen zerschnittenes UTF-8-Zeichen ganz weglassen, ein vollständiges nicht.
    size_t lead = n;
    while (lead > 0 && (static_cast<uint8_t>(patch.name[lead - 1]) & 0xC0) == 0x80) --lead;
    if (lead > 0) {
        const uint8_t first = static_cast<uint8_t>(patch.name[lead - 1]);
        const size_t size = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
        if (n - (lead - 1) < size) n = lead - 1;
    }
    patch.name[n] = '\0';
}

void init_patch(Patch& patch, const char* name) {
    memset(&patch, 0, sizeof(patch));
    set_name(patch, name);
    for (size_t i = 0; i < kParamCount; ++i) patch.values[i] = kParams[i].def;
}

}  // namespace synth
