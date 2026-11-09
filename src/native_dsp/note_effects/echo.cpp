// Echo (ADR-0059): each note repeats at a synced delay, quieter each time
// and optionally moved in pitch. A repeat lasts as long as the played note:
// it starts k delays after the note and ends k delays after its release.
#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::echo;
using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

constexpr auto time_choice = tables::parameter_index(table::parameters, "echo.time");
constexpr auto repeats = tables::parameter_index(table::parameters, "echo.repeats");
constexpr auto decay = tables::parameter_index(table::parameters, "echo.decay");
constexpr auto pitch = tables::parameter_index(table::parameters, "echo.pitch");
constexpr std::size_t max_repeats = 16;
constexpr std::uint8_t silent = 255;

[[nodiscard]] std::int64_t delay_ticks(int division) noexcept {
  return tables::division_ticks[static_cast<std::size_t>(
      std::clamp(division, 0, static_cast<int>(tables::divisions.size()) - 1))];
}

// What became of one played note: its repeats' keys (silent when dropped)
// and ids, and where they were scheduled from.
struct Echoed {
  std::int64_t on_tick = 0;
  std::int64_t delay = 0;
  std::uint64_t first_note = 0;
  std::array<std::uint8_t, max_repeats> keys{};
  std::size_t count = 0;
};

class EchoKernel final : public NoteEffectKernel {
public:
  explicit EchoKernel(IdBits clip) : clip_(clip) {}

  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    support::Emitter out(output);
    for (const auto& event : input) {
      out.pass(event);
      const auto tick = clock.tick_at(event.offset);
      if (event.kind == MidiEventKind::note_off) {
        if (const auto* echoed = notes_.find(event.clip_id_bits, event.note_id_bits)) {
          // A zero-length note's repeats still end after they start.
          const auto release = std::max(tick, echoed->on_tick + 1);
          for (std::size_t k = 0; k < echoed->count; ++k) {
            if (echoed->keys[k] == silent)
              continue;
            const auto at = release + static_cast<std::int64_t>(k + 1) * echoed->delay;
            (void)pending_.push({at, MidiEventKind::note_off, clip_,
                                 IdBits{0, echoed->first_note + k}, echoed->keys[k], 0});
          }
          notes_.erase(event.clip_id_bits, event.note_id_bits);
        }
        continue;
      }
      const auto at = event.offset;
      Echoed echoed;
      echoed.on_tick = tick;
      echoed.delay = delay_ticks(parameters.index_at(time_choice, at));
      echoed.count = static_cast<std::size_t>(
          std::clamp(parameters.index_at(repeats, at), 1, static_cast<int>(max_repeats)));
      echoed.first_note = serial_ + 1;
      serial_ += echoed.count;
      const double fall = std::clamp(parameters.at(decay, at), 0.0, 1.0);
      const int step = parameters.index_at(pitch, at);
      double velocity = event.velocity;
      for (std::size_t k = 0; k < echoed.count; ++k) {
        velocity *= fall;
        const int key = event.key + static_cast<int>(k + 1) * step;
        const auto level = static_cast<int>(std::lround(velocity));
        if (key < 0 || key > 127 || level < 1) {
          echoed.keys[k] = silent;
          continue;
        }
        echoed.keys[k] = static_cast<std::uint8_t>(key);
        const auto start = tick + static_cast<std::int64_t>(k + 1) * echoed.delay;
        // Starts use a bounded part of the queue, so the note-off of every
        // repeat of every remembered note always has room.
        if (pending_.size() >= starts_room ||
            !pending_.push({start, MidiEventKind::note_on, clip_, IdBits{0, echoed.first_note + k},
                            echoed.keys[k], static_cast<std::uint8_t>(std::min(level, 127))}))
          echoed.keys[k] = silent;
      }
      if (notes_.insert(event.clip_id_bits, event.note_id_bits, echoed) == nullptr) {
        // No room to remember the note: its repeats end with their delay.
        for (std::size_t k = 0; k < echoed.count; ++k) {
          if (echoed.keys[k] != silent)
            (void)pending_.push({tick + static_cast<std::int64_t>(k + 2) * echoed.delay,
                                 MidiEventKind::note_off, clip_, IdBits{0, echoed.first_note + k},
                                 echoed.keys[k], 0});
        }
      }
    }
    pending_.fire(clock, out);
    return out.finish();
  }

  void reset() noexcept override {
    notes_.clear();
    pending_.clear();
    serial_ = 0;
  }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    // Repeats of notes still held may be sounding; offs for any not yet
    // started are harmless.
    for (auto& entry : notes_.entries()) {
      for (std::size_t k = 0; k < entry.value.count; ++k) {
        if (entry.value.keys[k] != silent)
          out.note_off(0, clip_, IdBits{0, entry.value.first_note + k}, entry.value.keys[k]);
      }
    }
    pending_.flush_offs(out);
    notes_.clear();
    return out.finish();
  }

private:
  IdBits clip_;
  std::uint64_t serial_ = 0;
  support::NoteMap<Echoed, 128> notes_;
  static constexpr std::size_t starts_room = 1024;
  support::Pending<starts_room + 128 * max_repeats * 2> pending_;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 1> dotted{{{"echo.time", tables::dotted_eighth}}};
constexpr std::array<V, 2> sixteenths{{{"echo.time", tables::sixteenth}, {"echo.repeats", 6}}};
constexpr std::array<V, 2> triplets{{{"echo.time", 4}, {"echo.repeats", 5}}};
constexpr std::array<V, 2> rising{{{"echo.pitch", 7}, {"echo.repeats", 4}}};
constexpr std::array<V, 2> octave_fall{{{"echo.pitch", -12}, {"echo.repeats", 2}}};
constexpr std::array<V, 3> long_tail{
    {{"echo.repeats", 10}, {"echo.decay", 0.85}, {"echo.time", 9}}};
constexpr std::array<V, 3> stutter{{{"echo.time", 1}, {"echo.repeats", 8}, {"echo.decay", 0.9}}};

constexpr std::array<Preset, 8> presets{{
    {"Eighth Echo", "Echo", {}},
    {"Dotted Eighth", "Echo", dotted},
    {"Sixteenth Trail", "Echo", sixteenths},
    {"Triplet Bounce", "Echo", triplets},
    {"Long Tail", "Echo", long_tail},
    {"Stutter", "Echo", stutter},
    {"Rising Fifths", "Pitch", rising},
    {"Octave Fall", "Pitch", octave_fall},
}};

constexpr EngineDescriptor descriptor{"echo", "Note Echo", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(IdBits generated_clip) {
  return std::make_unique<EchoKernel>(generated_clip);
}

double tail(std::span<const double> values) {
  if (values.size() != table::parameters.size())
    return 0.0;
  const auto ticks = delay_ticks(static_cast<int>(std::lround(values[time_choice])));
  return static_cast<double>(ticks) / static_cast<double>(tables::quarter) *
         std::clamp(std::round(values[repeats]), 1.0, static_cast<double>(max_repeats));
}

} // namespace

extern const NoteEffectEngine echo_engine{&descriptor, create, tail};

} // namespace oscillate::native_dsp::note_effects
