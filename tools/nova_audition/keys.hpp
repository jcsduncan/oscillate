#pragma once

#include <filesystem>
#include <string_view>

// The keys audition set: the three picked pads (Two Flutes, Warm Sub Bed
// and Ney Breath, as in the pad auditions) for their twelve bars at
// 120 BPM, with one quiet Concert part on top, played above the pads so it
// doesn't fight them. Each clip lands in the output directory as
// "<NN idea> - <preset>.wav" in 32-bit float, unnormalised, beside
// "<NN idea> - <preset> (piano).mid", the piano part alone with its pedals.
namespace oscillate::tools::keys {

inline constexpr std::string_view flag = "--keys";
// The second round, for the breakdown: sixteen bars, the pads looped, and
// parts with more melody and quiet chords under it.
inline constexpr std::string_view breakdown_flag = "--keys2";

enum class Round : unsigned char { first, breakdown };

// `stems`: also render each instrument track alone.
void render_set(const std::filesystem::path& output, bool stems, Round round);

} // namespace oscillate::tools::keys
