#pragma once

#include <cstdint>

// Clay's edit bounds (ADR-0081): part of the persisted take contract, so the
// project model owns them. clay_dsp checks the same bounds through
// oscillate/clay_dsp/limits.hpp. Header-only, constants only.
namespace oscillate::project {

inline constexpr double max_clay_edit_cents = 4'800.0;
inline constexpr double max_clay_modulation_scale = 4.0;
inline constexpr double max_clay_drift_scale = 4.0;
inline constexpr double max_clay_edit_gain_db = 24.0;
inline constexpr double min_clay_edit_gain_db = -96.0;
// A placed note may be stretched or squeezed by at most this factor.
inline constexpr double max_clay_time_scale = 8.0;

// The analysis a saved take's note ids refer to. Bump it whenever detection
// or note ids change: a take saved against another version is refused.
inline constexpr std::uint32_t clay_analysis_version = 1;

} // namespace oscillate::project
