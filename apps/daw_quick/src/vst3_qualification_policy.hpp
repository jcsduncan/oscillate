#pragma once

#include "routed_playback.hpp"

namespace oscillate::quick {

// Evidence-backed compatibility data for the generic VST3 manual-event path.
// A record grants only its exact binary, class, control, rate and block size.
[[nodiscard]] Vst3LiveQualification verified_vst3_live_qualification();

} // namespace oscillate::quick
