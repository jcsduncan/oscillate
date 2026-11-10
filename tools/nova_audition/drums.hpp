#pragma once

#include <filesystem>
#include <string_view>

// The drums audition set: eight-bar grooves on Stick, one per factory
// preset, each at its own tempo with a fill closing every four bars and a
// crash ringing out at the end. Each groove lands in the output directory as
// "<NN groove> - <preset>.wav" in 32-bit float, unnormalised.
namespace oscillate::tools::drums {

inline constexpr std::string_view flag = "--drums";

void render_set(const std::filesystem::path& output);

} // namespace oscillate::tools::drums
