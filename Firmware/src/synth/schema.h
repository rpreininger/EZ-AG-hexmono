// Die Parametertabelle als JSON, so wie der Editor sie unter /api/schema abholt.
//
// Als Vorlage, damit Box (Arduino-Print) und Mac-App (eigener Puffer) dieselbe Ausgabe
// erzeugen. `Out` braucht nur print(const char*) und print(char).

#pragma once

#include <stdio.h>

#include "factory.h"
#include "params.h"

namespace synth {
namespace schema_detail {

template <typename Out>
void print_string(Out& out, const char* text) {
    out.print('"');
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') out.print('\\');
        out.print(*c);
    }
    out.print('"');
}

inline const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Range: return "range";
        case Kind::Choice: return "choice";
        case Kind::Toggle: return "toggle";
    }
    return "range";
}

}  // namespace schema_detail

template <typename Out>
void write_schema(Out& out) {
    using schema_detail::print_string;
    char text[96];
    snprintf(text, sizeof(text), "{\"slots\":%d,\"params\":[", presets::kSlots);
    out.print(text);
    for (size_t i = 0; i < kParamCount; ++i) {
        const Param& p = kParams[i];
        if (i > 0) out.print(',');
        out.print("{\"key\":");
        print_string(out, p.key);
        out.print(",\"label\":");
        print_string(out, p.label);
        out.print(",\"group\":");
        print_string(out, p.group);
        snprintf(text, sizeof(text), ",\"kind\":\"%s\",\"min\":%d,\"max\":%d,\"def\":%d,\"fmt\":\"%s\"",
                 schema_detail::kind_name(p.kind), p.min, p.max, p.def, format_name(p.format));
        out.print(text);
        if (p.kind == Kind::Choice) {
            out.print(",\"choices\":[\"");
            for (const char* c = p.choices; *c != '\0'; ++c) {
                if (*c == '|') {
                    out.print("\",\"");
                } else {
                    out.print(*c);
                }
            }
            out.print("\"]");
        }
        if (p.format == Format::Exp || p.format == Format::Exp0) {
            snprintf(text, sizeof(text), ",\"lo\":%g,\"hi\":%g,\"unit\":\"%s\"", p.lo, p.hi, p.unit);
            out.print(text);
        }
        out.print('}');
    }
    out.print("]}");
}

}  // namespace synth
