// Ein Rohwert so, wie ihn der Web-Editor der Box anzeigt (show() in index.html):
// Halbtöne, Cent, Prozent, SID-Hüllkurvenzeiten, Millisekunden, Hertz.

#pragma once

#include <string>

#include "params.h"

namespace synth {

std::string param_text(const Param& p, int raw);

}  // namespace synth
