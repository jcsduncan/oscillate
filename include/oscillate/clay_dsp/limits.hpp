#pragma once

#include "oscillate/project/clay_limits.hpp"

#include <cstdint>

// Clay's edit bounds under the DSP's names. The values are the persisted take
// contract in oscillate/project/clay_limits.hpp (header-only, constants only).
namespace oscillate::clay_dsp {

inline constexpr double max_edit_cents = project::max_clay_edit_cents;
inline constexpr double max_modulation_scale = project::max_clay_modulation_scale;
inline constexpr double max_drift_scale = project::max_clay_drift_scale;
inline constexpr double max_edit_gain_db = project::max_clay_edit_gain_db;
inline constexpr double min_edit_gain_db = project::min_clay_edit_gain_db;
inline constexpr double max_time_scale = project::max_clay_time_scale;
inline constexpr std::uint32_t analysis_version = project::clay_analysis_version;

} // namespace oscillate::clay_dsp
