#include "oscillate/native_dsp/synth/nova/arp.hpp"

#include "arp_state.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace oscillate::native_dsp::synth::nova {

namespace {

using namespace arp_detail;

constexpr double free_beats_per_second = 2.0; // 120 BPM with no tempo.
constexpr double free_beats_per_bar = 4.0;    // 4/4 with no tempo.
constexpr double frame_epsilon = 1e-4;        // Absorbs beat rounding between blocks.
constexpr double far_frames = 1e15;
constexpr double sixteenth = 0.25;
constexpr double shortest_clip_beats = 0.25;
constexpr double longest_clip_beats = 16.0;
constexpr std::uint64_t random_seed = 0x9e3779b97f4a7c15ULL;

[[nodiscard]] std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  std::int64_t quotient = value / divisor;
  if (value % divisor != 0 && (value < 0) != (divisor < 0)) {
    --quotient;
  }
  return quotient;
}

[[nodiscard]] std::int64_t positive_mod(std::int64_t value, std::int64_t divisor) noexcept {
  const std::int64_t rest = value % divisor;
  return rest < 0 ? rest + divisor : rest;
}

// SplitMix64: a stateless hash, so a step's random note depends only on its
// index and survives seeks and block splits.
using native_dsp::dsp::splitmix64;

[[nodiscard]] bool same_note(const NoteEvent& a, const NoteEvent& b) noexcept {
  return a.clip == b.clip && a.note == b.note && a.key == b.key;
}

[[nodiscard]] bool before(const GeneratedNote& a, const GeneratedNote& b) noexcept {
  if (a.offset != b.offset) {
    return a.offset < b.offset;
  }
  if (a.on != b.on) {
    return !a.on;
  }
  if (a.note.key != b.note.key) {
    return a.note.key < b.note.key;
  }
  if (a.note.clip != b.note.clip) {
    return a.note.clip < b.note.clip;
  }
  return a.note.note < b.note.note;
}

[[nodiscard]] GeneratedNote off_at(std::int64_t frame, const NoteEvent& note) noexcept {
  NoteEvent off = note;
  off.velocity = 0;
  return {static_cast<std::uint32_t>(frame), false, off};
}

[[nodiscard]] std::uint8_t to_velocity(double level) noexcept {
  const double scaled = std::round(std::clamp(level, 0.0, 1.0) * 127.0);
  return static_cast<std::uint8_t>(std::clamp(scaled, 1.0, 127.0));
}

// Salts that keep each seeded choice independent of the others.
constexpr std::uint64_t chance_salt = 0x6368616e6365ULL;
constexpr std::uint64_t walk_salt = 0x77616c6bULL;
constexpr std::uint64_t shuffle_salt = 0x73687566666c65ULL;
constexpr std::uint64_t variation_salt = 0x76617279ULL;
// A variation bar reads the Note and Octave lanes 1..this many steps on.
constexpr std::uint64_t variation_shifts = 3;
// A random walk moves one place, or two once in this many steps.
constexpr std::uint64_t walk_leap_odds = 4;
constexpr double grid_epsilon = 1e-9;
// Telemetry carries the step count as a float: exact below 2^24.
constexpr std::int64_t largest_reported_step = (std::int64_t{1} << 24) - 1;

[[nodiscard]] std::uint64_t seeded(std::uint32_t seed, std::uint64_t salt) noexcept {
  return splitmix64(splitmix64(static_cast<std::uint64_t>(seed) ^ random_seed) ^ salt);
}

[[nodiscard]] std::uint64_t hash_step(std::uint32_t seed, std::uint64_t salt,
                                      std::int64_t k) noexcept {
  return splitmix64(seeded(seed, salt) ^ static_cast<std::uint64_t>(k));
}

// 0 <= u < 1 from the top 53 bits.
[[nodiscard]] double unit(std::uint64_t value) noexcept {
  constexpr double scale = 1.0 / static_cast<double>(std::uint64_t{1} << 53U);
  return static_cast<double>(value >> 11U) * scale;
}

[[nodiscard]] double rate_of(const ArpSettings& settings) noexcept {
  return arp_rate_beats[settings.rate];
}

[[nodiscard]] std::int64_t groove_index(std::int64_t n, const ArpSettings& settings) noexcept {
  const auto& groove = settings.groove;
  const double per_quarter = arp_groove_grids[groove.grid];
  const double beat = static_cast<double>(n) * rate_of(settings);
  const auto index = static_cast<std::int64_t>(std::floor(beat * per_quarter + grid_epsilon));
  return positive_mod(index, static_cast<std::int64_t>(groove.steps));
}

// Swing and groove move step n off the grid: odd steps late by swing × a
// step, and the groove step under it by its timing × amount. Together at
// most arp_early_limit early or arp_late_limit late, so steps keep order.
[[nodiscard]] double step_start(std::int64_t n, const ArpSettings& settings) noexcept {
  const double rate = rate_of(settings);
  double late = positive_mod(n, 2) == 1 ? settings.swing : 0.0; // In steps.
  const auto& groove = settings.groove;
  if (groove.steps > 0 && groove.amount > 0.0) {
    const double per_quarter = arp_groove_grids[groove.grid];
    const double timing = groove.timing[static_cast<std::size_t>(groove_index(n, settings))];
    late += timing * groove.amount / per_quarter / rate;
  }
  return static_cast<double>(n) * rate + std::clamp(late, -arp_early_limit, arp_late_limit) * rate;
}

// The groove's velocity factor for step n, scaled by the amount.
[[nodiscard]] double groove_velocity(std::int64_t n, const ArpSettings& settings) noexcept {
  const auto& groove = settings.groove;
  if (groove.steps == 0) {
    return 1.0;
  }
  const double factor = groove.velocity[static_cast<std::size_t>(groove_index(n, settings))];
  return 1.0 + (factor - 1.0) * groove.amount;
}

// The first step whose start lands in this block or later.
[[nodiscard]] std::int64_t first_step(const Axis& axis, const ArpSettings& settings) noexcept {
  auto n = static_cast<std::int64_t>(std::floor(axis.beat / rate_of(settings))) - 2;
  while (axis.raw_frame(step_start(n, settings)) < 0) {
    ++n;
  }
  return n;
}

[[nodiscard]] double quantise(double beat) noexcept {
  return std::ceil(beat / sixteenth - 1e-9) * sixteenth;
}

[[nodiscard]] double clamp_finite(double value, double low, double high, double fallback) noexcept {
  return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

[[nodiscard]] ArpSettings sanitise(const ArpSettings& settings) noexcept {
  ArpSettings next = settings;
  if (static_cast<std::size_t>(next.mode) >= arp_mode_count) {
    next.mode = ArpMode::up;
  }
  if (next.velocity > ArpVelocity::pattern) {
    next.velocity = ArpVelocity::played;
  }
  if (next.note_mode > ArpNoteMode::scale) {
    next.note_mode = ArpNoteMode::held;
  }
  next.rate = std::min(next.rate, arp_rate_beats.size() - 1);
  next.gate = clamp_finite(next.gate, 0.0, 1.0, 0.6);
  next.swing = clamp_finite(next.swing, 0.0, arp_max_swing, 0.0);
  next.octaves = std::clamp<std::uint32_t>(next.octaves, 1, arp_max_octaves);
  next.scale = std::min(next.scale, arp_scales.size() - 1);
  next.root %= static_cast<std::uint32_t>(semitones);
  next.seed = std::min(next.seed, static_cast<std::uint32_t>(arp_max_seed));
  if (std::find(arp_variation_bars.begin(), arp_variation_bars.end(), next.variation) ==
      arp_variation_bars.end()) {
    next.variation = 0;
  }
  next.strum = clamp_finite(next.strum, 0.0, 1.0, arp_default_strum);
  next.euclid_steps =
      std::clamp<std::uint32_t>(next.euclid_steps, 1, static_cast<std::uint32_t>(arp_max_steps));
  next.euclid_hits = std::min(next.euclid_hits, next.euclid_steps);
  next.euclid_rotate %= next.euclid_steps;
  auto& groove = next.groove;
  groove.steps = std::min(groove.steps, static_cast<std::uint32_t>(arp_groove_steps));
  groove.grid = std::min(groove.grid, arp_groove_grids.size() - 1);
  groove.amount = clamp_finite(groove.amount, 0.0, 1.0, 1.0);
  for (std::size_t i = 0; i < arp_groove_steps; ++i) {
    groove.timing[i] =
        clamp_finite(groove.timing[i], -arp_max_groove_timing, arp_max_groove_timing, 0.0);
    groove.velocity[i] = clamp_finite(groove.velocity[i], 0.0, arp_max_groove_velocity, 1.0);
  }
  for (std::size_t l = 0; l < arp_lane_count; ++l) {
    const auto& spec = arp_lane_specs[l];
    auto& lane = next.lanes[l];
    lane.length =
        std::clamp<std::uint32_t>(lane.length, 1, static_cast<std::uint32_t>(arp_max_steps));
    for (auto& value : lane.values) {
      value = clamp_finite(value, spec.minimum, spec.maximum, spec.fallback);
      if (spec.kind != ArpLaneKind::level) {
        value = std::round(value);
      }
    }
  }
  return next;
}

[[nodiscard]] bool condition_met(ArpCondition condition, std::int64_t cycle, bool fill) noexcept {
  const auto of = [cycle](std::int64_t x, std::int64_t y) {
    return positive_mod(cycle, y) == x - 1;
  };
  switch (condition) {
  case ArpCondition::always:
    return true;
  case ArpCondition::one_of_two:
    return of(1, 2);
  case ArpCondition::two_of_two:
    return of(2, 2);
  case ArpCondition::one_of_three:
    return of(1, 3);
  case ArpCondition::two_of_three:
    return of(2, 3);
  case ArpCondition::three_of_three:
    return of(3, 3);
  case ArpCondition::one_of_four:
    return of(1, 4);
  case ArpCondition::two_of_four:
    return of(2, 4);
  case ArpCondition::three_of_four:
    return of(3, 4);
  case ArpCondition::four_of_four:
    return of(4, 4);
  case ArpCondition::fill:
    return fill;
  case ArpCondition::not_fill:
    return !fill;
  case ArpCondition::first:
    return cycle == 0;
  case ArpCondition::not_first:
    return cycle != 0;
  }
  return true;
}

// Hits of a Euclidean rhythm before step k (k may be negative).
[[nodiscard]] std::int64_t euclid_hits_before(std::int64_t k, const ArpSettings& s) noexcept {
  const auto steps = static_cast<std::int64_t>(s.euclid_steps);
  const std::int64_t cycles = floor_div(k, steps);
  std::int64_t hits = cycles * static_cast<std::int64_t>(s.euclid_hits);
  for (std::int64_t j = 0; j < k - cycles * steps; ++j) {
    hits += euclid_hit(j, s.euclid_hits, s.euclid_steps, s.euclid_rotate) ? 1 : 0;
  }
  return hits;
}

[[nodiscard]] bool in_scale(int key, std::uint16_t mask, std::uint32_t root) noexcept {
  const auto pc = positive_mod(key - static_cast<int>(root), static_cast<std::int64_t>(semitones));
  return ((mask >> static_cast<unsigned>(pc)) & 1U) != 0;
}

[[nodiscard]] std::uint64_t gcd(std::uint64_t a, std::uint64_t b) noexcept {
  while (b != 0) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

[[nodiscard]] SequencerSettings sanitise(const SequencerSettings& settings) noexcept {
  SequencerSettings next = settings;
  next.clip = std::min(next.clip, sequencer_clips - 1);
  if (next.trigger > SequencerTrigger::note) {
    next.trigger = SequencerTrigger::transport;
  }
  next.root = std::min<std::uint8_t>(next.root, 127);
  return next;
}

} // namespace

namespace arp_detail {

std::int64_t Axis::raw_frame(double at) const noexcept {
  const double frames = (at - beat) / per_frame;
  if (std::isnan(frames)) {
    return static_cast<std::int64_t>(-far_frames);
  }
  return static_cast<std::int64_t>(
      std::ceil(std::clamp(frames, -far_frames, far_frames) - frame_epsilon));
}

std::int64_t Axis::due_frame(double at) const noexcept {
  return std::max<std::int64_t>(0, raw_frame(at));
}

bool Emitter::push(const GeneratedNote& note) noexcept {
  if (count_ >= buffer_.size()) {
    return false;
  }
  buffer_[count_++] = note;
  return true;
}

bool Emitter::cancel_on(const NoteEvent& note, std::uint32_t offset) noexcept {
  for (std::size_t i = count_; i-- > 0;) {
    const auto& candidate = buffer_[i];
    if (candidate.on && candidate.offset == offset && same_note(candidate.note, note)) {
      std::move(buffer_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                buffer_.begin() + static_cast<std::ptrdiff_t>(count_),
                buffer_.begin() + static_cast<std::ptrdiff_t>(i));
      --count_;
      return true;
    }
  }
  return false;
}

void Emitter::sort() noexcept {
  // Insertion sort: stable, in place and allocation-free; blocks are small.
  for (std::size_t i = 1; i < count_; ++i) {
    const GeneratedNote note = buffer_[i];
    std::size_t j = i;
    while (j > 0 && before(note, buffer_[j - 1])) {
      buffer_[j] = buffer_[j - 1];
      --j;
    }
    buffer_[j] = note;
  }
}

bool SoundingSet::add(const Sounding& note) noexcept {
  if (count_ >= notes_.size()) {
    return false;
  }
  notes_[count_++] = note;
  return true;
}

void SoundingSet::remove(std::size_t index) noexcept {
  notes_[index] = notes_[count_ - 1];
  --count_;
}

void SoundingSet::flush(const Axis& axis, std::int64_t limit, bool inclusive,
                        Emitter& out) noexcept {
  for (std::size_t i = 0; i < count_;) {
    const std::int64_t due = axis.due_frame(notes_[i].end_beat);
    if (inclusive ? due > limit : due >= limit) {
      ++i;
      continue;
    }
    if (!out.push(off_at(due, notes_[i].note))) {
      notes_[i].end_beat = -std::numeric_limits<double>::infinity(); // Next block, frame 0.
      ++i;
      continue;
    }
    remove(i);
  }
}

void SoundingSet::release(std::uint32_t offset, Emitter& out) noexcept {
  for (std::size_t i = 0; i < count_; ++i) {
    (void)out.push(off_at(offset, notes_[i].note));
  }
  count_ = 0;
}

bool KeyPool::empty() const noexcept {
  for (std::size_t key = 0; key < key_count; ++key) {
    if (held(key)) {
      return false;
    }
  }
  return true;
}

int KeyPool::newest() const noexcept {
  int found = -1;
  std::uint64_t latest = 0;
  for (std::size_t key = 0; key < key_count; ++key) {
    if (held(key) && stamp[key] >= latest) {
      latest = stamp[key];
      found = static_cast<int>(key);
    }
  }
  return found;
}

void KeyPool::press(std::uint8_t key, std::uint8_t level) noexcept {
  if (key >= key_count) {
    return;
  }
  if (count[key] == 0) {
    stamp[key] = next_stamp++;
  }
  if (count[key] < std::numeric_limits<std::uint16_t>::max()) {
    ++count[key];
    ++active;
  }
  velocity[key] = std::max<std::uint8_t>(level, 1);
}

bool KeyPool::release(std::uint8_t key, bool latch) noexcept {
  if (key >= key_count || count[key] == 0) {
    return false;
  }
  --count[key];
  --active;
  if (count[key] == 0 && latch) {
    latched[key] = true;
  }
  return true;
}

void KeyPool::clear() noexcept {
  count.fill(0);
  latched.fill(false);
  active = 0;
}

void HeldNotes::add(const NoteEvent& note) noexcept {
  if (count_ < notes_.size()) {
    notes_[count_++] = note;
  }
}

void HeldNotes::remove(const NoteEvent& note) noexcept {
  for (std::size_t i = 0; i < count_; ++i) {
    if (same_note(notes_[i], note)) {
      std::move(notes_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                notes_.begin() + static_cast<std::ptrdiff_t>(count_),
                notes_.begin() + static_cast<std::ptrdiff_t>(i));
      --count_;
      return;
    }
  }
}

// ---- Arpeggiator ----------------------------------------------------------

double StepPlan::time(std::uint32_t event) const noexcept {
  const std::uint32_t hit = event / count;
  const std::uint32_t note = event % count;
  return start + static_cast<double>(hit) * hit_beats +
         strum * static_cast<double>(note) / static_cast<double>(count);
}

void ArpStage::run(std::span<const GeneratedNote> in, const BlockContext& block,
                   const ArpSettings& settings, Emitter& out) noexcept {
  const auto frames = static_cast<std::int64_t>(block.frames);
  std::int64_t n = first_step(block.axis, settings);
  if (last_step_ != no_step) {
    n = std::max(n, last_step_ + 1);
  }
  std::size_t next = 0;
  for (;;) {
    // The plan's next note (a ratchet hit or a strummed note) comes before
    // the next step: they all land inside their step.
    const bool event = plan_.pending();
    const std::int64_t at =
        block.axis.raw_frame(event ? plan_.time(plan_.next) : step_start(n, settings));
    // Notes at a frame change the held keys before a step on that frame.
    if (next < in.size() && static_cast<std::int64_t>(in[next].offset) <= at) {
      apply(in[next++], settings);
      continue;
    }
    if (at >= frames) {
      break;
    }
    const std::int64_t frame = std::max<std::int64_t>(at, 0);
    if (event) {
      play_event(frame, block, out);
      continue;
    }
    plan_step(n, frame, block, settings, out);
    last_step_ = n;
    ++n;
  }
  sounding_.flush(block.axis, frames, false, out);
  if (pool_.empty()) {
    step_ = -1;
    variation_ = false;
  }
}

void ArpStage::apply(const GeneratedNote& note, const ArpSettings& settings) noexcept {
  const std::uint8_t key = note.note.key;
  if (key >= key_count) {
    return;
  }
  if (!note.on) {
    (void)pool_.release(key, settings.latch);
    return;
  }
  // A new chord: nothing pressed before it. It replaces a latched chord and
  // (with retrigger) restarts the pattern at the next step.
  if (pool_.active == 0) {
    pool_.unlatch();
    anchor_pending_ = true;
  }
  pool_.press(key, note.note.velocity);
}

std::size_t ArpStage::build_sequence(const ArpSettings& settings) noexcept {
  base_count_ = 0;
  for (std::size_t key = 0; key < key_count; ++key) {
    if (pool_.held(key)) {
      base_[base_count_++] = static_cast<std::uint8_t>(key);
    }
  }
  if (settings.mode == ArpMode::played) {
    for (std::size_t i = 1; i < base_count_; ++i) {
      const std::uint8_t key = base_[i];
      std::size_t j = i;
      while (j > 0 && pool_.stamp[key] < pool_.stamp[base_[j - 1]]) {
        base_[j] = base_[j - 1];
        --j;
      }
      base_[j] = key;
    }
  }
  // Passing notes: the scale's tones between the lowest and highest held
  // note join them (ascending modes only: Played keeps its order).
  const std::uint16_t mask = arp_scales[settings.scale].mask;
  if (settings.passing && mask != 0 && settings.mode != ArpMode::played && base_count_ > 0) {
    const std::uint8_t low = base_[0];
    const std::uint8_t high = base_[base_count_ - 1];
    std::size_t count = 0;
    for (int key = low; key <= high; ++key) {
      if (pool_.held(static_cast<std::size_t>(key)) || in_scale(key, mask, settings.root)) {
        base_[count++] = static_cast<std::uint8_t>(key);
      }
    }
    base_count_ = count;
  }
  std::size_t length = 0;
  for (std::uint32_t octave = 0; octave < settings.octaves; ++octave) {
    for (std::size_t i = 0; i < base_count_; ++i) {
      const std::uint32_t key = base_[i] + static_cast<std::uint32_t>(semitones) * octave;
      if (key < key_count) {
        sequence_[length] = static_cast<std::uint8_t>(key);
        // A passing note plays at the velocity of the held note below it.
        std::size_t below = i;
        while (below > 0 && !pool_.held(base_[below])) {
          --below;
        }
        sequence_velocity_[length] = pool_.velocity[base_[below]];
        ++length;
      }
    }
  }
  return length;
}

std::size_t ArpStage::build_order(std::size_t length, ArpMode mode) noexcept {
  std::size_t count = 0;
  const auto put = [&](std::size_t position) {
    order_[count++] = static_cast<std::uint16_t>(position);
  };
  switch (mode) {
  case ArpMode::down:
    for (std::size_t i = length; i-- > 0;) {
      put(i);
    }
    break;
  case ArpMode::up_down:
    // Up then back down, without repeating the top or the bottom note.
    for (std::size_t i = 0; i < length; ++i) {
      put(i);
    }
    for (std::size_t i = length - 1; i-- > 1;) {
      put(i);
    }
    break;
  case ArpMode::down_up:
    for (std::size_t i = length; i-- > 0;) {
      put(i);
    }
    for (std::size_t i = 1; i + 1 < length; ++i) {
      put(i);
    }
    break;
  case ArpMode::converge:
  case ArpMode::diverge: {
    // Outside in: lowest, highest, second lowest, second highest...
    std::size_t low = 0;
    std::size_t high = length - 1;
    while (low <= high) {
      put(low);
      if (low != high) {
        put(high);
      }
      ++low;
      if (high == 0) {
        break;
      }
      --high;
    }
    if (mode == ArpMode::diverge) {
      std::reverse(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(count));
    }
    break;
  }
  case ArpMode::thumb:
    // The lowest note between every other: 1 2 1 3 1 4...
    if (length == 1) {
      put(0);
    }
    for (std::size_t i = 1; i < length; ++i) {
      put(0);
      put(i);
    }
    break;
  case ArpMode::pinky:
    if (length == 1) {
      put(0);
    }
    for (std::size_t i = 0; i + 1 < length; ++i) {
      put(length - 1);
      put(i);
    }
    break;
  default:
    for (std::size_t i = 0; i < length; ++i) {
      put(i);
    }
    break;
  }
  return count;
}

// A random walk over `length` places: from the first at the phrase start,
// each step one place (sometimes two) up or down, held at the ends. Found
// from the phrase start, so it is a function of the step alone.
std::int64_t ArpStage::walk(std::int64_t k, std::size_t length, std::uint32_t seed) noexcept {
  const std::int64_t phrase = floor_div(k, arp_walk_phrase) * arp_walk_phrase;
  const auto top = static_cast<std::int64_t>(length) - 1;
  const auto step = [&](std::int64_t position, std::int64_t j) {
    const std::uint64_t h = hash_step(seed, walk_salt, j);
    const std::int64_t size = ((h >> 1U) % walk_leap_odds) == 0 ? 2 : 1;
    return std::clamp<std::int64_t>(position + ((h & 1U) != 0 ? size : -size), 0, top);
  };
  std::int64_t position = 0;
  if (walk_step_ != no_step && walk_step_ + 1 == k && walk_step_ >= phrase &&
      walk_length_ == length && walk_seed_ == seed) {
    position = step(walk_position_, k);
  } else {
    for (std::int64_t j = phrase + 1; j <= k; ++j) {
      position = step(position, j);
    }
  }
  walk_step_ = k;
  walk_length_ = length;
  walk_seed_ = seed;
  walk_position_ = position;
  return position;
}

// A key moved by scale degrees: snapped down onto the scale, then stepped.
int ArpStage::shift_degrees(int key, int degrees, const ArpSettings& settings) const noexcept {
  const std::uint16_t mask = arp_scales[settings.scale].mask;
  int x = key;
  for (std::size_t guard = 0; guard < semitones && !in_scale(x, mask, settings.root); ++guard) {
    --x;
  }
  const int direction = degrees < 0 ? -1 : 1;
  for (int d = 0; d < std::abs(degrees); ++d) {
    std::size_t guard = 0;
    do {
      x += direction;
    } while (!in_scale(x, mask, settings.root) && ++guard < semitones);
  }
  return x;
}

std::size_t ArpStage::choose(std::int64_t k, const StepLanes& lanes,
                             const ArpSettings& settings) noexcept {
  auto& keys = plan_.keys;
  auto& velocities = plan_.velocities;
  std::size_t count = 0;
  const auto add = [&](int key, std::uint8_t velocity) {
    if (key < 0 || key >= static_cast<int>(key_count) || count >= plan_capacity) {
      return;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (keys[i] == key) {
        return;
      }
    }
    keys[count] = static_cast<std::uint8_t>(key);
    velocities[count] = velocity;
    ++count;
  };
  const int octave_shift = static_cast<int>(semitones) * lanes.octave;
  const std::int64_t index = settings.mode == ArpMode::euclid ? euclid_hits_before(k, settings) : k;

  if (settings.mode == ArpMode::chord || settings.mode == ArpMode::strum) {
    const auto octave =
        static_cast<int>(positive_mod(index, static_cast<std::int64_t>(settings.octaves)));
    for (std::size_t key = 0; key < key_count; ++key) {
      if (pool_.held(key)) {
        add(static_cast<int>(key) + static_cast<int>(semitones) * octave + octave_shift,
            pool_.velocity[key]);
      }
    }
    return count;
  }

  const std::size_t length = build_sequence(settings);
  if (length == 0) {
    return 0;
  }
  const std::size_t period = build_order(length, settings.mode);
  const auto size = static_cast<std::int64_t>(length);
  std::int64_t place = 0; // A place in order_.
  switch (settings.mode) {
  case ArpMode::random:
    place = static_cast<std::int64_t>(hash_step(settings.seed, random_seed, index) %
                                      static_cast<std::uint64_t>(length));
    break;
  case ArpMode::shuffle: {
    // Every note once per cycle, in an order the seed and cycle pick: an
    // affine permutation j → a·j + b (mod length) with a coprime.
    const std::int64_t cycle = floor_div(index, size);
    const std::uint64_t h = hash_step(settings.seed, shuffle_salt, cycle);
    std::uint64_t a = 1 + (h % static_cast<std::uint64_t>(length));
    while (gcd(a, static_cast<std::uint64_t>(length)) != 1) {
      ++a;
    }
    const std::uint64_t b = (h >> 32U) % static_cast<std::uint64_t>(length);
    const auto j = static_cast<std::uint64_t>(positive_mod(index, size));
    place = static_cast<std::int64_t>((a * j + b) % static_cast<std::uint64_t>(length));
    break;
  }
  case ArpMode::walk:
    place = walk(k, length, settings.seed);
    break;
  case ArpMode::shape:
    place = positive_mod(lanes.note, size);
    break;
  default:
    place = positive_mod(index, static_cast<std::int64_t>(period));
    break;
  }
  std::size_t position = order_[static_cast<std::size_t>(place)];
  int key = sequence_[position];
  const std::uint8_t velocity = sequence_velocity_[position];
  // The chord-tone degree of the note (a place in base_ over octaves).
  auto degree = static_cast<std::int64_t>(position);
  const bool chord_scale = arp_scales[settings.scale].mask == 0;
  const auto from_degree = [&](std::int64_t d) {
    const auto base = static_cast<std::int64_t>(base_count_);
    return static_cast<int>(base_[static_cast<std::size_t>(positive_mod(d, base))]) +
           static_cast<int>(semitones) * static_cast<int>(floor_div(d, base));
  };
  if (settings.mode != ArpMode::shape && lanes.note != 0) {
    if (settings.note_mode == ArpNoteMode::held) {
      position = order_[static_cast<std::size_t>(
          positive_mod(place + lanes.note, static_cast<std::int64_t>(period)))];
      key = sequence_[position];
      degree = static_cast<std::int64_t>(position);
    } else if (chord_scale) {
      degree += lanes.note;
      key = from_degree(degree);
    } else {
      key = shift_degrees(key, lanes.note, settings);
    }
  }
  add(key + octave_shift, velocity);
  // The chord lane's notes over it.
  const auto stack = [&](std::initializer_list<int> degrees) {
    for (const int d : degrees) {
      const int above = chord_scale ? from_degree(degree + d) : shift_degrees(key, 2 * d, settings);
      add(above + octave_shift, velocity);
    }
  };
  constexpr int fifth = 7;
  constexpr int fourth = 5;
  switch (lanes.chord) {
  case ArpChord::off:
    break;
  case ArpChord::octave:
    add(key + static_cast<int>(semitones) + octave_shift, velocity);
    break;
  case ArpChord::fifth:
    if (chord_scale) {
      add(key + fifth + octave_shift, velocity);
    } else {
      add(shift_degrees(key, 4, settings) + octave_shift, velocity);
    }
    break;
  case ArpChord::triad:
    stack({1, 2});
    break;
  case ArpChord::seventh:
    stack({1, 2, 3});
    break;
  case ArpChord::sus:
    if (chord_scale) {
      add(key + fourth + octave_shift, velocity);
      add(key + fifth + octave_shift, velocity);
    } else {
      add(shift_degrees(key, 3, settings) + octave_shift, velocity);
      add(shift_degrees(key, 4, settings) + octave_shift, velocity);
    }
    break;
  }
  return count;
}

void ArpStage::plan_step(std::int64_t n, std::int64_t frame, const BlockContext& block,
                         const ArpSettings& settings, Emitter& out) noexcept {
  const Axis& axis = block.axis;
  plan_.clear();
  if (pool_.empty()) {
    step_ = -1;
    variation_ = false;
    sounding_.flush(axis, frame, true, out);
    return;
  }
  if (anchor_pending_) {
    anchor_ = n;
    anchor_pending_ = false;
  }
  const std::int64_t k = settings.retrigger ? n - anchor_ : n;
  step_ = static_cast<int>(std::clamp<std::int64_t>(k, 0, largest_reported_step));

  // A variation bar: the last of every `variation` bars of the song.
  const double rate = rate_of(settings);
  const auto bar = static_cast<std::int64_t>(
      std::floor(static_cast<double>(n) * rate / block.beats_per_bar + grid_epsilon));
  const auto bars = static_cast<std::int64_t>(settings.variation);
  const bool vary = bars > 0 && positive_mod(bar, bars) == bars - 1;
  variation_ = vary;
  const bool fill = settings.fill || vary;
  const std::int64_t shift =
      vary ? 1 + static_cast<std::int64_t>(hash_step(settings.seed, variation_salt, bar) %
                                           variation_shifts)
           : 0;
  const auto read = [&](ArpLane lane, std::int64_t offset = 0) {
    const auto& values = settings.lane(lane);
    return values.values[static_cast<std::size_t>(
        positive_mod(k + offset, static_cast<std::int64_t>(values.length)))];
  };

  const double start = step_start(n, settings);
  const double next = step_start(n + 1, settings);
  const double lane_gate = read(ArpLane::gate);
  const auto hits = static_cast<std::uint32_t>(
      std::clamp<long>(std::lround(read(ArpLane::ratchet)), 1, arp_max_ratchet));
  const bool tie = read(ArpLane::tie) >= 0.5;
  const double hit_beats = (next - start) / static_cast<double>(hits);
  // One hit: the gate, never past the next step (a swung step is shorter
  // than the rate); a ratchet's hits share the step.
  const double hit_gate = hits == 1 ? std::min(settings.gate * lane_gate * rate, next - start)
                                    : std::min(settings.gate * lane_gate * hit_beats, hit_beats);
  const auto condition = static_cast<ArpCondition>(std::lround(read(ArpLane::condition)));
  const std::int64_t cycle =
      floor_div(k, static_cast<std::int64_t>(settings.lane(ArpLane::condition).length));

  // Rests: Gate at 0, a Euclidean miss, a failed condition or chance, or
  // a note too short to sound.
  bool rest = lane_gate <= 0.0 || !condition_met(condition, cycle, fill) ||
              unit(hash_step(settings.seed, chance_salt, k)) >= read(ArpLane::chance);
  if (settings.mode == ArpMode::euclid &&
      !euclid_hit(k, settings.euclid_hits, settings.euclid_steps, settings.euclid_rotate)) {
    rest = true;
  }
  const double first_end = hits == 1 && tie ? next : start + hit_gate;
  if (rest || axis.raw_frame(first_end) <= frame) {
    sounding_.flush(axis, frame, true, out);
    return;
  }

  StepLanes lanes;
  lanes.note = static_cast<int>(std::lround(read(ArpLane::note, shift)));
  lanes.octave = static_cast<int>(std::lround(read(ArpLane::octave, shift)));
  lanes.chord = static_cast<ArpChord>(std::lround(read(ArpLane::chord)));
  const std::size_t count = choose(k, lanes, settings);
  if (count == 0) {
    sounding_.flush(axis, frame, true, out);
    return;
  }
  const bool accent = read(ArpLane::accent) >= 0.5;
  const double level = read(ArpLane::velocity) * groove_velocity(n, settings);
  for (std::size_t c = 0; c < count; ++c) {
    const double played = settings.velocity == ArpVelocity::pattern
                              ? 1.0
                              : static_cast<double>(plan_.velocities[c]) / 127.0;
    plan_.velocities[c] = to_velocity(played * level + (accent ? arp_accent_velocity : 0.0));
  }
  plan_.step = n;
  plan_.hits = hits;
  plan_.count = static_cast<std::uint32_t>(count);
  plan_.events = hits * plan_.count;
  plan_.next = 0;
  plan_.start = start;
  plan_.next_start = next;
  plan_.hit_beats = hit_beats;
  plan_.hit_gate = hit_gate;
  plan_.strum = settings.mode == ArpMode::strum ? settings.strum * hit_beats : 0.0;
  plan_.strum_down = settings.mode == ArpMode::strum && positive_mod(k, 2) == 1;
  plan_.tie = tie;
  plan_.mod = static_cast<float>(read(ArpLane::mod));
  plan_.accent = accent;
  // A tied note this step plays again continues instead of retriggering
  // (one hit, not strummed); velocity 0 marks it as continued.
  if (hits == 1 && plan_.strum == 0.0) {
    for (std::size_t c = 0; c < count; ++c) {
      for (std::size_t i = 0; i < sounding_.size(); ++i) {
        Sounding& sounding = sounding_[i];
        if (sounding.tied && sounding.note.key == plan_.keys[c] &&
            axis.due_frame(sounding.end_beat) <= frame) {
          sounding.end_beat = first_end;
          sounding.tied = tie;
          plan_.velocities[c] = 0;
          break;
        }
      }
    }
  }
  sounding_.flush(axis, frame, true, out);
}

void ArpStage::play_event(std::int64_t frame, const BlockContext& block, Emitter& out) noexcept {
  const Axis& axis = block.axis;
  const std::uint32_t event = plan_.next++;
  const std::uint32_t hit = event / plan_.count;
  const std::uint32_t at = event % plan_.count;
  const std::uint32_t note = plan_.strum_down ? plan_.count - 1 - at : at;
  sounding_.flush(axis, frame, true, out);
  std::uint8_t velocity = plan_.velocities[note];
  if (velocity == 0 || sounding_.size() >= sounding_capacity) {
    return; // A continued tie.
  }
  if (hit > 0) {
    velocity = static_cast<std::uint8_t>(
        std::max(1L, std::lround(static_cast<double>(velocity) * arp_ratchet_velocity)));
  }
  const bool last = hit + 1 == plan_.hits;
  const double hit_end =
      last ? plan_.next_start : plan_.start + static_cast<double>(hit + 1) * plan_.hit_beats;
  const bool tied = last && plan_.tie;
  const double end =
      tied ? plan_.next_start : std::min(plan_.time(event) + plan_.hit_gate, hit_end);
  if (axis.raw_frame(end) <= frame) {
    return;
  }
  const NoteEvent minted = block.ids.mint(plan_.keys[note], velocity);
  if (out.push(
          {static_cast<std::uint32_t>(frame), true, minted, plan_.mod, plan_.accent && hit == 0})) {
    (void)sounding_.add({minted, end, tied, -1});
  }
}

void ArpStage::release(Emitter& out) noexcept {
  sounding_.release(0, out);
  plan_.clear();
  last_step_ = no_step;
  anchor_pending_ = true;
}

void ArpStage::forget() noexcept {
  pool_.clear();
  sounding_.clear();
  plan_.clear();
  last_step_ = no_step;
  anchor_pending_ = true;
  step_ = -1;
  variation_ = false;
  walk_step_ = no_step;
}

// ---- Clip sequencer -------------------------------------------------------

void SequencerStage::run(std::span<const GeneratedNote> in, const BlockContext& block,
                         const SequencerSettings& settings, const SequencerClips& clips,
                         bool transport, Emitter& out) noexcept {
  if (settings.trigger == SequencerTrigger::transport) {
    const Player& player = players_[settings.clip];
    if (transport && !player.active) {
      start(settings.clip, 0.0, false);
    } else if (!transport && !player.active && keys_.active + forwarded_ > 0) {
      start(settings.clip, quantise(block.axis.beat), true);
    }
  }
  const std::uint32_t last = block.frames - 1;
  std::uint32_t from = 0;
  std::size_t next = 0;
  for (;;) {
    const std::uint32_t to = next < in.size() ? std::min(in[next].offset, last) : block.frames;
    if (to > from) {
      play(from, to, block, settings, clips, out);
      from = to;
    }
    if (next >= in.size()) {
      break;
    }
    // Notes at a frame act before the clip notes on that frame.
    while (next < in.size() && std::min(in[next].offset, last) == to) {
      apply(in[next++], to, block, settings, transport, out);
    }
  }
  sounding_.flush(block.axis, block.frames, false, out);
}

void SequencerStage::apply(const GeneratedNote& note, std::uint32_t frame,
                           const BlockContext& block, const SequencerSettings& settings,
                           bool transport, Emitter& out) noexcept {
  const std::uint8_t key = note.note.key;
  if (key >= key_count) {
    return;
  }
  const int trigger = static_cast<int>(key) - static_cast<int>(settings.root);
  if (settings.trigger == SequencerTrigger::note && trigger >= 0 &&
      trigger < static_cast<int>(sequencer_clips)) {
    const auto clip = static_cast<std::size_t>(trigger);
    if (note.on) {
      if (triggers_[clip]++ == 0) {
        start(clip, quantise(block.axis.beat_at(frame)), true);
      }
    } else if (triggers_[clip] > 0 && --triggers_[clip] == 0) {
      stop(clip, frame, block.axis, out);
    }
    return;
  }
  if (settings.follow) {
    if (note.on) {
      keys_.press(key, note.note.velocity);
    } else {
      (void)keys_.release(key, false);
    }
  } else if (note.on) {
    GeneratedNote passed = note;
    passed.offset = frame;
    (void)out.push(passed);
    ++forwarded_;
  } else {
    if (!out.cancel_on(note.note, frame)) {
      GeneratedNote passed = note;
      passed.offset = frame;
      (void)out.push(passed);
    }
    forwarded_ = forwarded_ > 0 ? forwarded_ - 1 : 0;
  }
  // Without a transport, the selected clip plays while any key is held.
  if (!transport && settings.trigger == SequencerTrigger::transport) {
    const bool held = keys_.active + forwarded_ > 0;
    if (held && !players_[settings.clip].active) {
      start(settings.clip, quantise(block.axis.beat_at(frame)), true);
    } else if (!held && players_[settings.clip].active) {
      stop(settings.clip, frame, block.axis, out);
    }
  }
}

void SequencerStage::start(std::size_t clip, double origin, bool from_press) noexcept {
  players_[clip] = {true, from_press, origin, next_stamp_++};
}

void SequencerStage::stop(std::size_t clip, std::uint32_t frame, const Axis& axis,
                          Emitter& out) noexcept {
  players_[clip].active = false;
  for (std::size_t i = 0; i < sounding_.size();) {
    if (sounding_[i].clip != static_cast<int>(clip)) {
      ++i;
      continue;
    }
    const std::int64_t due = std::min<std::int64_t>(axis.due_frame(sounding_[i].end_beat), frame);
    (void)out.push(off_at(due, sounding_[i].note));
    sounding_.remove(i);
  }
}

void SequencerStage::play(std::uint32_t from, std::uint32_t to, const BlockContext& block,
                          const SequencerSettings& settings, const SequencerClips& clips,
                          Emitter& out) noexcept {
  const Axis& axis = block.axis;
  const int newest = keys_.newest();
  const int transpose = settings.follow && newest >= 0 ? newest - settings.root : 0;
  std::size_t pending = 0;
  for (std::size_t clip = 0; clip < sequencer_clips; ++clip) {
    const Player& player = players_[clip];
    const double length = clips.length_beats[clip];
    const auto& notes = clips.notes[clip];
    if (!player.active || notes.empty() || !(length >= shortest_clip_beats)) {
      continue;
    }
    // Every loop pass that can start a note in [from, to), with a frame of
    // margin for rounding; the frame test below decides.
    const double first = axis.beat_at(from) - axis.per_frame;
    const double last = axis.beat_at(to);
    auto pass = static_cast<std::int64_t>(std::floor((first - player.origin) / length));
    const auto final_pass = static_cast<std::int64_t>(std::floor((last - player.origin) / length));
    if (player.from_press) {
      pass = std::max<std::int64_t>(pass, 0);
    }
    for (; pass <= final_pass; ++pass) {
      const double loop_start = player.origin + static_cast<double>(pass) * length;
      for (const auto& note : notes) {
        const double at = loop_start + note.start_beats;
        const std::int64_t frame = axis.raw_frame(at);
        if (frame < from || frame >= to) {
          continue;
        }
        const int key = note.key + transpose;
        const double end = at + std::min(note.length_beats, length);
        if (key < 0 || key >= static_cast<int>(key_count) || axis.raw_frame(end) <= frame ||
            pending >= pending_.size()) {
          continue;
        }
        pending_[pending++] = {frame, static_cast<std::uint8_t>(key),
                               std::max<std::uint8_t>(note.velocity, 1), static_cast<int>(clip),
                               end};
      }
    }
  }
  // Mint in time order so ids do not depend on where blocks split.
  for (std::size_t i = 1; i < pending; ++i) {
    const PendingOn on = pending_[i];
    std::size_t j = i;
    while (j > 0 && (on.frame < pending_[j - 1].frame ||
                     (on.frame == pending_[j - 1].frame &&
                      (on.key < pending_[j - 1].key ||
                       (on.key == pending_[j - 1].key && on.clip < pending_[j - 1].clip))))) {
      pending_[j] = pending_[j - 1];
      --j;
    }
    pending_[j] = on;
  }
  for (std::size_t i = 0; i < pending; ++i) {
    const PendingOn& on = pending_[i];
    if (sounding_.size() >= sounding_capacity) {
      continue;
    }
    const NoteEvent note = block.ids.mint(on.key, on.velocity);
    if (out.push({static_cast<std::uint32_t>(on.frame), true, note})) {
      (void)sounding_.add({note, on.end_beat, false, on.clip});
    }
  }
}

void SequencerStage::release(Emitter& out) noexcept { sounding_.release(0, out); }

void SequencerStage::requantise(const Axis& axis, bool transport) noexcept {
  for (auto& player : players_) {
    if (!player.active) {
      continue;
    }
    if (player.from_press) {
      player.origin = quantise(axis.beat);
    } else if (!transport) {
      player.active = false; // Song-looped clips need the song.
    }
  }
}

void SequencerStage::forget() noexcept {
  players_.fill({});
  triggers_.fill(0);
  keys_.clear();
  forwarded_ = 0;
  sounding_.clear();
}

bool SequencerStage::idle() const noexcept {
  for (std::size_t clip = 0; clip < sequencer_clips; ++clip) {
    if (players_[clip].active || triggers_[clip] > 0) {
      return false;
    }
  }
  return sounding_.size() == 0 && keys_.active == 0 && forwarded_ == 0;
}

int SequencerStage::playing_clip() const noexcept {
  int found = -1;
  std::uint64_t latest = 0;
  for (std::size_t clip = 0; clip < sequencer_clips; ++clip) {
    if (players_[clip].active && players_[clip].stamp >= latest) {
      latest = players_[clip].stamp;
      found = static_cast<int>(clip);
    }
  }
  return found;
}

double SequencerStage::playhead(const SequencerClips& clips, double beat) const noexcept {
  const int clip = playing_clip();
  if (clip < 0) {
    return 0.0;
  }
  const Player& player = players_[static_cast<std::size_t>(clip)];
  const double length = clips.length_beats[static_cast<std::size_t>(clip)];
  const double position = beat - player.origin;
  if (!(position > 0.0) || !(length > 0.0)) {
    return 0.0;
  }
  return std::fmod(position, length);
}

} // namespace arp_detail

// ---- Generator ------------------------------------------------------------

void NoteGenerator::State::forget() noexcept {
  rebuild = false;
  release_arp = false;
  unlatch = false;
  have_axis = false;
  clock_running = false;
  clock_frame = 0;
  ids.next = 1;
  held.clear();
  passed_count = 0;
  arp_stage.forget();
  sequencer_stage.forget();
  playing_clip = -1;
  playhead = 0.0;
  arp_step = -1;
  arp_variation = false;
}

bool NoteGenerator::State::idle() const noexcept {
  return held.notes().empty() && arp_stage.idle() && sequencer_stage.idle();
}

std::span<const GeneratedNote>
NoteGenerator::State::prepare_input(std::span<const GeneratedNote> played, bool replay) noexcept {
  if (!replay) {
    return played;
  }
  // The pipeline changed: the held notes go in again at frame 0, except
  // those released on that very frame.
  std::size_t count = 0;
  for (const auto& note : held.notes()) {
    const bool released = std::any_of(played.begin(), played.end(), [&](const auto& event) {
      return event.offset == 0 && !event.on && same_note(event.note, note);
    });
    if (!released && count < input.size()) {
      input[count++] = {0, true, note};
    }
  }
  for (const auto& note : played) {
    if (count < input.size()) {
      input[count++] = note;
    }
  }
  return std::span(input).first(count);
}

void NoteGenerator::State::track_held(std::span<const GeneratedNote> played) noexcept {
  for (const auto& note : played) {
    if (note.on) {
      held.add(note.note);
    } else {
      held.remove(note.note);
    }
  }
}

void NoteGenerator::State::track_passed(std::span<const GeneratedNote> notes) noexcept {
  for (const auto& note : notes) {
    if (note.note.clip == arp_detail::generated_clip) {
      continue;
    }
    if (note.on) {
      if (passed_count < passed.size()) {
        passed[passed_count++] = note.note;
      }
      continue;
    }
    for (std::size_t i = 0; i < passed_count; ++i) {
      if (same_note(passed[i], note.note)) {
        passed[i] = passed[--passed_count];
        break;
      }
    }
  }
}

void NoteGenerator::State::release_passed(arp_detail::Emitter& out) noexcept {
  for (std::size_t i = 0; i < passed_count; ++i) {
    (void)out.push(off_at(0, passed[i]));
  }
  passed_count = 0;
}

std::size_t
NoteGenerator::State::process(std::span<const GeneratedNote> played,
                              const SynthTransport& transport, std::uint32_t frames,
                              std::span<GeneratedNote, max_generated_notes> out) noexcept {
  arp_detail::Emitter emit(out);
  // The sequencer's notes reach the voices only when the arp is off; when
  // it feeds the arp, its releases go to the arp's input, never out.
  arp_detail::Emitter sequenced(sequencer_out);
  if (frames == 0) {
    return 0;
  }
  const bool replay = rebuild;
  if (rebuild) {
    release_passed(emit);
    arp_stage.release(emit);
    arp_stage.forget();
    sequencer_stage.release(sequencer_into_arp ? sequenced : emit);
    sequencer_stage.forget();
    rebuild = false;
    release_arp = false;
    unlatch = false;
  }
  if (release_arp) {
    arp_stage.release(emit);
    release_arp = false;
  }
  if (unlatch) {
    arp_stage.unlatch();
    unlatch = false;
  }
  sequencer_into_arp = arp.on && sequencer.on;
  auto& sequencer_target = sequencer_into_arp ? sequenced : emit;
  const auto in = prepare_input(played, replay);

  if (!arp.on && !sequencer.on) {
    for (const auto& note : in) {
      (void)emit.push(note);
    }
    if (replay) {
      emit.sort();
    }
    track_passed(emit.notes());
    track_held(played);
    have_axis = false;
    clock_running = false;
    playing_clip = -1;
    playhead = 0.0;
    arp_step = -1;
    arp_variation = false;
    return emit.size();
  }

  // The axis: song beats while the transport runs, else a free 120 BPM
  // clock from the first held note. A change of axis, or a jump in the song
  // without a reset, releases the generated notes.
  const bool on_transport = transport.beats_per_frame > 0.0;
  const bool switched = have_axis && on_transport != transport_axis;
  if (switched) {
    arp_stage.release(emit);
    sequencer_stage.release(sequencer_target);
    clock_running = false;
  }
  arp_detail::Axis axis;
  if (on_transport) {
    axis = {transport.beat_at_block_start, transport.beats_per_frame};
    const double tolerance = std::max(1e-3, 2.0 * transport.beats_per_frame);
    if (have_axis && transport_axis && std::fabs(axis.beat - expected_beat) > tolerance) {
      arp_stage.release(emit);
      sequencer_stage.release(sequencer_target);
      sequencer_stage.requantise(axis, true);
    }
    expected_beat = axis.beat + transport.beats_per_frame * static_cast<double>(frames);
  } else {
    const double per_frame = free_beats_per_second / sample_rate;
    if (!clock_running) {
      if (!idle()) {
        clock_frame = 0;
        clock_running = true;
      } else {
        for (const auto& note : in) {
          if (note.on) {
            clock_frame = -static_cast<std::int64_t>(note.offset);
            clock_running = true;
            break;
          }
        }
      }
      // A new clock counts steps from zero.
      arp_stage.restart();
    }
    axis = {static_cast<double>(clock_frame) * per_frame, per_frame};
  }
  if (!on_transport && !clock_running) {
    // Nothing held, nothing sounding and no note starts: no axis to play on.
    have_axis = true;
    transport_axis = false;
    playing_clip = -1;
    playhead = 0.0;
    arp_step = -1;
    arp_variation = false;
    emit.sort();
    track_passed(emit.notes());
    track_held(played);
    return emit.size();
  }
  if (switched) {
    sequencer_stage.requantise(axis, on_transport);
  }
  have_axis = true;
  transport_axis = on_transport;

  const double beats_per_bar =
      transport.beats_per_bar > 0.0 && std::isfinite(transport.beats_per_bar)
          ? transport.beats_per_bar
          : free_beats_per_bar;
  const arp_detail::BlockContext block{axis, frames, ids, beats_per_bar};
  std::span<const GeneratedNote> arp_input = in;
  if (sequencer.on) {
    const SequencerClips& source = clips != nullptr ? *clips : no_clips;
    // Played → sequencer → arpeggiator: the sequencer's notes are the
    // arpeggiator's held keys.
    sequencer_stage.run(in, block, sequencer, source, on_transport, sequencer_target);
    if (arp.on) {
      sequenced.sort();
      arp_input = sequenced.notes();
    }
    playing_clip = sequencer_stage.playing_clip();
    playhead = sequencer_stage.playhead(source, axis.beat_at(frames));
  } else {
    playing_clip = -1;
    playhead = 0.0;
  }
  if (arp.on) {
    arp_stage.run(arp_input, block, arp, emit);
    arp_step = arp_stage.step();
    arp_variation = arp_step >= 0 && arp_stage.variation();
  } else {
    arp_step = -1;
    arp_variation = false;
  }
  emit.sort();
  track_passed(emit.notes());
  track_held(played);
  if (!on_transport && clock_running) {
    if (idle()) {
      clock_running = false;
    } else {
      clock_frame += static_cast<std::int64_t>(frames);
    }
  }
  return emit.size();
}

// ---- Public API -----------------------------------------------------------

SequencerClips compile_sequencer_clips(const project::NativeSynthDocument& document) {
  constexpr double quarter = static_cast<double>(timeline::ticks_per_quarter);
  SequencerClips clips;
  for (std::size_t clip = 0; clip < sequencer_clips; ++clip) {
    const std::string id = "clip." + std::to_string(clip + 1);
    const auto found =
        std::find_if(document.sequences.begin(), document.sequences.end(),
                     [&](const project::NativeSequence& sequence) { return sequence.id == id; });
    if (found == document.sequences.end()) {
      continue; // One empty bar.
    }
    double length = static_cast<double>(found->length.value) / quarter;
    if (!(length > 0.0)) {
      length = 4.0;
    }
    length = std::clamp(length, shortest_clip_beats, longest_clip_beats);
    clips.length_beats[clip] = length;
    auto& notes = clips.notes[clip];
    notes.reserve(found->notes.size());
    for (const auto& note : found->notes) {
      const double start = static_cast<double>(note.start.value) / quarter;
      const double duration = static_cast<double>(note.length.value) / quarter;
      if (start < 0.0 || start >= length || !(duration > 0.0) || note.key >= 128) {
        continue;
      }
      notes.push_back({start, duration, note.key, std::clamp<std::uint8_t>(note.velocity, 1, 127)});
    }
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
      return a.start_beats < b.start_beats || (a.start_beats == b.start_beats && a.key < b.key);
    });
  }
  return clips;
}

NoteGenerator::NoteGenerator() : state_(std::make_unique<State>()) {}

NoteGenerator::~NoteGenerator() = default;

void NoteGenerator::prepare(double sample_rate) {
  auto state = std::make_unique<State>();
  if (state_) {
    state->arp = state_->arp;
    state->sequencer = state_->sequencer;
    state->clips = state_->clips;
  }
  state->sample_rate = sample_rate > 0.0 && std::isfinite(sample_rate) ? sample_rate : 48'000.0;
  state_ = std::move(state);
}

void NoteGenerator::reset() noexcept {
  if (state_) {
    state_->forget();
  }
}

void NoteGenerator::set(const ArpSettings& arp, const SequencerSettings& sequencer) noexcept {
  if (!state_) {
    return;
  }
  State& state = *state_;
  const ArpSettings next_arp = sanitise(arp);
  const SequencerSettings next_sequencer = sanitise(sequencer);
  const ArpSettings& was = state.arp;
  const SequencerSettings& was_sequencer = state.sequencer;
  // The pipeline changes: release everything and replay the held notes.
  const bool pipeline = next_arp.on != was.on || next_sequencer.on != was_sequencer.on ||
                        (next_sequencer.on && (next_sequencer.clip != was_sequencer.clip ||
                                               next_sequencer.trigger != was_sequencer.trigger ||
                                               next_sequencer.root != was_sequencer.root ||
                                               next_sequencer.follow != was_sequencer.follow));
  // The pattern changes: release the arpeggiated notes, keep the chord.
  // Gate, swing, velocity and the step pattern apply from the next step.
  const bool pattern = next_arp.on && (next_arp.mode != was.mode || next_arp.rate != was.rate ||
                                       next_arp.octaves != was.octaves);
  if (pipeline) {
    state.rebuild = true;
  } else if (pattern) {
    state.release_arp = true;
  }
  if (was.latch && !next_arp.latch) {
    state.unlatch = true;
  }
  state.arp = next_arp;
  state.sequencer = next_sequencer;
}

void NoteGenerator::adopt(const SequencerClips& clips) noexcept {
  if (state_) {
    state_->clips = &clips;
  }
}

bool NoteGenerator::active() const noexcept {
  return state_ && (state_->arp.on || state_->sequencer.on);
}

std::size_t NoteGenerator::process(std::span<const GeneratedNote> played,
                                   const SynthTransport& transport, std::uint32_t frames,
                                   std::span<GeneratedNote, max_generated_notes> out) noexcept {
  if (!state_) {
    const std::size_t count = std::min(played.size(), out.size());
    std::copy_n(played.begin(), count, out.begin());
    return count;
  }
  return state_->process(played, transport, frames, out);
}

int NoteGenerator::playing_clip() const noexcept { return state_ ? state_->playing_clip : -1; }

double NoteGenerator::clip_playhead_beats() const noexcept {
  return state_ ? state_->playhead : 0.0;
}

int NoteGenerator::arp_step() const noexcept { return state_ ? state_->arp_step : -1; }

bool NoteGenerator::arp_variation() const noexcept { return state_ && state_->arp_variation; }

bool euclid_hit(std::int64_t k, std::uint32_t hits, std::uint32_t steps,
                std::uint32_t rotate) noexcept {
  const auto size = static_cast<std::int64_t>(std::max<std::uint32_t>(steps, 1));
  const auto count = static_cast<std::int64_t>(std::min<std::uint32_t>(hits, steps));
  const std::int64_t i = positive_mod(k + static_cast<std::int64_t>(rotate), size);
  return (i * count) % size < count;
}

std::uint64_t arp_period(const ArpSettings& settings) noexcept {
  // Saturates rather than overflowing; the panel shows the large ones as such.
  constexpr std::uint64_t ceiling = std::uint64_t{1} << 53U;
  std::uint64_t period = 1;
  const auto join = [&](std::uint64_t length) {
    length = std::max<std::uint64_t>(length, 1);
    period = std::min(ceiling, period / gcd(period, length) * length);
  };
  for (const auto& lane : settings.lanes) {
    join(lane.length);
  }
  if (settings.mode == ArpMode::euclid) {
    join(settings.euclid_steps);
  }
  return period;
}

} // namespace oscillate::native_dsp::synth::nova
