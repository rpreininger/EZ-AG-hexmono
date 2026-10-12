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

void init_patch(Patch& patch, const char* name) {
    memset(&patch, 0, sizeof(patch));
    strncpy(patch.name, name, kNameLength - 1);
    for (size_t i = 0; i < kParamCount; ++i) patch.values[i] = kParams[i].def;
}

}  // namespace synth
