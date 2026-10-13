// Die Werksklänge auf den ersten Plätzen, siehe presets.h.

#pragma once

#include "params.h"

namespace synth {
namespace presets {

constexpr int kSlots = 32;

/// Der Werksklang dieses Platzes (ab 0). false, wenn der Platz keinen hat.
bool factory(int slot, Patch& patch);

}  // namespace presets
}  // namespace synth
