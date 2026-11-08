// Arp (ADR-0059): the held notes play one at a time (or together, in
// Chord mode) on a synced grid. Steps sit on the song's grid of the rate,
// counted from tick zero, with every second step of a pair swung late; a
// first note after silence plays at once and the grid carries on from it.
// Held notes are the arp's input: they do not sound themselves.
#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::arp;
using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

constexpr auto mode = tables::parameter_index(table::parameters, "arp.mode");
constexpr auto rate = tables::parameter_index(table::parameters, "arp.rate");
constexpr auto gate = tables::parameter_index(table::parameters, "arp.gate");
constexpr auto octaves = tables::parameter_index(table::parameters, "arp.octaves");
constexpr auto swing = tables::parameter_index(table::parameters, "arp.swing");
constexpr auto steps = tables::parameter_index(table::parameters, "arp.steps");
constexpr auto accent = tables::parameter_index(table::parameters, "arp.accent");
constexpr auto velocity_mode = tables::parameter_index(table::parameters, "arp.velocity");
constexpr auto level = tables::parameter_index(table::parameters, "arp.level");
constexpr auto latch = tables::parameter_index(table::parameters, "arp.latch");
constexpr auto seed = tables::parameter_index(table::parameters, "arp.seed");

constexpr std::size_t max_held = 32;
constexpr std::size_t max_octaves = 4;
constexpr int accent_boost = 24;
constexpr std::int64_t never = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  const auto quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// The grid: step n of length `length`, every odd step late by the swing.
struct Grid {
  std::int64_t length = tables::division_ticks[tables::sixteenth];
  std::int64_t late = 0; // Odd steps start this much after the straight grid.

  [[nodiscard]] std::int64_t start(std::int64_t step) const noexcept {
    const auto pair = floor_div(step, 2);
    return pair * 2 * length + ((step - pair * 2) != 0 ? length + late : 0);
  }
  // The first step starting after `tick`.
  [[nodiscard]] std::int64_t after(std::int64_t tick) const noexcept {
    auto step = floor_div(tick, length) - 1;
    while (start(step) <= tick)
      ++step;
    return step;
  }
};

struct Held {
  IdBits clip;
  IdBits note;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  std::uint64_t order = 0;
};

class HeldSet {
public:
  bool add(const Held& note) noexcept {
    if (count_ == max_held)
      return false;
    notes_[count_++] = note;
    return true;
  }
  bool remove(IdBits clip, IdBits note) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (notes_[i].clip == clip && notes_[i].note == note) {
        // Keep arrival order: shift down.
        std::move(notes_.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                  notes_.begin() + static_cast<std::ptrdiff_t>(count_),
                  notes_.begin() + static_cast<std::ptrdiff_t>(i));
        --count_;
        return true;
      }
    }
    return false;
  }
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::span<const Held> notes() const noexcept {
    return std::span(notes_).first(count_);
  }

private:
  std::array<Held, max_held> notes_{};
  std::size_t count_ = 0;
};

class ArpKernel final : public NoteEffectKernel {
public:
  explicit ArpKernel(IdBits clip) : clip_(clip) {}

  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    support::Emitter out(output);
    const Grid grid = grid_at(parameters, 0);
    std::size_t next = 0;
    auto cursor = clock.previous_tick();
    while (true) {
      // The next grid step in this block not yet played.
      auto step = grid.after(std::max(cursor, last_step_tick_));
      auto step_tick = grid.start(step);
      const bool stepping = clock.holds(step_tick);
      const auto step_offset = stepping ? clock.offset_of(step_tick) : clock.frames();
      // Notes up to and including the step's frame join it.
      while (next < input.size() && (input[next].offset <= step_offset || !stepping)) {
        const auto offset = input[next].offset;
        const bool was_silent = active(parameters, offset).empty();
        while (next < input.size() && input[next].offset == offset)
          take(input[next++], parameters, out);
        if (was_silent && !active(parameters, offset).empty()) {
          // A first note after silence plays at once.
          const auto now = clock.tick_at(offset);
          play(offset, now, grid.after(now) - 1, parameters, out);
          last_step_tick_ = now;
        }
        if (stepping && offset == step_offset)
          break;
      }
      if (!stepping)
        break;
      if (step_tick > last_step_tick_) {
        play(step_offset, step_tick, step, parameters, out);
        last_step_tick_ = step_tick;
      }
      cursor = step_tick;
    }
    pending_.fire(clock, out);
    return out.finish();
  }

  void reset() noexcept override {
    physical_.clear();
    latched_.clear();
    pending_.clear();
    last_step_tick_ = never;
    position_ = 0;
    order_ = 0;
    ids_ = {};
  }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    pending_.flush_offs(out);
    physical_.clear();
    latched_.clear();
    last_step_tick_ = never;
    position_ = 0;
    return out.finish();
  }

private:
  [[nodiscard]] static Grid grid_at(const NoteParameters& parameters, std::uint32_t offset) {
    const auto index = static_cast<std::size_t>(std::clamp(
        parameters.index_at(rate, offset), 0, static_cast<int>(tables::divisions.size()) - 1));
    Grid grid;
    grid.length = tables::division_ticks[index];
    const double amount = std::clamp(parameters.at(swing, offset), 0.5, 0.75);
    grid.late = static_cast<std::int64_t>(
        std::llround((2.0 * amount - 1.0) * static_cast<double>(grid.length)));
    return grid;
  }

  [[nodiscard]] const HeldSet& active(const NoteParameters& parameters,
                                      std::uint32_t offset) const noexcept {
    return parameters.index_at(latch, offset) != 0 ? latched_ : physical_;
  }

  // One input event: held notes change; offs the arp does not hold pass.
  void take(const BlockMidiEvent& event, const NoteParameters& parameters,
            support::Emitter& out) noexcept {
    const bool latching = parameters.index_at(latch, event.offset) != 0;
    if (event.kind == MidiEventKind::note_off) {
      const bool held = physical_.remove(event.clip_id_bits, event.note_id_bits);
      // Unlatched, the latched set mirrors the keys down.
      const bool latched = !latching && latched_.remove(event.clip_id_bits, event.note_id_bits);
      if (!held && !latched)
        out.pass(event);
      if (active(parameters, event.offset).empty())
        position_ = 0;
      return;
    }
    // A new chord after every key was let go replaces a latched one.
    if (physical_.empty())
      latched_.clear();
    const Held note{event.clip_id_bits, event.note_id_bits, event.key, event.velocity, ++order_};
    if (physical_.add(note))
      (void)latched_.add(note);
  }

  // Plays step `step` at `offset` (tick `tick`) from the active notes.
  void play(std::uint32_t offset, std::int64_t tick, std::int64_t step,
            const NoteParameters& parameters, support::Emitter& out) noexcept {
    const auto& held = active(parameters, offset);
    if (held.empty()) {
      position_ = 0;
      return;
    }
    const int pattern_mode = parameters.index_at(mode, offset);
    const int octave_count =
        std::clamp(parameters.index_at(octaves, offset), 1, static_cast<int>(max_octaves));
    const int length = parameters.index_at(steps, offset);
    if (length > 0 && position_ >= static_cast<std::uint64_t>(length))
      position_ = 0;
    // The pattern's notes, low to high or as played.
    std::array<Held, max_held> sorted{};
    const auto notes = held.notes();
    std::ranges::copy(notes, sorted.begin());
    const auto base = std::span(sorted).first(notes.size());
    if (pattern_mode != table::as_played)
      std::ranges::sort(base, [](const Held& a, const Held& b) { return a.key < b.key; });
    const auto count = static_cast<std::int64_t>(base.size());
    const auto run = count * octave_count; // One pass up, over every octave.
    const auto pick = [&](std::int64_t index) {
      const auto& note = base[static_cast<std::size_t>(index % count)];
      return std::pair{static_cast<int>(note.key) + 12 * static_cast<int>(index / count), note};
    };
    const auto& grid = grid_at(parameters, offset);
    const auto off_tick =
        tick +
        std::max<std::int64_t>(1, std::llround(std::clamp(parameters.at(gate, offset), 0.05, 2.0) *
                                               static_cast<double>(grid.length)));
    const int accent_every = parameters.index_at(accent, offset);
    const bool accented =
        accent_every > 0 && position_ % static_cast<std::uint64_t>(accent_every) == 0;
    const auto sound = [&](int key, const Held& note) {
      int velocity = parameters.index_at(velocity_mode, offset) == 0
                         ? note.velocity
                         : parameters.index_at(level, offset);
      if (accented)
        velocity += accent_boost;
      // A note plays only when its note-off can be scheduled.
      if (key > 127 || pending_.full())
        return;
      const auto id = ids_.next();
      if (out.note_on(offset, clip_, id, key, velocity))
        (void)pending_.push(
            {off_tick, MidiEventKind::note_off, clip_, id, static_cast<std::uint8_t>(key), 0});
    };
    const auto position = static_cast<std::int64_t>(position_);
    switch (pattern_mode) {
    case table::chord: {
      const auto octave = static_cast<int>(position % octave_count);
      for (const auto& note : base)
        sound(note.key + 12 * octave, note);
      break;
    }
    case table::random: {
      const auto hash = support::mix(static_cast<std::uint64_t>(parameters.index_at(seed, offset)),
                                     static_cast<std::uint64_t>(step));
      const auto [key, note] =
          pick(static_cast<std::int64_t>(hash % static_cast<std::uint64_t>(run)));
      sound(key, note);
      break;
    }
    case table::down: {
      const auto [key, note] = pick(run - 1 - position % run);
      sound(key, note);
      break;
    }
    case table::up_down:
    case table::down_up: {
      // Up then down without repeating the turning notes.
      const auto cycle = run > 1 ? 2 * run - 2 : 1;
      auto index = position % cycle;
      if (pattern_mode == table::down_up)
        index = (index + run - 1) % cycle;
      const auto [key, note] = pick(index < run ? index : cycle - index);
      sound(key, note);
      break;
    }
    default: { // Up, As Played.
      const auto [key, note] = pick(position % run);
      sound(key, note);
      break;
    }
    }
    ++position_;
  }

  IdBits clip_;
  support::NoteIds ids_;
  HeldSet physical_;
  HeldSet latched_;
  support::Pending<512> pending_;
  std::int64_t last_step_tick_ = never;
  std::uint64_t position_ = 0;
  std::uint64_t order_ = 0;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 2> down_eighths{{{"arp.mode", table::down}, {"arp.rate", tables::eighth}}};
constexpr std::array<V, 2> up_down{{{"arp.mode", table::up_down}, {"arp.octaves", 2}}};
constexpr std::array<V, 3> trance{{{"arp.gate", 0.25}, {"arp.octaves", 3}, {"arp.accent", 4}}};
constexpr std::array<V, 3> random_32{
    {{"arp.mode", table::random}, {"arp.rate", 1}, {"arp.gate", 0.6}}};
constexpr std::array<V, 3> stabs{
    {{"arp.mode", table::chord}, {"arp.rate", tables::eighth}, {"arp.gate", 0.3}}};
constexpr std::array<V, 2> swing_pluck{{{"arp.swing", 0.62}, {"arp.gate", 0.4}}};
constexpr std::array<V, 2> latch_up{{{"arp.latch", 1}, {"arp.octaves", 3}}};
constexpr std::array<V, 3> played{
    {{"arp.mode", table::as_played}, {"arp.rate", 4}, {"arp.gate", 0.8}}};
constexpr std::array<V, 3> four_step{{{"arp.steps", 4}, {"arp.octaves", 2}, {"arp.accent", 4}}};
constexpr std::array<V, 3> legato{{{"arp.gate", 1.0}, {"arp.velocity", 1}, {"arp.level", 90}}};

constexpr std::array<Preset, 11> presets{{
    {"Up 16ths", "Classic", {}},
    {"Down 8ths", "Classic", down_eighths},
    {"Up-Down 2 Octaves", "Classic", up_down},
    {"As Played Triplets", "Classic", played},
    {"Legato Even", "Classic", legato},
    {"Trance Gate", "Dance", trance},
    {"Chord Stabs", "Dance", stabs},
    {"Swing Pluck", "Dance", swing_pluck},
    {"Four Step Accent", "Dance", four_step},
    {"Random 32nds", "Texture", random_32},
    {"Latch Up 3 Octaves", "Texture", latch_up},
}};

constexpr EngineDescriptor descriptor{"arp", "Arpeggiator", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(IdBits generated_clip) {
  return std::make_unique<ArpKernel>(generated_clip);
}

double tail(std::span<const double> values) {
  if (values.size() != table::parameters.size())
    return 0.0;
  // The last step's gate, at its longest.
  const auto index =
      static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(values[rate])), 0,
                                          static_cast<int>(tables::divisions.size()) - 1));
  return static_cast<double>(tables::division_ticks[index]) / static_cast<double>(tables::quarter) *
         std::clamp(values[gate], 0.05, 2.0);
}

} // namespace

extern const NoteEffectEngine arp_engine{&descriptor, create, tail};

} // namespace oscillate::native_dsp::note_effects
