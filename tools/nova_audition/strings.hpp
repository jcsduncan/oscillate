#pragma once

#include <filesystem>
#include <string_view>

// The strings audition set: the three picked pads, looped to sixteen bars
// at 120 BPM as in the breakdown keys, with a Bow arrangement on top, each
// section on its own track through Endless Cloud. Each clip lands in the
// output directory as "<NN idea> - <preset>.wav" in 32-bit float,
// unnormalised, beside "<NN idea> - <section>.mid" for each section's part,
// with its dynamics (CC1) and keyswitches.
namespace oscillate::tools::strings {

inline constexpr std::string_view flag = "--strings";

// `stems`: also render each instrument track alone.
void render_set(const std::filesystem::path& output, bool stems);

} // namespace oscillate::tools::strings
