// Knock's patterns (ADR-0068, docs/ui/knock-mockup.html): the generator is
// seeded and follows density and each pad's role, locks hold through Roll
// and Mutate, the library is complete and names real kits, notes land on
// the pads' keys at their velocities and sixteenths, chance and ratchets
// resolve as documented, and Evolve keeps changing every four bars.

#include "oscillate/application/knock_patterns.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace patterns = oscillate::application::knock_patterns;
namespace knock = oscillate::native_dsp::synth::knock;
namespace synth = oscillate::native_dsp::synth;
using knock::PadRole;
using patterns::Cell;

using oscillate::test::require;

// Two low pads, three mid, three high: a typical kit.
constexpr patterns::Roles mixed_roles{PadRole::low, PadRole::low,  PadRole::mid,  PadRole::mid,
                                      PadRole::mid, PadRole::high, PadRole::high, PadRole::high};
constexpr patterns::Locks no_locks{};
constexpr std::uint32_t seeds = 24;

std::size_t lane_hits(const patterns::Pattern& pattern, PadRole role,
                      const patterns::Roles& roles = mixed_roles) {
  std::size_t count = 0;
  for (const auto& bar : pattern.bars)
    for (std::size_t lane = 0; lane < patterns::lane_count; ++lane)
      if (roles[lane] == role)
        count += patterns::hit_count(bar[lane]);
  return count;
}

void generation_is_seeded() {
  patterns::GeneratorSettings settings;
  settings.bars = 4;
  settings.seed = 7;
  const auto first = patterns::generate(settings, mixed_roles);
  REQUIRE_EQ(first.bars.size(), 4, "the loop has the bars asked for");
  REQUIRE_EQ(first, patterns::generate(settings, mixed_roles), "a seed gives the same loop");
  REQUIRE_GT(patterns::hit_count(first), 0, "a loop plays");
  settings.seed = 8;
  REQUIRE_NE(patterns::generate(settings, mixed_roles), first, "another seed gives another loop");
  // Bars round up to a choice and settings clamp.
  settings.bars = 3;
  settings.density = 4.0;
  const auto clamped = patterns::clamped(settings);
  REQUIRE_EQ(clamped.bars, 4, "settings are clamped");
  REQUIRE_EQ(clamped.density, 1.0, "settings are clamped");
}

void density_adds_hits() {
  std::size_t sparse = 0;
  std::size_t dense = 0;
  for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
    patterns::GeneratorSettings settings;
    settings.seed = seed;
    settings.density = 0.1;
    sparse += patterns::hit_count(patterns::generate(settings, mixed_roles));
    settings.density = 0.9;
    dense += patterns::hit_count(patterns::generate(settings, mixed_roles));
  }
  REQUIRE_GT(dense, sparse * 2, "density makes a busier loop");
}

// Top gives the high pads the sixteenths and leaves the low ones sparse;
// Underbelly is the other way round.
void roles_shape_the_parts() {
  std::size_t top_low = 0, top_high = 0, top_offbeat = 0, top_beat = 0;
  std::size_t under_low = 0, under_high = 0;
  for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
    patterns::GeneratorSettings settings;
    settings.seed = seed;
    settings.density = 1.0; // Every lane plays.
    settings.style = patterns::Style::top;
    const auto top = patterns::generate(settings, mixed_roles);
    top_low += lane_hits(top, PadRole::low);
    top_high += lane_hits(top, PadRole::high);
    for (const auto& bar : top.bars) {
      for (std::size_t lane = 5; lane < patterns::lane_count; ++lane) {
        for (std::size_t step = 0; step < patterns::steps_per_bar;
             step += patterns::steps_per_quarter) {
          top_beat += bar[lane][step] != Cell::off ? 1U : 0U;
          top_offbeat += bar[lane][step + 2] != Cell::off ? 1U : 0U;
        }
      }
    }
    settings.style = patterns::Style::underbelly;
    const auto under = patterns::generate(settings, mixed_roles);
    under_low += lane_hits(under, PadRole::low);
    under_high += lane_hits(under, PadRole::high);
  }
  // Per lane: two low lanes against three high ones.
  REQUIRE_GT(top_high * 2, top_low * 3 * 3, "Top gives the high pads the busy part");
  REQUIRE_GT(top_offbeat, top_beat * 2, "Top's high pads play the off-beat sixteenths");
  REQUIRE_GT(under_low * 3, under_high * 2 * 3, "Underbelly gives the low pads the busy part");
  // Low pads never take ghosts.
  for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
    patterns::GeneratorSettings settings;
    settings.seed = seed;
    settings.ghosts = 1.0;
    for (const auto& bar : patterns::generate(settings, mixed_roles).bars)
      for (std::size_t lane = 0; lane < 2; ++lane)
        require(std::ranges::none_of(bar[lane], [](Cell cell) { return cell == Cell::ghost; }),
                "a low pad has no ghosts");
  }
}

void locks_hold() {
  patterns::GeneratorSettings settings;
  settings.seed = 3;
  const auto current = patterns::generate(settings, mixed_roles);
  patterns::Locks locked{};
  locked[1] = true;
  locked[6] = true;
  settings.seed = 4;
  settings.bars = 4; // Longer than the current loop: its bars repeat.
  const auto rolled = patterns::roll(settings, mixed_roles, current, locked);
  REQUIRE_EQ(rolled.bars.size(), 4, "a roll has its own length");
  for (std::size_t bar = 0; bar < rolled.bars.size(); ++bar) {
    const auto& from = current.bars[bar % current.bars.size()];
    REQUIRE_EQ(rolled.bars[bar][1], from[1], "a roll keeps the locked lanes");
    REQUIRE_EQ(rolled.bars[bar][6], from[6], "a roll keeps the locked lanes");
  }
  REQUIRE_NE(rolled, patterns::roll(settings, mixed_roles, current, no_locks),
             "unlocked lanes roll");

  const auto mutated = patterns::mutate(rolled, patterns::mutate_amount, 11, locked);
  REQUIRE_NE(mutated, rolled, "a mutation changes something");
  std::size_t changed = 0;
  for (std::size_t bar = 0; bar < rolled.bars.size(); ++bar) {
    REQUIRE_EQ(mutated.bars[bar][1], rolled.bars[bar][1], "a mutation keeps the locked lanes");
    REQUIRE_EQ(mutated.bars[bar][6], rolled.bars[bar][6], "a mutation keeps the locked lanes");
    for (std::size_t lane = 0; lane < patterns::lane_count; ++lane)
      for (std::size_t step = 0; step < patterns::steps_per_bar; ++step)
        changed += mutated.bars[bar][lane][step] != rolled.bars[bar][lane][step] ? 1U : 0U;
  }
  const auto cells = rolled.bars.size() * patterns::lane_count * patterns::steps_per_bar;
  REQUIRE_LT(changed * 3, cells, "a mutation changes only a little");
  REQUIRE_EQ(mutated, patterns::mutate(rolled, patterns::mutate_amount, 11, locked),
             "a mutation is seeded");
  // Even a tiny amount shows; every lane locked keeps everything.
  REQUIRE_NE(patterns::mutate(rolled, 0.0, 1, no_locks), rolled, "a mutation always shows");
  patterns::Locks all{};
  all.fill(true);
  REQUIRE_EQ(patterns::mutate(rolled, 1.0, 1, all), rolled, "all locked: nothing changes");
}

void library_is_complete() {
  const auto& entries = patterns::library();
  const auto& table = *knock::engine().descriptor;
  REQUIRE_EQ(entries.size(),
             patterns::general_loop_count + patterns::loops_per_kit * table.presets.size(),
             "thirty loops and two per kit");
  std::set<std::string> names;
  for (const auto& entry : entries) {
    require(names.insert(entry.name).second, "loop names are unique: " + entry.name);
    REQUIRE_EQ(patterns::find_loop(entry.name), &entry, "a loop is found by name");
  }
  REQUIRE_EQ(patterns::find_loop("No such loop"), nullptr, "unknown loops are not found");
  for (const auto& preset : table.presets) {
    const auto kit_loops = std::ranges::count_if(
        entries, [&](const patterns::LibraryEntry& entry) { return entry.kit == preset.name; });
    REQUIRE_EQ(kit_loops, static_cast<std::ptrdiff_t>(patterns::loops_per_kit),
               "every kit has its two loops: " + std::string(preset.name));
    const auto values = synth::preset_values(table, preset);
    REQUIRE_OK(values, "kit values");
    const auto roles = patterns::roles_of(values.value());
    for (const auto& entry : entries) {
      if (entry.kit != preset.name)
        continue;
      const auto loop = patterns::loop_pattern(entry, roles);
      require(!loop.bars.empty() && patterns::hit_count(loop) > 0,
              "a kit loop plays: " + entry.name);
    }
  }
  for (const auto& entry : entries) {
    if (!entry.kit.empty())
      continue;
    require(!entry.fixed, "the thirty come from the generator");
    const auto loop = patterns::loop_pattern(entry, mixed_roles);
    require(std::ranges::find(patterns::bar_choices, loop.bars.size()) !=
                    patterns::bar_choices.end() &&
                patterns::hit_count(loop) > 0 && patterns::to_notes(loop, 1).has_value(),
            "a library loop is valid: " + entry.name);
  }
  // The mockup's hand-written Minimal top, row 3.
  const auto* minimal = patterns::find_loop("Minimal top");
  REQUIRE_NE(minimal, nullptr, "Minimal's top is hand-written");
  REQUIRE_TRUE(minimal->fixed, "Minimal's top is hand-written");
  REQUIRE_EQ(minimal->fixed->bars.size(), 1, "Minimal's top is hand-written");
  const auto& row = minimal->fixed->bars[0][2];
  REQUIRE_EQ(row[0], Cell::hit, "the hand-written rows are read cell for cell");
  REQUIRE_EQ(row[2], Cell::chance, "the hand-written rows are read cell for cell");
  REQUIRE_EQ(row[1], Cell::off, "the hand-written rows are read cell for cell");

  const std::array<std::string_view, 2> short_rows{"x...", "...."};
  require(!patterns::bar_from_rows(short_rows), "a malformed bar is refused");
}

void notes_land_on_pads() {
  patterns::Pattern pattern;
  patterns::Bar bar{};
  bar[0][0] = Cell::hit;
  bar[1][4] = Cell::accent;
  bar[2][8] = Cell::ghost;
  bar[7][15] = Cell::ratchet;
  pattern.bars.push_back(bar);
  pattern.bars.push_back(patterns::Bar{});
  const auto clip = patterns::to_notes(pattern, 1);
  REQUIRE_OK(clip, "the pattern writes notes");
  const auto& notes = clip.value().notes;
  const auto sixteenth = oscillate::timeline::ticks_per_quarter / 4;
  REQUIRE_EQ(clip.value().length.value, 2 * 16 * sixteenth, "the clip is two 4/4 bars");
  REQUIRE_EQ(notes.size(), 5, "a hit each, and a ratchet is two");
  REQUIRE_EQ(notes[0].key, knock::first_key, "a hit is a short note on its pad's key");
  REQUIRE_EQ(notes[0].start.value, 0, "a hit is a short note on its pad's key");
  REQUIRE_EQ(notes[0].velocity, patterns::hit_velocity, "a hit is a short note on its pad's key");
  REQUIRE_EQ(notes[0].end.value, sixteenth / 2, "a hit is a short note on its pad's key");
  REQUIRE_EQ(notes[1].key, knock::first_key + 1, "an accent is loud on its sixteenth");
  REQUIRE_EQ(notes[1].start.value, 4 * sixteenth, "an accent is loud on its sixteenth");
  REQUIRE_EQ(notes[1].velocity, patterns::accent_velocity, "an accent is loud on its sixteenth");
  REQUIRE_EQ(notes[2].key, knock::first_key + 2, "a ghost is soft");
  REQUIRE_EQ(notes[2].start.value, 8 * sixteenth, "a ghost is soft");
  REQUIRE_EQ(notes[2].velocity, patterns::ghost_velocity, "a ghost is soft");
  REQUIRE_EQ(notes[3].key, knock::first_key + 7, "a ratchet is two notes a thirty-second apart");
  REQUIRE_EQ(notes[4].key, knock::first_key + 7, "a ratchet is two notes a thirty-second apart");
  REQUIRE_EQ(notes[3].start.value, 15 * sixteenth, "a ratchet is two notes a thirty-second apart");
  REQUIRE_EQ(notes[4].start.value, 15 * sixteenth + sixteenth / 2,
             "a ratchet is two notes a thirty-second apart");
  REQUIRE_LE(notes[4].end.value, clip.value().length.value,
             "a ratchet is two notes a thirty-second apart");
  REQUIRE_GT(patterns::accent_velocity, patterns::hit_velocity,
             "accents are loudest, ghosts softest");
  REQUIRE_GT(patterns::hit_velocity, patterns::ghost_velocity,
             "accents are loudest, ghosts softest");
  require(!patterns::to_notes({}, 1).has_value(), "an empty pattern writes nothing");

  // An audition at 120 BPM and 48 kHz: a beat is 24 000 frames.
  const auto preview = patterns::preview_notes(clip.value(), 120.0, 48'000, 3);
  REQUIRE_EQ(preview.size(), 3, "an audition plays the first notes at the tempo");
  REQUIRE_EQ(preview[0].start_frame, 0, "an audition plays the first notes at the tempo");
  REQUIRE_EQ(preview[1].start_frame, 24'000, "an audition plays the first notes at the tempo");
  REQUIRE_EQ(preview[1].key, notes[1].key, "an audition plays the first notes at the tempo");
  REQUIRE_EQ(preview[1].velocity, notes[1].velocity,
             "an audition plays the first notes at the tempo");
  REQUIRE_EQ(preview[0].length_frames, 3'000, "an audition plays the first notes at the tempo");
}

void chance_is_seeded() {
  patterns::Pattern pattern;
  patterns::Bar bar{};
  bar[3].fill(Cell::chance);
  pattern.bars.assign(4, bar);
  const auto first = patterns::to_notes(pattern, 5);
  REQUIRE_OK(first, "chance notes");
  const auto again = patterns::to_notes(pattern, 5);
  REQUIRE_OK(again, "chance resolves the same for a seed");
  REQUIRE_EQ(again.value().notes, first.value().notes, "chance resolves the same for a seed");
  const auto played = first.value().notes.size();
  const auto cells = 4 * patterns::steps_per_bar;
  REQUIRE_GT(played * 4, cells, "chance plays about half");
  REQUIRE_LT(played * 4, cells * 3, "chance plays about half");
  const auto other = patterns::to_notes(pattern, 6);
  REQUIRE_OK(other, "another seed resolves differently");
  REQUIRE_NE(other.value().notes, first.value().notes, "another seed resolves differently");
}

void evolve_keeps_moving() {
  constexpr std::size_t evolve_bars = 16;
  constexpr std::uint32_t evolve_seed = 21;
  patterns::GeneratorSettings settings;
  settings.seed = 9;
  const auto loop = patterns::generate(settings, mixed_roles); // Two bars.
  patterns::Locks locked{};
  locked[0] = true;
  const auto evolved =
      patterns::evolve(loop, {.total_bars = evolve_bars, .seed = evolve_seed}, locked);
  REQUIRE_EQ(evolved.bars.size(), 16, "Evolve writes the bars asked for");
  for (std::size_t bar = 0; bar < patterns::evolve_every_bars; ++bar)
    REQUIRE_EQ(evolved.bars[bar], loop.bars[bar % loop.bars.size()],
               "the first four bars are the loop");
  for (std::size_t block = 1; block * patterns::evolve_every_bars < evolved.bars.size(); ++block) {
    bool differs = false;
    for (std::size_t bar = 0; bar < patterns::evolve_every_bars; ++bar) {
      const auto at = block * patterns::evolve_every_bars + bar;
      differs = differs || evolved.bars[at] != evolved.bars[at - patterns::evolve_every_bars];
    }
    require(differs, "every four bars it changes");
  }
  for (std::size_t bar = 0; bar < evolved.bars.size(); ++bar)
    REQUIRE_EQ(evolved.bars[bar][0], loop.bars[bar % loop.bars.size()][0],
               "a locked lane never evolves");
  REQUIRE_EQ(evolved,
             patterns::evolve(loop, {.total_bars = evolve_bars, .seed = evolve_seed}, locked),
             "Evolve is seeded");
  REQUIRE_EQ(patterns::to_notes(evolved, 1).value().length.value,
             static_cast<std::int64_t>(evolve_bars) * patterns::bar_ticks.value,
             "an evolved clip is its bars long");
}

} // namespace

int main() {
  try {
    generation_is_seeded();
    density_adds_hits();
    roles_shape_the_parts();
    locks_hold();
    library_is_complete();
    notes_land_on_pads();
    chance_is_seeded();
    evolve_keeps_moving();
  } catch (const std::exception& error) {
    std::cerr << "knock patterns: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
