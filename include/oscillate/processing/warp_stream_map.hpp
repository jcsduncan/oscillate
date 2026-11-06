#pragma once

#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <cstdint>

namespace oscillate::processing {

// Q48.16 position, in the project-rate view of the source, of an absolute
// source-rate frame: round(frame * P * 65536 / S), half up, in exact integer
// arithmetic (ADR-0026 point 5). Equal rates give frame * 65536.
[[nodiscard]] foundation::Result<std::uint64_t>
project_rate_source_position(std::uint64_t source_frame, std::uint32_t source_rate,
                             std::uint32_t project_rate);

// Worker/control-side adapter from the authoritative clip frame plan to a
// warp_dsp map. The plan has already resolved placement and musical markers
// through the shared tempo map; this only changes coordinates. Output frames
// stay clip-local project-rate frames. Source positions are absolute in the
// project-rate view of the source (warp_dsp::RateConvertedSourceView), so a
// stream reads a different-rate source at its own pitch and duration.
[[nodiscard]] foundation::Result<warp_dsp::WarpTimeMap>
make_warp_stream_map(const ClipFramePlan& plan, std::uint32_t source_rate,
                     std::uint32_t project_rate);

} // namespace oscillate::processing
