#pragma once

#include <filesystem>
#include <string_view>

// The breakdown audition set, at 120 BPM in C minor: the picked pads alone
// for four bars, a two-bar build (A: the pads' reverb frozen and faded
// under a noise riser; B: the pads swelling back as if reversed, with a
// soft snare roll), then eight bars of a candidate over a four-to-the-floor
// kick, once after each build. Each clip lands in the output directory as
// "<NN candidate> - <preset> (A riser).wav" or "... (B swell).wav" in 32-bit
// float, unnormalised.
namespace oscillate::tools::breakdown {

inline constexpr std::string_view flag = "--breakdown";
// After the output directory: also render each instrument track alone.

void render_set(const std::filesystem::path& output, bool stems);

} // namespace oscillate::tools::breakdown
