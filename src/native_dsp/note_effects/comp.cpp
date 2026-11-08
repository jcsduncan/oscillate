// Comp (ADR-0083, ADR-0084): plays the held notes in a rhythm. One step per
// 16th on the song's grid from tick zero, every second 16th of a pair swung
// late by the groove. Each step reads three hit lanes (the lowest held note,
// the ones between and the highest) and a length lane; the top lane may
// cycle shorter than the bar. A chord change may be pushed: the steps just
// before it already play the new chord, seen in the track's notes ahead of
// the block, a hit is added where the push lands and the hit on the change
// itself is dropped. A note joins the first step whose tick passes the
// frame before it, and every choice by chance hashes the seed with the
// step's tick, so any block split, live playback and an applied rhythm agree.
// Motion keeps a loop moving, measured in phrases of 4, 8 or 16 bars from
// the song's start: bars that vary (a dropped hit, a ghost 16th after a
// hit), a turnaround in each phrase's last bar, the top voice drifting to
// another inversion for the second half, and a velocity swell.
#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::comp;
using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

constexpr auto first_low = tables::parameter_index(table::parameters, "comp.low.1");
constexpr auto first_mid = tables::parameter_index(table::parameters, "comp.mid.1");
constexpr auto first_top = tables::parameter_index(table::parameters, "comp.top.1");
constexpr auto first_length = tables::parameter_index(table::parameters, "comp.length.1");
constexpr auto top_length = tables::parameter_index(table::parameters, "comp.top_length");
constexpr auto push_chance = tables::parameter_index(table::parameters, "comp.push_chance");
constexpr auto push_amount = tables::parameter_index(table::parameters, "comp.push_amount");
constexpr auto groove = tables::parameter_index(table::parameters, "comp.groove");
constexpr auto humanise = tables::parameter_index(table::parameters, "comp.humanise");
constexpr auto seed = tables::parameter_index(table::parameters, "comp.seed");
constexpr auto velocity = tables::parameter_index(table::parameters, "comp.velocity");
constexpr auto ghost_velocity = tables::parameter_index(table::parameters, "comp.ghost_velocity");
constexpr auto octave = tables::parameter_index(table::parameters, "comp.octave");
constexpr auto variation = tables::parameter_index(table::parameters, "comp.variation");
constexpr auto phrase = tables::parameter_index(table::parameters, "comp.phrase");
constexpr auto turnaround = tables::parameter_index(table::parameters, "comp.turnaround");
constexpr auto drift = tables::parameter_index(table::parameters, "comp.drift");
constexpr auto swell = tables::parameter_index(table::parameters, "comp.swell");
constexpr auto density = tables::parameter_index(table::parameters, "comp.density");
constexpr auto lift = tables::parameter_index(table::parameters, "comp.lift");
constexpr auto articulation = tables::parameter_index(table::parameters, "comp.articulation");

constexpr std::int64_t sixteenth = tables::division_ticks[tables::sixteenth];
// Articulation never shortens a hit below a 32nd.
constexpr std::int64_t shortest_hit = sixteenth / 2;
constexpr std::int64_t never = std::numeric_limits<std::int64_t>::min();
// Notes Comp holds at once: two hands and a pedal.
constexpr std::size_t max_held = 16;
// Humanise delays a hit by up to this share of a 16th (never earlier).
constexpr double humanise_delay = 0.3;
// And moves its velocity by up to this share either way.
constexpr double humanise_velocity = 0.25;
// A push lands at this share of the step's "On" velocity when its lanes are empty there.
constexpr double push_velocity = 0.9;
// Distinct hash streams per voice, so the three lanes don't move together.
enum Stream : std::uint8_t {
  low_stream = 1,
  mid_stream,
  top_stream,
  push_stream,
  velocity_stream,
  bar_stream,
  turnaround_stream,
  vary_stream,
  density_stream
};
// Density's middle leaves the pattern as written. Below it a hit on a 16th
// between the 8ths drops out, at the bottom always, and below a quarter
// ghosts go too; above it a ghost 16th may follow a hit, at most this often.
constexpr double density_neutral = 0.5;
constexpr double density_ghosts_below = 0.25;
constexpr double density_fill_most = 0.6;
// Motion at full Variation: the share of bars that vary, and in a varied
// bar the chance a hit drops out (never the bar's first) or a ghost 16th
// follows a hit.
constexpr double varied_bar_share = 0.6;
constexpr double drop_share = 0.35;
constexpr double fill_share = 0.25;
// Full Swell dips the velocity by this share at the phrase's turn, rising
// to full in its middle.
constexpr double swell_range = 0.45;
constexpr double full_turn = 2.0 * std::numbers::pi;
// A phrase's last bar takes one of these, chosen per phrase: a ghost 16th
// tucked in before the next bar; beat 3 left silent and the tuck-in; or the
// next phrase's first chord arriving early (pushed for certain).
enum class Turn : std::uint8_t { tuck_in, gap, anticipate, count };
// Beat 3, which the gap leaves silent: its four 16ths.
constexpr int gap_first = 8;
constexpr int gap_last = 11;

[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  const auto quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}
[[nodiscard]] constexpr int modulo(std::int64_t value, int divisor) noexcept {
  return static_cast<int>(((value % divisor) + divisor) % divisor);
}
// The step a note joins: the first whose tick passes the frame before it.
[[nodiscard]] constexpr std::int64_t joining_step(std::int64_t previous_tick) noexcept {
  return floor_div(previous_tick, sixteenth) + 1;
}

struct Held {
  IdBits clip;
  IdBits note;
  std::uint8_t key = 0;
};

// The notes held at a moment, by the note that holds each.
class HeldSet {
public:
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  // False for a note-off it does not hold.
  bool apply(MidiEventKind kind, IdBits clip, IdBits note, std::uint8_t key) noexcept {
    if (kind == MidiEventKind::note_off) {
      for (std::size_t i = 0; i < count_; ++i) {
        if (notes_[i].clip == clip && notes_[i].note == note) {
          notes_[i] = notes_[--count_];
          return true;
        }
      }
      return false;
    }
    if (count_ < notes_.size())
      notes_[count_++] = {clip, note, key};
    return true;
  }
  // The held keys, ascending; returns how many.
  [[nodiscard]] std::size_t keys(std::array<std::uint8_t, max_held>& out) const noexcept {
    // Insertion sort: a handful of keys.
    for (std::size_t i = 0; i < count_; ++i) {
      std::size_t at = i;
      for (; at > 0 && out[at - 1] > notes_[i].key; --at)
        out[at] = out[at - 1];
      out[at] = notes_[i].key;
    }
    return count_;
  }
  [[nodiscard]] bool same_keys(const HeldSet& other) const noexcept {
    if (count_ != other.count_)
      return false;
    std::array<std::uint8_t, max_held> mine{};
    std::array<std::uint8_t, max_held> theirs{};
    (void)keys(mine);
    (void)other.keys(theirs);
    return std::equal(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(count_),
                      theirs.begin());
  }

private:
  std::array<Held, max_held> notes_{};
  std::size_t count_ = 0;
};

// A note event still to come: in the block after the current step, or ahead of it.
struct Upcoming {
  std::int64_t step = 0;
  MidiEventKind kind = MidiEventKind::note_off;
  IdBits clip;
  IdBits note;
  std::uint8_t key = 0;
};

class CompKernel final : public NoteEffectKernel {
public:
  explicit CompKernel(IdBits clip) : clip_(clip) {}

  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    support::Emitter out(output);
    // Held notes are Comp's input and do not sound themselves.
    std::size_t next = 0;
    auto step = std::max(floor_div(clock.previous_tick(), sixteenth) + 1, last_step_ + 1);
    for (; step * sixteenth <= clock.last_tick(); ++step) {
      const auto offset = clock.offset_of(step * sixteenth);
      // Notes up to and including the step's frame join it.
      before_ = held_;
      bool struck = false;
      for (; next < input.size() && input[next].offset <= offset; ++next)
        struck = take(input[next], out) || struck;
      const bool changed = struck && !held_.same_keys(before_);
      play(step, offset, changed, input.subspan(next), clock, parameters);
      last_step_ = step;
    }
    for (; next < input.size(); ++next)
      (void)take(input[next], out);
    pending_.fire(clock, out);
    return out.finish();
  }

  void reset() noexcept override {
    pending_.clear();
    held_.clear();
    last_step_ = never;
    pushed_change_ = never;
  }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    pending_.flush_offs(out);
    held_.clear();
    last_step_ = never;
    pushed_change_ = never;
    return out.finish();
  }

private:
  // True for a note-on (a new note held).
  bool take(const BlockMidiEvent& event, support::Emitter& out) noexcept {
    if (!held_.apply(event.kind, event.clip_id_bits, event.note_id_bits, event.key))
      out.pass(event); // A note-off that is not one of ours.
    return event.kind == MidiEventKind::note_on;
  }

  // The `index`th event still to come after step `step`: first the rest of
  // the block's input, then the notes ahead of the block. Events come in
  // time order, so their steps never fall.
  [[nodiscard]] static std::optional<Upcoming> upcoming(std::size_t index,
                                                        std::span<const BlockMidiEvent> rest,
                                                        const NoteClock& clock,
                                                        const NoteParameters& parameters) noexcept {
    if (index < rest.size()) {
      const auto& event = rest[index];
      const auto previous =
          event.offset == 0 ? clock.previous_tick() : clock.tick_at(event.offset - 1);
      return Upcoming{joining_step(previous), event.kind, event.clip_id_bits, event.note_id_bits,
                      event.key};
    }
    const auto ahead = parameters.ahead();
    index -= rest.size();
    if (index >= ahead.size())
      return std::nullopt;
    const auto& event = ahead[index];
    return Upcoming{joining_step(event.previous_tick), event.kind, event.clip_id_bits,
                    event.note_id_bits, event.key};
  }

  // True when the change at step `change` arrives early. A push that would
  // land before the song's start never happens.
  [[nodiscard]] static bool pushed(std::int64_t change, std::int64_t push_steps,
                                   const NoteParameters& parameters, std::uint32_t offset,
                                   bool forced) noexcept {
    if (change < push_steps)
      return false;
    if (forced)
      return true;
    const auto chance = std::clamp(parameters.at(push_chance, offset), 0.0, 1.0);
    const auto hash =
        support::mix(static_cast<std::uint64_t>(parameters.index_at(seed, offset)),
                     support::mix(push_stream, static_cast<std::uint64_t>(change * sixteenth)));
    return support::unit(hash) < chance;
  }

  // The first change after step `step` and no later than `last`: the step
  // where new notes make the held keys differ, with what is held there
  // (into `chord`). None when the notes coming do not change the chord.
  [[nodiscard]] std::optional<std::int64_t> next_change(std::int64_t step, std::int64_t last,
                                                        std::span<const BlockMidiEvent> rest,
                                                        const NoteClock& clock,
                                                        const NoteParameters& parameters,
                                                        HeldSet& chord) const noexcept {
    chord = held_;
    std::size_t index = 0;
    auto event = upcoming(index, rest, clock, parameters);
    while (event && event->step <= last) {
      const auto group = event->step;
      HeldSet before = chord;
      bool struck = false;
      for (; event && event->step == group; event = upcoming(++index, rest, clock, parameters)) {
        (void)chord.apply(event->kind, event->clip, event->note, event->key);
        struck = struck || event->kind == MidiEventKind::note_on;
      }
      if (group > step && struck && !chord.same_keys(before))
        return group;
    }
    return std::nullopt;
  }

  // Plays step `step` (at frame `offset` of the block). `changed`: new
  // notes joined this step and changed the chord.
  void play(std::int64_t step, std::uint32_t offset, bool changed,
            std::span<const BlockMidiEvent> rest, const NoteClock& clock,
            const NoteParameters& parameters) noexcept {
    const auto tick = step * sixteenth;
    const auto bar_step = modulo(step, table::steps);
    const auto top_steps = std::clamp(parameters.index_at(top_length, offset), 1, table::steps);
    const auto top_step = modulo(step, top_steps);
    const auto amount = static_cast<std::size_t>(std::clamp(
        parameters.index_at(push_amount, offset), 0, static_cast<int>(table::pushes.size()) - 1));
    const std::int64_t push_steps = table::push_sixteenths[amount];
    // Motion: where the step sits in its bar and phrase.
    const auto seed_value = static_cast<std::uint64_t>(parameters.index_at(seed, offset));
    const auto phrase_length = table::phrase_bars[static_cast<std::size_t>(std::clamp(
        parameters.index_at(phrase, offset), 0, static_cast<int>(table::phrases.size()) - 1))];
    const auto bar = floor_div(step, table::steps);
    const auto bar_in_phrase = modulo(bar, phrase_length);
    const auto phrase_index = floor_div(bar, phrase_length);
    const bool turn_bar =
        parameters.index_at(turnaround, offset) != 0 && bar_in_phrase == phrase_length - 1;
    const auto turn = static_cast<Turn>(
        support::mix(seed_value,
                     support::mix(turnaround_stream, static_cast<std::uint64_t>(phrase_index))) %
        static_cast<std::uint64_t>(Turn::count));
    const double vary = std::clamp(parameters.at(variation, offset), 0.0, 1.0);
    const bool bar_varies =
        !turn_bar && vary > 0.0 &&
        support::unit(
            support::mix(seed_value, support::mix(bar_stream, static_cast<std::uint64_t>(bar)))) <
            vary * varied_bar_share;
    const auto next_phrase = (phrase_index + 1) * phrase_length * table::steps;
    // A pushed change within reach plays now; one exactly a push away lands here.
    bool push_lands = false;
    const HeldSet* sounding = &held_;
    if (const auto change = next_change(step, step + push_steps, rest, clock, parameters, chord_);
        change && pushed(*change, push_steps, parameters, offset,
                         turn_bar && turn == Turn::anticipate && *change == next_phrase)) {
      sounding = &chord_;
      push_lands = *change == step + push_steps;
      pushed_change_ = *change;
    } else if (changed && pushed_change_ == step) {
      return; // The change already sounded, pushed.
    }
    std::array<std::uint8_t, max_held> keys{};
    const auto count = sounding->keys(keys);
    if (count == 0)
      return;
    const int shift = 12 * parameters.index_at(octave, offset);
    // Top drift: for the phrase's second half the lowest upper voice goes up
    // an octave, so another chord tone is on top.
    if (parameters.index_at(drift, offset) != 0 && bar_in_phrase >= phrase_length / 2 &&
        count > 2 && keys[1] + 12 <= 127) {
      keys[1] = static_cast<std::uint8_t>(keys[1] + 12);
      std::sort(keys.begin() + 1, keys.begin() + static_cast<std::ptrdiff_t>(count));
    }
    // Swell: velocity dips at the phrase's turn and peaks in its middle.
    const double phrase_position = (static_cast<double>(bar_in_phrase * table::steps + bar_step)) /
                                   static_cast<double>(phrase_length * table::steps);
    const double swelled = 1.0 - std::clamp(parameters.at(swell, offset), 0.0, 1.0) * swell_range *
                                     0.5 * (1.0 + std::cos(full_turn * phrase_position));
    const double human = std::clamp(parameters.at(humanise, offset), 0.0, 1.0);
    const auto groove_index = static_cast<std::size_t>(std::clamp(
        parameters.index_at(groove, offset), 0, static_cast<int>(table::grooves.size()) - 1));
    const auto late =
        (step % 2 != 0)
            ? static_cast<std::int64_t>(std::llround(
                  (2.0 * table::groove_swing[groove_index] - 1.0) * static_cast<double>(sixteenth)))
            : 0;
    const auto push_ticks = push_steps * sixteenth;
    // Lift doubles the top voice an octave up for the phrase's second half.
    const bool lifted =
        parameters.index_at(lift, offset) != 0 && bar_in_phrase >= phrase_length / 2;
    const auto hit = [&](std::size_t lane_first, int lane_step, Stream stream,
                         std::span<const std::uint8_t> lane_keys) {
      const auto lane_value = [&](int at) {
        return parameters.index_at(lane_first + static_cast<std::size_t>(at), offset);
      };
      int value = lane_value(lane_step);
      // Motion varies the chord's lanes; the bass keeps its place.
      if (lane_first != first_low) {
        const int lane_steps = lane_first == first_top ? top_steps : table::steps;
        int first_hit = 0;
        while (first_hit < lane_steps && lane_value(first_hit) == table::off)
          ++first_hit;
        // Density, by the step in the pattern so every bar agrees.
        const double dense = std::clamp(parameters.at(density, offset), 0.0, 1.0);
        const double pattern_roll = support::unit(
            support::mix(seed_value, support::mix(density_stream + stream,
                                                  static_cast<std::uint64_t>(lane_step))));
        if (lane_step != first_hit && value != table::off) {
          const bool between_eighths = lane_step % 2 != 0;
          if ((between_eighths && pattern_roll < (density_neutral - dense) / density_neutral) ||
              (value == table::ghost &&
               pattern_roll < (density_ghosts_below - dense) / density_ghosts_below))
            value = table::off;
        } else if (value == table::off && dense > density_neutral &&
                   lane_value(modulo(lane_step - 1, lane_steps)) != table::off &&
                   pattern_roll < (dense - density_neutral) / density_neutral * density_fill_most) {
          value = table::ghost;
        }
        if (turn_bar && turn == Turn::gap && bar_step >= gap_first && bar_step <= gap_last) {
          value = table::off;
        } else if (turn_bar && turn != Turn::anticipate && bar_step == table::steps - 1 &&
                   value == table::off) {
          value = table::ghost;
        } else if (bar_varies) {
          const double roll = support::unit(support::mix(
              seed_value, support::mix(vary_stream + stream, static_cast<std::uint64_t>(tick))));
          if (value != table::off && lane_step != first_hit && roll < vary * drop_share)
            value = table::off;
          else if (value == table::off &&
                   lane_value(modulo(lane_step - 1, lane_steps)) != table::off &&
                   roll < vary * fill_share)
            value = table::ghost;
        }
      }
      if (push_lands && value == table::off)
        value = table::on;
      if (value == table::off || lane_keys.empty())
        return;
      const auto length_step = lane_first == first_top ? lane_step : bar_step;
      auto length = std::clamp(parameters.index_at(
                                   first_length + static_cast<std::size_t>(length_step), offset),
                               1, table::max_length) *
                    sixteenth;
      if (push_lands)
        length += push_ticks;
      length =
          std::max(shortest_hit, static_cast<std::int64_t>(std::llround(
                                     static_cast<double>(length) *
                                     std::clamp(parameters.at(articulation, offset), 0.0, 1.0))));
      const auto stream_hash =
          support::mix(seed_value, support::mix(stream, static_cast<std::uint64_t>(tick)));
      const auto delay = static_cast<std::int64_t>(std::llround(
          support::unit(stream_hash) * human * humanise_delay * static_cast<double>(sixteenth)));
      const double base = value == table::on
                              ? parameters.at(velocity, offset) * (push_lands ? push_velocity : 1.0)
                              : parameters.at(ghost_velocity, offset);
      const double spread = (support::unit(support::mix(stream_hash, velocity_stream)) - 0.5) *
                            2.0 * human * humanise_velocity;
      const auto level = static_cast<std::uint8_t>(
          std::clamp(static_cast<int>(std::lround(base * swelled * (1.0 + spread))), 1, 127));
      const auto on_tick = tick + late + delay;
      std::array<std::uint8_t, max_held + 1> sounded{};
      std::size_t sounded_count = 0;
      for (const auto key : lane_keys)
        sounded[sounded_count++] = key;
      if (lifted && lane_first == first_top && !lane_keys.empty() && lane_keys.back() + 12 <= 127)
        sounded[sounded_count++] = static_cast<std::uint8_t>(lane_keys.back() + 12);
      for (const auto key : std::span(sounded).first(sounded_count)) {
        const int played = static_cast<int>(key) + shift;
        // A note plays only when its note-off can be scheduled too.
        if (played < 0 || played > 127 || pending_.size() + 2 > pending_capacity)
          continue;
        const auto id = ids_.next();
        (void)pending_.push(
            {on_tick, MidiEventKind::note_on, clip_, id, static_cast<std::uint8_t>(played), level});
        (void)pending_.push({on_tick + length, MidiEventKind::note_off, clip_, id,
                             static_cast<std::uint8_t>(played), 0});
      }
    };
    // Low is the lowest held note (the bass), top the highest, middle the rest.
    const auto notes = std::span(keys).first(count);
    hit(first_low, bar_step, low_stream, notes.first(1));
    if (notes.size() > 2)
      hit(first_mid, bar_step, mid_stream, notes.subspan(1, notes.size() - 2));
    if (notes.size() > 1)
      hit(first_top, top_step, top_stream, notes.last(1));
  }

  static constexpr std::size_t pending_capacity = 1'024;
  IdBits clip_;
  support::NoteIds ids_;
  support::Pending<pending_capacity> pending_;
  HeldSet held_;
  HeldSet before_; // What was held before the step being played.
  HeldSet chord_;  // What a pushed change will hold.
  std::int64_t last_step_ = never;
  // The change a push played early: its own hit is dropped. A change Comp
  // could not see coming (notes played live) is never dropped.
  std::int64_t pushed_change_ = never;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;

// A pattern as the values that differ from the defaults.
// Lanes are written as text: `x` on, `o` ghost, `.` off.
constexpr std::size_t max_pattern_values = 4 * table::steps + 4;
struct Pattern {
  std::array<V, max_pattern_values> values{};
  std::size_t count = 0;
  constexpr void set(std::size_t parameter, double value) {
    if (table::parameters[parameter].default_value != value)
      values[count++] = {table::parameters[parameter].id, value};
  }
  constexpr void lane(std::size_t first, std::string_view hits) {
    for (std::size_t step = 0; step < hits.size(); ++step)
      set(first + step, hits[step] == 'x'   ? table::on
                        : hits[step] == 'o' ? table::ghost
                                            : table::off);
  }
  [[nodiscard]] constexpr std::span<const V> span() const { return std::span(values).first(count); }
};
struct Shape {
  std::string_view low, mid, top;
  int length = 3;
  double push_chance = 0.5;
  int push = table::push_eighth;
  int groove = table::groove_58;
  int top_steps = table::steps;
  // Per-step lengths in 16ths where they differ (`.` keeps `length`): 1-9,
  // then a-g for 10-16.
  std::string_view lengths = {};
};
constexpr int length_digit(char c) {
  constexpr int first_letter = 10;
  return c >= 'a' ? first_letter + (c - 'a') : c - '0';
}
constexpr Pattern pattern(const Shape& shape) {
  Pattern made;
  made.lane(first_low, shape.low);
  made.lane(first_mid, shape.mid);
  made.lane(first_top, shape.top);
  for (std::size_t step = 0; step < table::steps; ++step) {
    const bool own = step < shape.lengths.size() && shape.lengths[step] != '.';
    made.set(first_length + step, own ? length_digit(shape.lengths[step]) : shape.length);
  }
  made.set(push_chance, shape.push_chance);
  made.set(push_amount, shape.push);
  made.set(groove, shape.groove);
  made.set(top_length, shape.top_steps);
  return made;
}

// Rhythms from melodic house, progressive, melodic techno and trance
// (ADR-0084 addendum). They play the chord's upper voices; the bass is the
// bass line's job (a chord clip keeps its bass held under them).
// The Anjunadeep pluck: offbeat 8ths with a 16th tuck-in before the bar.
constexpr auto anjuna_offbeats =
    pattern({"................", "..x...x...x...xo", "..x...x...x...xo", 2, 0.0, table::push_eighth,
             table::groove_56, table::steps, "..............11"});
// Progressive trance plucks: the gaps make the pattern; beats accented.
constexpr auto driving_pluck = pattern({"................", "x.o.x..ox.o.x..o", "x.o.x..ox.o.x..o",
                                        1, 0.0, table::push_eighth, table::straight});
constexpr auto rolling_pluck = pattern({"................", "x..ox.o.x..ox.o.", "x..ox.o.x..ox.o.",
                                        1, 0.0, table::push_eighth, table::groove_56});
// 3-3-2: 1, the "and" of 2 and 4, held to the next hit (piano chords).
constexpr auto three_three_two =
    pattern({"................", "x..x..x.x..x..x.", "x..x..x.x..x..x.", 3, 0.0, table::push_eighth,
             table::groove_56, table::steps, "......2.......2."});
// Every dotted 8th against the four-on-the-floor kick.
constexpr auto dotted_pulse = pattern({"................", "x..x..x..x..x..x", "x..x..x..x..x..x",
                                       2, 0.0, table::push_eighth, table::straight});
// Melodic techno: off the beat on the "and" of 2 and 4, sometimes early.
constexpr auto techno_stabs = pattern({"................", "......x.......x.", "......x.......x.",
                                       2, 0.4, table::push_sixteenth, table::straight});
// Sphinx (Nora En Pure): the chord held most of the bar, struck again just
// before the next on the last two 16th positions of the beat.
constexpr auto anticipated =
    pattern({"................", "x..........x..x.", "x..........x..x.", 2, 0.0, table::push_eighth,
             table::straight, table::steps, "b..........3..2."});
// Repeated 8ths, the deadmau5 piano house pulse (Avicii, David August).
constexpr auto piano_eighths = pattern({"................", "x.x.x.x.x.x.x.x.", "x.x.x.x.x.x.x.x.",
                                        2, 0.0, table::push_eighth, table::straight});
// A 16th trance gate over the held chord, short-short-long, beats accented.
constexpr auto trance_gate = pattern({"................", "xo.ox.o.xo.ox.o.", "xo.ox.o.xo.ox.o.", 1,
                                      0.0, table::push_eighth, table::straight});
constexpr auto pads_swells = pattern({"x...............", "x...............", "x...............",
                                      16, 0.0, table::push_eighth, table::straight});

constexpr std::array<Preset, 10> presets{{
    {"Anjuna offbeats", "Patterns", anjuna_offbeats.span()},
    {"Driving pluck", "Patterns", driving_pluck.span()},
    {"Rolling pluck", "Patterns", rolling_pluck.span()},
    {"Three-three-two", "Patterns", three_three_two.span()},
    {"Dotted pulse", "Patterns", dotted_pulse.span()},
    {"Techno stabs", "Patterns", techno_stabs.span()},
    {"Trance gate", "Patterns", trance_gate.span()},
    {"Pads & swells", "Patterns", pads_swells.span()},
    {"Anticipated", "Patterns", anticipated.span()},
    {"Piano eighths", "Patterns", piano_eighths.span()},
}};

constexpr EngineDescriptor descriptor{"comp", "Chord Comp", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(IdBits generated_clip) {
  return std::make_unique<CompKernel>(generated_clip);
}

double tail(std::span<const double> values) {
  if (values.size() != table::parameters.size())
    return 0.0;
  // The longest hit, pushed, plus the latest a groove and humanise move it.
  double longest = 1.0;
  for (int step = 0; step < table::steps; ++step)
    longest = std::max(longest, values[first_length + static_cast<std::size_t>(step)]);
  const auto amount =
      static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(values[push_amount])), 0,
                                          static_cast<int>(table::pushes.size()) - 1));
  constexpr double latest_shift = 1.0; // A 16th covers the groove plus humanise.
  return (longest + table::push_sixteenths[amount] + latest_shift) / 4.0;
}

} // namespace

extern const NoteEffectEngine comp_engine{&descriptor, create, tail};

} // namespace oscillate::native_dsp::note_effects
