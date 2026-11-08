// Chord (ADR-0059): each note plays a chord, from fixed intervals or from
// the scale degrees of a key. The played note sounds under its own ids; the
// added tones under the stage's generated ids, released with it.
#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::chord;
using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

constexpr auto mode = tables::parameter_index(table::parameters, "chord.mode");
constexpr auto shift1 = tables::parameter_index(table::parameters, "chord.shift1");
constexpr auto shape = tables::parameter_index(table::parameters, "chord.shape");
constexpr auto inversion = tables::parameter_index(table::parameters, "chord.inversion");
constexpr auto root = tables::parameter_index(table::parameters, "chord.root");
constexpr auto scale_choice = tables::parameter_index(table::parameters, "chord.scale");
constexpr auto added_velocity = tables::parameter_index(table::parameters, "chord.velocity");
constexpr std::size_t shifts = 6;
static_assert(tables::parameter_index(table::parameters, "chord.shift6") == shift1 + shifts - 1);

// Scale degrees above the root, per In Key shape.
constexpr std::array<std::array<int, 4>, table::shapes.size()> shape_degrees{{
    {2, 4, 0, 0}, // Triad
    {2, 4, 6, 0}, // Seventh
    {1, 4, 0, 0}, // Sus2
    {3, 4, 0, 0}, // Sus4
    {2, 4, 5, 0}, // Sixth
    {2, 4, 8, 0}, // Add9
    {2, 4, 6, 8}, // Ninth
}};

constexpr std::size_t max_tones = 1 + shifts;

struct Voicing {
  std::array<std::uint8_t, max_tones> keys{};
  std::array<IdBits, max_tones> clips{};
  std::array<IdBits, max_tones> notes{};
  std::size_t count = 0;
};

class ChordKernel final : public NoteEffectKernel {
public:
  explicit ChordKernel(IdBits clip) : clip_(clip) {}

  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    (void)clock;
    support::Emitter out(output);
    for (const auto& event : input) {
      if (event.kind == MidiEventKind::note_off) {
        if (const auto* voicing = chords_.find(event.clip_id_bits, event.note_id_bits)) {
          for (std::size_t i = 0; i < voicing->count; ++i)
            out.note_off(event.offset, voicing->clips[i], voicing->notes[i], voicing->keys[i],
                         event.boundary_crop);
          chords_.erase(event.clip_id_bits, event.note_id_bits);
        } else {
          out.pass(event);
        }
        continue;
      }
      play(event, parameters, out);
    }
    return out.finish();
  }

  void reset() noexcept override {
    chords_.clear();
    ids_ = {};
  }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    for (auto& entry : chords_.entries()) {
      for (std::size_t i = 0; i < entry.value.count; ++i)
        out.note_off(0, entry.value.clips[i], entry.value.notes[i], entry.value.keys[i]);
    }
    chords_.clear();
    return out.finish();
  }

private:
  void play(const BlockMidiEvent& event, const NoteParameters& parameters,
            support::Emitter& out) noexcept {
    const auto at = event.offset;
    // The chord's tones, root first.
    std::array<int, max_tones> tones{};
    std::size_t count = 0;
    tones[count++] = event.key;
    if (parameters.index_at(mode, at) == 0) {
      for (std::size_t i = 0; i < shifts; ++i) {
        if (const int shift = parameters.index_at(shift1 + i, at); shift != 0)
          tones[count++] = event.key + shift;
      }
    } else {
      // In Key: degrees above the scale tone at or below the played key,
      // moved so the played key keeps its pitch.
      const auto scale = support::Scale::from(parameters.index_at(root, at),
                                              parameters.index_at(scale_choice, at));
      const int base = scale.snap(event.key, tables::scale::down);
      const auto shape_index = static_cast<std::size_t>(std::clamp(
          parameters.index_at(shape, at), 0, static_cast<int>(table::shapes.size()) - 1));
      for (const int degree : shape_degrees[shape_index]) {
        if (degree != 0)
          tones[count++] = scale.move(base, degree) + (event.key - base);
      }
      // Lift the lowest tones an octave per inversion step.
      std::sort(tones.begin(), tones.begin() + static_cast<std::ptrdiff_t>(count));
      const int lifts =
          std::clamp(parameters.index_at(inversion, at), 0, static_cast<int>(count) - 1);
      for (int i = 0; i < lifts; ++i)
        tones[static_cast<std::size_t>(i)] += 12;
    }
    const double scale_velocity = std::clamp(parameters.at(added_velocity, at), 0.0, 1.0);
    Voicing voicing;
    bool root_sounded = false;
    for (std::size_t i = 0; i < count; ++i) {
      const int key = tones[i];
      if (key < 0 || key > 127 ||
          std::find(voicing.keys.begin(),
                    voicing.keys.begin() + static_cast<std::ptrdiff_t>(voicing.count),
                    static_cast<std::uint8_t>(key)) !=
              voicing.keys.begin() + static_cast<std::ptrdiff_t>(voicing.count))
        continue;
      // The played note keeps its ids and velocity; added tones are scaled.
      const bool is_root = key == event.key && !root_sounded;
      const int velocity =
          is_root
              ? event.velocity
              : static_cast<int>(std::lround(static_cast<double>(event.velocity) * scale_velocity));
      if (velocity < 1)
        continue;
      const IdBits clip = is_root ? event.clip_id_bits : clip_;
      const IdBits note = is_root ? event.note_id_bits : ids_.next();
      if (!out.note_on(event.offset, clip, note, key, velocity))
        continue;
      root_sounded = root_sounded || is_root;
      voicing.keys[voicing.count] = static_cast<std::uint8_t>(key);
      voicing.clips[voicing.count] = clip;
      voicing.notes[voicing.count] = note;
      ++voicing.count;
    }
    if (voicing.count > 0 &&
        chords_.insert(event.clip_id_bits, event.note_id_bits, voicing) == nullptr) {
      // No room to remember the chord: release it at once rather than
      // leave it hanging.
      for (std::size_t i = 0; i < voicing.count; ++i)
        out.note_off(event.offset, voicing.clips[i], voicing.notes[i], voicing.keys[i]);
    }
  }

  IdBits clip_;
  support::NoteIds ids_;
  support::NoteMap<Voicing, 128> chords_;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 1> minor_triad{{{"chord.shift1", 3}}};
constexpr std::array<V, 2> power{{{"chord.shift1", 7}, {"chord.shift2", 12}}};
constexpr std::array<V, 1> seventh{{{"chord.shift3", 10}}};
constexpr std::array<V, 2> minor_seventh{{{"chord.shift1", 3}, {"chord.shift3", 10}}};
constexpr std::array<V, 3> minor_ninth{
    {{"chord.shift1", 3}, {"chord.shift3", 10}, {"chord.shift4", 14}}};
constexpr std::array<V, 2> octaves{{{"chord.shift1", 12}, {"chord.shift2", -12}}};
constexpr std::array<V, 2> stacked_fifths{{{"chord.shift1", 7}, {"chord.shift2", 14}}};
constexpr std::array<V, 1> in_key{{{"chord.mode", 1}}};
constexpr std::array<V, 2> in_key_sevenths{{{"chord.mode", 1}, {"chord.shape", 1}}};
constexpr std::array<V, 3> in_key_minor_ninths{
    {{"chord.mode", 1}, {"chord.shape", 6}, {"chord.scale", tables::minor_scale}}};
constexpr std::array<V, 2> soft_stack{{{"chord.shift3", 12}, {"chord.velocity", 0.6}}};

constexpr std::array<Preset, 12> presets{{
    {"Major", "Intervals", {}},
    {"Minor", "Intervals", minor_triad},
    {"Power", "Intervals", power},
    {"Dominant Seventh", "Intervals", seventh},
    {"Minor Seventh", "Intervals", minor_seventh},
    {"Minor Ninth", "Intervals", minor_ninth},
    {"Octaves", "Intervals", octaves},
    {"Stacked Fifths", "Intervals", stacked_fifths},
    {"Soft Stack", "Intervals", soft_stack},
    {"Triads in Key", "In Key", in_key},
    {"Sevenths in Key", "In Key", in_key_sevenths},
    {"Minor Ninths in Key", "In Key", in_key_minor_ninths},
}};

constexpr EngineDescriptor descriptor{"chord", "Chord", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(IdBits generated_clip) {
  return std::make_unique<ChordKernel>(generated_clip);
}

double no_tail(std::span<const double> values) {
  (void)values;
  return 0.0;
}

} // namespace

extern const NoteEffectEngine chord_engine{&descriptor, create, no_tail};

} // namespace oscillate::native_dsp::note_effects
