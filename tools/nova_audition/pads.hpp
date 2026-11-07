#pragma once

#include <filesystem>
#include <string_view>

// The pad audition set: each pad plays a slow, sparse part under a flute
// lead (Wren playing a Standard MIDI File), through an Expanse insert, and
// again alone. Files land in the output directory as
// "<NN direction> - <preset>.wav" and "... (pad alone).wav".
namespace oscillate::tools::pads {

inline constexpr std::string_view flag = "--pads";

void render_set(const std::filesystem::path& output, const std::filesystem::path& lead_midi);

} // namespace oscillate::tools::pads
