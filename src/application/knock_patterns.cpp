#include "oscillate/application/knock_patterns.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

namespace oscillate::application::knock_patterns {
namespace {

// ---- Seeded randomness --------------------------------------------------------

// Mulberry32, as the mockup's: the same seed gives the same draws in the
// browser and here.
class Random {
public:
  explicit Random(std::uint32_t seed) noexcept : state_(seed) {}
  // Uniform in [0, 1).
  double next() noexcept {
    state_ += 0x6D2B'79F5U;
    std::uint32_t t = state_;
    t = (t ^ (t >> 15U)) * (t | 1U);
    t ^= t + (t ^ (t >> 7U)) * (t | 61U);
    return static_cast<double>(t ^ (t >> 14U)) / two_to_32;
  }
  // Uniform in 0..count-1 (count > 0).
  std::size_t below(std::size_t count) noexcept {
    const auto pick = static_cast<std::size_t>(next() * static_cast<double>(count));
    return std::min(pick, count - 1);
  }

private:
  static constexpr double two_to_32 = 4'294'967'296.0;
  std::uint32_t state_;
};

// Knuth's multiplicative hash spreads small seeds (the mockup's own mix).
constexpr std::uint32_t seed_multiplier = 2'654'435'761U;
// A second odd constant keeps a salt's bits apart from the seed's.
constexpr std::uint32_t salt_multiplier = 0x85EB'CA6BU;
// Salts so mutation, evolution and chance draws never share a stream.
constexpr std::uint32_t mutate_salt = 0x4D55'5441U;
constexpr std::uint32_t chance_salt = 0x4348'4E43U;

constexpr std::uint32_t mixed(std::uint32_t seed) noexcept { return seed * seed_multiplier; }
constexpr std::uint32_t mixed(std::uint32_t seed, std::uint32_t salt) noexcept {
  return (seed * seed_multiplier) ^ (salt * salt_multiplier);
}

// ---- Styles --------------------------------------------------------------------

using Weights = std::array<double, steps_per_bar>;
// A one-beat figure repeated over the bar.
constexpr Weights per_beat(double a, double b, double c, double d) noexcept {
  return {a, b, c, d, a, b, c, d, a, b, c, d, a, b, c, d};
}

// How likely each sixteenth is, per role (low, mid, high), from the mockup.
struct StyleWeights {
  Weights low;
  Weights mid;
  Weights high;
};
constexpr StyleWeights top_weights{
    per_beat(0.03, 0.02, 0.03, 0.02),
    {0, 0, 0.25, 0.1, 0, 0.1, 0.55, 0.15, 0, 0.05, 0.35, 0.2, 0, 0.15, 0.6, 0.2},
    per_beat(0.15, 0.35, 0.85, 0.35)};
constexpr StyleWeights underbelly_weights{
    {0.15, 0.25, 0.45, 0.55, 0.05, 0.5, 0.35, 0.6, 0.1, 0.3, 0.55, 0.45, 0.05, 0.5, 0.4, 0.65},
    per_beat(0, 0.2, 0.12, 0.3), per_beat(0.04, 0.04, 0.06, 0.04)};
constexpr StyleWeights rolling_weights{per_beat(0.2, 0.3, 0.25, 0.35),
                                       per_beat(0.35, 0.45, 0.4, 0.5),
                                       per_beat(0.8, 0.7, 0.85, 0.7)};
constexpr StyleWeights skippy_weights{
    {0.1, 0, 0.3, 0.5, 0, 0.2, 0.1, 0.55, 0, 0.1, 0.35, 0.5, 0, 0.25, 0.15, 0.6},
    {0, 0.1, 0.2, 0.6, 0.1, 0, 0.4, 0.5, 0, 0.15, 0.2, 0.55, 0.1, 0, 0.45, 0.4},
    per_beat(0.3, 0.15, 0.6, 0.5)};

const Weights& weights_for(const StyleWeights& style, knock::PadRole role) noexcept {
  switch (role) {
  case knock::PadRole::low:
    return style.low;
  case knock::PadRole::mid:
    return style.mid;
  case knock::PadRole::high:
    return style.high;
  }
  return style.mid;
}

// Broken: a Euclidean rhythm of so many hits per role, rotated.
constexpr std::array<std::size_t, 3> euclid_high_hits{5, 7, 9};
constexpr std::array<std::size_t, 2> euclid_mid_hits{3, 5};
constexpr std::array<std::size_t, 2> euclid_low_hits{3, 4};
constexpr double euclid_on_weight = 0.95;
constexpr double euclid_off_weight = 0.03;

// `hits` spread evenly over the bar, turned by `rotation` steps.
struct EuclidRhythm {
  std::size_t hits = 0;
  std::size_t rotation = 0;
};
Weights euclid(EuclidRhythm rhythm) noexcept {
  const auto [hits, rotation] = rhythm;
  Weights out{};
  for (std::size_t step = 0; step < steps_per_bar; ++step) {
    const std::size_t from = (step + rotation) % steps_per_bar;
    out[step] = (from * hits) % steps_per_bar < hits ? euclid_on_weight : euclid_off_weight;
  }
  return out;
}

// ---- The generator's shape (the mockup's generateBar) ---------------------------

// A sparse loop leaves room: at least this many lanes play, density adds
// up to `density_lanes` more.
constexpr std::size_t base_lanes = 3;
constexpr double density_lanes = 5.0;
// Syncopation lifts the off-beats and lowers the beats.
constexpr double offbeat_lift = 0.8;
constexpr double beat_drop = 0.6;
// A hit's chance is weight × (floor + density × gain), at most the cap.
constexpr double density_floor = 0.35;
constexpr double density_gain = 1.3;
constexpr double max_hit_probability = 0.97;
// Strong steps (weight above this) are accented this often.
constexpr double accent_weight = 0.5;
constexpr double accent_probability = 0.45;
// The settings' share that turns into ratchets, chances and ghosts.
constexpr double ratchet_share = 0.5;
constexpr double chance_share = 0.6;
constexpr double ghost_share = 0.18;
// Later bars take this share of a fresh bar's cells per Variation; the last
// bar fills its final beat when Variation is past half.
constexpr double variation_share = 0.35;
constexpr double fill_share = 0.9;
constexpr double fill_density = 0.3;
constexpr double fill_threshold = 0.5;
constexpr std::size_t fill_from_step = 12;
// Mutate's odds for a changed cell (the mockup's).
constexpr double drop_probability = 0.5;
constexpr double add_probability = 0.5;
constexpr double add_chance_probability = 0.3;

bool off_beat(std::size_t step) noexcept {
  return step % 2 == 1 || step % steps_per_quarter == 2;
}

Bar generate_bar(const GeneratorSettings& settings, const Roles& roles, Random& random) {
  Bar rows{};
  for (auto& lane : rows)
    lane.fill(Cell::off);
  std::array<std::size_t, lane_count> order{};
  std::iota(order.begin(), order.end(), std::size_t{0});
  for (std::size_t i = lane_count - 1; i > 0; --i)
    std::swap(order[i], order[random.below(i + 1)]);
  const auto playing = std::min(
      lane_count, base_lanes + static_cast<std::size_t>(std::lround(settings.density * density_lanes)));
  std::array<bool, lane_count> active{};
  for (std::size_t i = 0; i < playing; ++i)
    active[order[i]] = true;

  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    if (!active[lane])
      continue;
    const auto role = roles[lane];
    Weights weights{};
    switch (settings.style) {
    case Style::broken: {
      std::size_t hits = 0;
      if (role == knock::PadRole::high)
        hits = euclid_high_hits[random.below(euclid_high_hits.size())];
      else if (role == knock::PadRole::mid)
        hits = euclid_mid_hits[random.below(euclid_mid_hits.size())];
      else
        hits = euclid_low_hits[random.below(euclid_low_hits.size())];
      weights = euclid({.hits = hits, .rotation = random.below(steps_per_bar)});
      break;
    }
    case Style::top:
      weights = weights_for(top_weights, role);
      break;
    case Style::underbelly:
      weights = weights_for(underbelly_weights, role);
      break;
    case Style::rolling:
      weights = weights_for(rolling_weights, role);
      break;
    case Style::skippy:
      weights = weights_for(skippy_weights, role);
      break;
    }
    for (std::size_t step = 0; step < steps_per_bar; ++step) {
      const double lift = off_beat(step) ? offbeat_lift : -beat_drop;
      const double weight = weights[step] * (1.0 + settings.syncopation * lift);
      const double probability = std::clamp(
          weight * (density_floor + density_gain * settings.density), 0.0, max_hit_probability);
      auto& cell = rows[lane][step];
      if (random.next() < probability) {
        cell = weights[step] > accent_weight && random.next() < accent_probability ? Cell::accent
                                                                                   : Cell::hit;
        if (role == knock::PadRole::high && random.next() < settings.ratchets * ratchet_share)
          cell = Cell::ratchet;
        else if (random.next() < settings.chance * chance_share)
          cell = Cell::chance;
      } else if (random.next() < settings.ghosts * ghost_share && role != knock::PadRole::low) {
        cell = Cell::ghost;
      }
    }
  }
  return rows;
}

Cell mutated(Cell cell, Random& random) noexcept {
  if (cell != Cell::off) {
    if (random.next() < drop_probability)
      return Cell::off;
    return cell == Cell::hit ? Cell::accent : Cell::hit;
  }
  if (random.next() < add_probability)
    return Cell::hit;
  return random.next() < add_chance_probability ? Cell::chance : Cell::off;
}

// ---- The library's data -------------------------------------------------------

using Rows = std::array<std::string_view, lane_count>;
struct HandWritten {
  std::string_view kit;
  Rows top;
  Rows under;
};
// The mockup's own loops for the first four kits (KITS[...].patterns).
constexpr std::array<HandWritten, 4> hand_written{{
    {"Minimal",
     {"......x.....?...", "...x.......x..x.", "x.?.x.x.?.x.x.?x", "..x...x...x...x.",
      "..........X.....", "................", "x.X.x.X.x.X.x.Xx", "...............?"},
     {".....x........x.", "x..x..x...x..x..", "................", "....?.......?...",
      "................", "..x.....x?..r...", "................", "........?......."}},
    {"Tech House",
     {"..x.....x..x....", "x.....x...x...x.", "...x.?.....x.?.x", "x..x..x...x.x...",
      "....x.......x..?", "xxXxxxXxxxXxxxXx", "....X.......X...", "..........?....."},
     {"x..x..x...x..x..", "..x.....x.....x.", "................", "................",
      "....?.......?...", "................", "................", "......x.......x."}},
    {"Dub Techno",
     {"................", "......x.........", "...x......x...?.", "..........X.....",
      "..x...x...x...x.", "...............?", "................", "......?........."},
     {"x.....x...x.....", "...x......?..x..", "................", "................",
      "................", "................", "..x.....x.?...x.", "............x..."}},
    {"Organic",
     {"................", "...x......x.....", "x.?.x.?Xx.?.x.r.", "..x...x...x..?x.",
      "......?.......X.", "................", "x.x.X.x.x.x.X.x?", "............?..."},
     {"..x.....x?....x.", "x.....x...?.x...", "................", "....?.......?...",
      "................", ".x..X..x.x..X..r", "................", "................"}},
}};

// The other kits' two loops, from the generator with settings that suit
// each kit's sounds.
struct GeneratedKit {
  std::string_view kit;
  GeneratorSettings top;
  GeneratorSettings under;
};
constexpr std::array<GeneratedKit, 8> generated_kits{{
    {"Microhouse Clicks",
     {Style::top, 0.55, 0.5, 0.45, 0.35, 0.25, 0.3, 2, 211},
     {Style::underbelly, 0.4, 0.45, 0.3, 0.15, 0.2, 0.3, 2, 212}},
    {"Afro Percussion",
     {Style::rolling, 0.6, 0.55, 0.4, 0.2, 0.1, 0.35, 2, 223},
     {Style::underbelly, 0.55, 0.5, 0.35, 0.1, 0.1, 0.3, 2, 224}},
    {"Kitchen Foley",
     {Style::skippy, 0.45, 0.45, 0.4, 0.2, 0.2, 0.45, 2, 235},
     {Style::underbelly, 0.4, 0.4, 0.3, 0.1, 0.2, 0.4, 2, 236}},
    {"Glitch Crush",
     {Style::broken, 0.5, 0.6, 0.3, 0.5, 0.3, 0.5, 2, 247},
     {Style::underbelly, 0.4, 0.5, 0.2, 0.3, 0.25, 0.4, 2, 248}},
    {"Deep Underbelly",
     {Style::top, 0.35, 0.35, 0.25, 0.1, 0.15, 0.3, 2, 259},
     {Style::underbelly, 0.6, 0.5, 0.3, 0.1, 0.15, 0.35, 2, 260}},
    {"Rubber Bounce",
     {Style::skippy, 0.5, 0.5, 0.35, 0.15, 0.15, 0.35, 2, 271},
     {Style::underbelly, 0.5, 0.55, 0.3, 0.1, 0.1, 0.3, 2, 272}},
    {"Metal Workshop",
     {Style::rolling, 0.45, 0.4, 0.3, 0.25, 0.15, 0.3, 2, 283},
     {Style::underbelly, 0.45, 0.4, 0.25, 0.1, 0.2, 0.35, 2, 284}},
    {"Glass Garden",
     {Style::top, 0.4, 0.45, 0.45, 0.2, 0.25, 0.45, 2, 295},
     {Style::underbelly, 0.35, 0.4, 0.35, 0.1, 0.2, 0.4, 2, 296}},
}};
constexpr std::string_view top_suffix = " top";
constexpr std::string_view under_suffix = " underbelly";

// The thirty, six per style in the mockup's order, named by the mockup.
constexpr std::size_t loops_per_style = general_loop_count / style_count;
constexpr std::array<std::array<std::string_view, loops_per_style>, style_count> loop_names{{
    {"Tin Roof", "Night Bus", "Hi-Vis", "Glass Stairs", "Pocket Change", "Rain Gutter"},
    {"Boiler Room", "Low Ceiling", "Basement Pipes", "Sub Rattle", "Engine Hum", "Cellar Door"},
    {"Conveyor", "Loose Gravel", "Freight", "Rolling Pin", "Spin Cycle", "Ticker Tape"},
    {"Crooked Steps", "Off Axis", "Wonky Table", "Fault Line", "Bent Fork", "Odd Socks"},
    {"Hopscotch", "Skip Rope", "Pebble Skim", "Stutter", "Tap Shoe", "Pogo"},
}};

// The mockup's settings for loop k of style s: each setting walks its own
// small cycle, so neighbours differ.
GeneratorSettings general_settings(std::size_t style, std::size_t k) noexcept {
  constexpr double density_from = 0.3, density_step = 0.1;
  constexpr double sync_from = 0.2, sync_step = 0.12;
  constexpr std::size_t sync_cycle = 5;
  constexpr double ghosts_from = 0.2, ghosts_step = 0.1;
  constexpr std::size_t ghosts_cycle = 3;
  constexpr double ratchets_from = 0.1, ratchets_step = 0.08;
  constexpr std::size_t ratchets_cycle = 4;
  constexpr double chance_step = 0.1;
  constexpr std::size_t chance_cycle = 3;
  constexpr double variation_from = 0.2, variation_step = 0.15;
  constexpr std::size_t variation_cycle = 4;
  constexpr std::size_t long_cycle = 3, long_at = 2, long_bars = 4, short_bars = 2;
  constexpr std::uint32_t seed_from = 101, seed_per_style = 37, seed_per_loop = 11;
  const auto at = [](std::size_t value) { return static_cast<double>(value); };
  GeneratorSettings settings;
  settings.style = static_cast<Style>(style);
  settings.density = density_from + density_step * at(k);
  settings.syncopation = sync_from + sync_step * at((k + style) % sync_cycle);
  settings.ghosts = ghosts_from + ghosts_step * at(k % ghosts_cycle);
  settings.ratchets = ratchets_from + ratchets_step * at(k % ratchets_cycle);
  settings.chance = chance_step * at(k % chance_cycle);
  settings.variation = variation_from + variation_step * at(k % variation_cycle);
  settings.bars = k % long_cycle == long_at ? long_bars : short_bars;
  settings.seed = seed_from + seed_per_style * static_cast<std::uint32_t>(style) +
                  seed_per_loop * static_cast<std::uint32_t>(k);
  return settings;
}

Pattern fixed_pattern(const Rows& rows) {
  Pattern pattern;
  if (auto bar = bar_from_rows(rows))
    pattern.bars.push_back(*bar);
  return pattern;
}

std::vector<LibraryEntry> build_library() {
  std::vector<LibraryEntry> entries;
  entries.reserve(general_loop_count + loops_per_kit * (hand_written.size() + generated_kits.size()));
  for (const auto& kit : hand_written) {
    entries.push_back({std::string(kit.kit) + std::string(top_suffix), std::string(kit.kit),
                       Style::top, fixed_pattern(kit.top), default_settings});
    entries.push_back({std::string(kit.kit) + std::string(under_suffix), std::string(kit.kit),
                       Style::underbelly, fixed_pattern(kit.under), default_settings});
  }
  for (const auto& kit : generated_kits) {
    entries.push_back({std::string(kit.kit) + std::string(top_suffix), std::string(kit.kit),
                       kit.top.style, std::nullopt, kit.top});
    entries.push_back({std::string(kit.kit) + std::string(under_suffix), std::string(kit.kit),
                       kit.under.style, std::nullopt, kit.under});
  }
  for (std::size_t style = 0; style < style_count; ++style) {
    for (std::size_t k = 0; k < loops_per_style; ++k) {
      const auto settings = general_settings(style, k);
      entries.push_back({std::string(loop_names[style][k]), {}, settings.style, std::nullopt,
                         settings});
    }
  }
  return entries;
}

} // namespace

// ---- Cells -----------------------------------------------------------------------

Cell cell_from_letter(char letter) noexcept {
  switch (letter) {
  case 'x':
    return Cell::hit;
  case 'X':
    return Cell::accent;
  case 'g':
    return Cell::ghost;
  case '?':
    return Cell::chance;
  case 'r':
    return Cell::ratchet;
  default:
    return Cell::off;
  }
}

std::optional<Bar> bar_from_rows(std::span<const std::string_view> rows) {
  if (rows.size() != lane_count)
    return std::nullopt;
  Bar bar{};
  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    if (rows[lane].size() != steps_per_bar)
      return std::nullopt;
    for (std::size_t step = 0; step < steps_per_bar; ++step)
      bar[lane][step] = cell_from_letter(rows[lane][step]);
  }
  return bar;
}

std::size_t hit_count(const Lane& lane) noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(lane, [](Cell cell) { return cell != Cell::off; }));
}

std::size_t hit_count(const Pattern& pattern) noexcept {
  std::size_t count = 0;
  for (const auto& bar : pattern.bars)
    for (const auto& lane : bar)
      count += hit_count(lane);
  return count;
}

// ---- The generator ----------------------------------------------------------------

std::string_view style_name(Style style) noexcept {
  switch (style) {
  case Style::top:
    return "Top";
  case Style::underbelly:
    return "Underbelly";
  case Style::rolling:
    return "Rolling";
  case Style::broken:
    return "Broken";
  case Style::skippy:
    return "Skippy";
  }
  return "Top";
}

Roles roles_of(std::span<const double> values) noexcept {
  Roles roles{};
  for (std::size_t pad = 0; pad < lane_count; ++pad)
    roles[pad] = knock::pad_role(values, pad);
  return roles;
}

GeneratorSettings clamped(GeneratorSettings settings) noexcept {
  const auto unit = [](double value) { return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0; };
  settings.density = unit(settings.density);
  settings.syncopation = unit(settings.syncopation);
  settings.ghosts = unit(settings.ghosts);
  settings.ratchets = unit(settings.ratchets);
  settings.chance = unit(settings.chance);
  settings.variation = unit(settings.variation);
  if (static_cast<std::size_t>(settings.style) >= style_count)
    settings.style = Style::top;
  const auto choice = std::ranges::find_if(
      bar_choices, [&](std::size_t bars) { return bars >= settings.bars; });
  settings.bars = choice != bar_choices.end() ? *choice : bar_choices.back();
  return settings;
}

Pattern generate(const GeneratorSettings& requested, const Roles& roles) {
  const auto settings = clamped(requested);
  Random random(mixed(settings.seed));
  Pattern pattern;
  pattern.bars.reserve(settings.bars);
  const Bar first = generate_bar(settings, roles, random);
  pattern.bars.push_back(first);
  for (std::size_t bar = 1; bar < settings.bars; ++bar) {
    const bool last = bar + 1 == settings.bars;
    auto busier = settings;
    if (last)
      busier.density = std::min(1.0, settings.density + settings.variation * fill_density);
    const Bar next = generate_bar(busier, roles, random);
    Bar merged = first;
    for (std::size_t lane = 0; lane < lane_count; ++lane) {
      for (std::size_t step = 0; step < steps_per_bar; ++step) {
        const bool fill = last && step >= fill_from_step && settings.variation > fill_threshold;
        if (random.next() < settings.variation * (fill ? fill_share : variation_share))
          merged[lane][step] = next[lane][step];
      }
    }
    pattern.bars.push_back(merged);
  }
  return pattern;
}

Pattern roll(const GeneratorSettings& settings, const Roles& roles, const Pattern& current,
             const Locks& locked) {
  auto fresh = generate(settings, roles);
  if (current.bars.empty())
    return fresh;
  for (std::size_t bar = 0; bar < fresh.bars.size(); ++bar) {
    for (std::size_t lane = 0; lane < lane_count; ++lane) {
      if (locked[lane])
        fresh.bars[bar][lane] = current.bars[bar % current.bars.size()][lane];
    }
  }
  return fresh;
}

Pattern mutate(const Pattern& pattern, double amount, std::uint32_t seed, const Locks& locked) {
  auto out = pattern;
  const bool any_unlocked = std::ranges::any_of(locked, [](bool lock) { return !lock; });
  if (out.bars.empty() || !any_unlocked)
    return out;
  Random random(mixed(seed, mutate_salt));
  bool changed = false;
  for (auto& bar : out.bars) {
    for (std::size_t lane = 0; lane < lane_count; ++lane) {
      if (locked[lane])
        continue;
      for (auto& cell : bar[lane]) {
        if (random.next() >= amount)
          continue;
        const auto next = mutated(cell, random);
        changed = changed || next != cell;
        cell = next;
      }
    }
  }
  if (!changed) {
    // A mutation always shows: one unlocked cell turns over.
    std::vector<std::size_t> open;
    for (std::size_t lane = 0; lane < lane_count; ++lane)
      if (!locked[lane])
        open.push_back(lane);
    auto& cell = out.bars[random.below(out.bars.size())][open[random.below(open.size())]]
                         [random.below(steps_per_bar)];
    cell = cell == Cell::off ? Cell::hit : Cell::off;
  }
  return out;
}

Pattern evolve(const Pattern& pattern, Evolution evolution, const Locks& locked) {
  auto total_bars = evolution.total_bars;
  auto every_bars = evolution.every_bars;
  const double amount = evolution.amount;
  const std::uint32_t seed = evolution.seed;
  if (pattern.bars.empty() || total_bars == 0)
    return pattern;
  total_bars = std::min(total_bars, max_bars);
  every_bars = std::max<std::size_t>(every_bars, 1);
  Pattern current = pattern;
  Pattern out;
  out.bars.reserve(total_bars);
  for (std::size_t bar = 0; bar < total_bars; ++bar) {
    if (bar > 0 && bar % every_bars == 0)
      current = mutate(current, amount, seed + static_cast<std::uint32_t>(bar), locked);
    out.bars.push_back(current.bars[bar % current.bars.size()]);
  }
  return out;
}

// ---- The library -------------------------------------------------------------------

const std::vector<LibraryEntry>& library() {
  static const std::vector<LibraryEntry> entries = build_library();
  return entries;
}

const LibraryEntry* find_loop(std::string_view name) {
  const auto& entries = library();
  const auto found = std::ranges::find(entries, name, &LibraryEntry::name);
  return found != entries.end() ? &*found : nullptr;
}

Pattern loop_pattern(const LibraryEntry& entry, const Roles& roles) {
  return entry.fixed ? *entry.fixed : generate(entry.settings, roles);
}

// ---- Into a clip -------------------------------------------------------------------

foundation::Result<ClipNotes> to_notes(const Pattern& pattern, std::uint32_t seed) {
  using Result = foundation::Result<ClipNotes>;
  if (pattern.bars.empty() || pattern.bars.size() > max_bars)
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "A pattern is 1 to " + std::to_string(max_bars) + " bars"});
  const auto placeholder = project::NoteId::from_bits({0, 1});
  if (!placeholder)
    return Result::failure(placeholder.error());
  ClipNotes clip;
  const auto note = [&](std::int64_t start, std::size_t lane, std::uint8_t velocity) {
    clip.notes.push_back({placeholder.value(),
                          {start},
                          {start + note_ticks.value},
                          static_cast<std::uint8_t>(knock::first_key + lane),
                          velocity});
  };
  for (std::size_t bar = 0; bar < pattern.bars.size(); ++bar) {
    for (std::size_t lane = 0; lane < lane_count; ++lane) {
      for (std::size_t step = 0; step < steps_per_bar; ++step) {
        const auto start = static_cast<std::int64_t>(bar) * bar_ticks.value +
                           static_cast<std::int64_t>(step) * sixteenth_ticks.value;
        switch (pattern.bars[bar][lane][step]) {
        case Cell::off:
          break;
        case Cell::hit:
          note(start, lane, hit_velocity);
          break;
        case Cell::accent:
          note(start, lane, accent_velocity);
          break;
        case Cell::ghost:
          note(start, lane, ghost_velocity);
          break;
        case Cell::chance: {
          // Each cell its own draw, so editing one never reshuffles another.
          const auto cell = static_cast<std::uint32_t>((bar * lane_count + lane) * steps_per_bar + step);
          Random random(mixed(seed ^ chance_salt, cell));
          if (random.next() < chance_probability)
            note(start, lane, hit_velocity);
          break;
        }
        case Cell::ratchet:
          note(start, lane, hit_velocity);
          note(start + note_ticks.value, lane, ratchet_echo_velocity);
          break;
        }
      }
    }
  }
  if (clip.notes.size() > project::max_midi_notes_per_clip)
    return Result::failure({foundation::ErrorCode::resource_exhausted, "Too many notes for a clip"});
  std::ranges::sort(clip.notes, [](const project::MidiNote& a, const project::MidiNote& b) {
    return a.start != b.start ? a.start < b.start : a.key < b.key;
  });
  clip.length = {static_cast<std::int64_t>(pattern.bars.size()) * bar_ticks.value};
  return Result::success(std::move(clip));
}

std::vector<PreviewNote> preview_notes(const ClipNotes& clip, double bpm,
                                       std::uint32_t sample_rate, std::size_t limit) {
  constexpr double seconds_per_minute = 60.0;
  std::vector<PreviewNote> out;
  if (sample_rate == 0 || !std::isfinite(bpm))
    return out;
  const double tempo = std::clamp(bpm, min_preview_bpm, max_preview_bpm);
  const double frames_per_tick = seconds_per_minute / tempo * static_cast<double>(sample_rate) /
                                 static_cast<double>(timeline::ticks_per_quarter);
  const auto frames = [&](std::int64_t ticks) {
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(ticks) * frames_per_tick));
  };
  const auto count = std::min(limit, clip.notes.size());
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& note = clip.notes[index];
    out.push_back({frames(note.start.value),
                   std::max<std::uint64_t>(1, frames(note.end.value - note.start.value)), note.key,
                   note.velocity});
  }
  return out;
}

} // namespace oscillate::application::knock_patterns
// clang-format on
