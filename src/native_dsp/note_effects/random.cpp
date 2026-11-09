// Random (ADR-0059): seeded changes to pitch (optionally kept in a scale),
// velocity and a late start. Every choice hashes the seed, the key and the
// note's tick, so export and live playback make the same choices, and a
// loop repeats them.
#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::random;
using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

constexpr auto chance = tables::parameter_index(table::parameters, "random.chance");
constexpr auto range = tables::parameter_index(table::parameters, "random.range");
constexpr auto root = tables::parameter_index(table::parameters, "random.root");
constexpr auto scale_choice = tables::parameter_index(table::parameters, "random.scale");
constexpr auto velocity_amount = tables::parameter_index(table::parameters, "random.velocity");
constexpr auto timing = tables::parameter_index(table::parameters, "random.timing");
constexpr auto seed = tables::parameter_index(table::parameters, "random.seed");
// Timing moves a note up to a sixteenth late.
constexpr std::int64_t timing_span = tables::division_ticks[tables::sixteenth];

struct Moved {
  std::uint8_t key = 0;
  std::int64_t on_tick = 0;
  std::int64_t delay = 0;
};

class RandomKernel final : public NoteEffectKernel {
public:
  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    support::Emitter out(output);
    for (const auto& event : input) {
      const auto tick = clock.tick_at(event.offset);
      if (event.kind == MidiEventKind::note_off) {
        const auto* moved = notes_.find(event.clip_id_bits, event.note_id_bits);
        if (moved == nullptr) {
          out.pass(event);
          continue;
        }
        if (moved->delay == 0) {
          out.note_off(event.offset, event.clip_id_bits, event.note_id_bits, moved->key,
                       event.boundary_crop);
        } else {
          const auto release = std::max(tick, moved->on_tick + 1) + moved->delay;
          (void)pending_.push({release, MidiEventKind::note_off, event.clip_id_bits,
                               event.note_id_bits, moved->key, 0});
        }
        notes_.erase(event.clip_id_bits, event.note_id_bits);
        continue;
      }
      const auto at = event.offset;
      const auto hash = support::mix(static_cast<std::uint64_t>(parameters.index_at(seed, at)),
                                     support::mix(event.key, static_cast<std::uint64_t>(tick)));
      int key = event.key;
      if (support::unit(support::mix(hash, 1)) < std::clamp(parameters.at(chance, at), 0.0, 1.0)) {
        const int span = std::max(1, parameters.index_at(range, at));
        const int distance =
            1 + static_cast<int>(support::unit(support::mix(hash, 2)) * static_cast<double>(span));
        const int candidate =
            key + ((support::mix(hash, 3) & 1U) != 0 ? std::min(distance, span) : -distance);
        const auto scale = support::Scale::from(parameters.index_at(root, at),
                                                parameters.index_at(scale_choice, at));
        const int snapped = scale.snap(candidate, tables::scale::nearest);
        if (snapped >= 0 && snapped <= 127)
          key = snapped;
      }
      const double swing = std::clamp(parameters.at(velocity_amount, at), 0.0, 1.0);
      const int velocity =
          std::clamp(static_cast<int>(std::lround(
                         static_cast<double>(event.velocity) +
                         (support::unit(support::mix(hash, 4)) * 2.0 - 1.0) * swing * 64.0)),
                     1, 127);
      const auto delay = static_cast<std::int64_t>(support::unit(support::mix(hash, 5)) *
                                                   std::clamp(parameters.at(timing, at), 0.0, 1.0) *
                                                   static_cast<double>(timing_span));
      const Moved moved{static_cast<std::uint8_t>(key), tick, delay};
      if (notes_.insert(event.clip_id_bits, event.note_id_bits, moved) == nullptr) {
        out.pass(event); // No room to remember a change: play it as it came.
        continue;
      }
      if (delay == 0) {
        if (!out.note_on(at, event.clip_id_bits, event.note_id_bits, key, velocity))
          notes_.erase(event.clip_id_bits, event.note_id_bits);
      } else if (pending_.size() >= starts_room ||
                 !pending_.push({tick + delay, MidiEventKind::note_on, event.clip_id_bits,
                                 event.note_id_bits, static_cast<std::uint8_t>(key),
                                 static_cast<std::uint8_t>(velocity)})) {
        notes_.erase(event.clip_id_bits, event.note_id_bits);
      }
    }
    pending_.fire(clock, out);
    return out.finish();
  }

  void reset() noexcept override {
    notes_.clear();
    pending_.clear();
  }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    for (auto& entry : notes_.entries())
      out.note_off(0, entry.clip, entry.note, entry.value.key);
    pending_.flush_offs(out);
    notes_.clear();
    return out.finish();
  }

private:
  support::NoteMap<Moved, 256> notes_;
  // Late starts use at most this much of the queue, so every remembered
  // note's note-off always has room.
  static constexpr std::size_t starts_room = 256;
  support::Pending<starts_room + 256> pending_;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 3> humanise{
    {{"random.chance", 0.0}, {"random.velocity", 0.2}, {"random.timing", 0.12}}};
constexpr std::array<V, 2> velocity_only{{{"random.chance", 0.0}, {"random.velocity", 0.5}}};
constexpr std::array<V, 1> major_dice{{{"random.scale", tables::major_scale}}};
constexpr std::array<V, 3> sparkle{
    {{"random.chance", 0.3}, {"random.scale", 10}, {"random.range", 12}}};
constexpr std::array<V, 2> octave_jumps{{{"random.chance", 0.35}, {"random.scale", 13}}};
constexpr std::array<V, 3> loose{
    {{"random.chance", 0.15}, {"random.range", 2}, {"random.velocity", 0.3}}};

constexpr std::array<Preset, 7> presets{{
    {"Pitch Dice", "Pitch", {}},
    {"Melody Dice in Major", "Pitch", major_dice},
    {"Pentatonic Sparkle", "Pitch", sparkle},
    {"Wide Jumps", "Pitch", octave_jumps},
    {"Humanise", "Feel", humanise},
    {"Velocity Only", "Feel", velocity_only},
    {"Loose Hands", "Feel", loose},
}};

constexpr EngineDescriptor descriptor{"random", "Random", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(IdBits generated_clip) {
  (void)generated_clip;
  return std::make_unique<RandomKernel>();
}

double tail(std::span<const double> values) {
  (void)values;
  return static_cast<double>(timing_span) / static_cast<double>(tables::quarter);
}

} // namespace

extern const NoteEffectEngine random_engine{&descriptor, create, tail};

} // namespace oscillate::native_dsp::note_effects
