// Scale (ADR-0059): every note snaps into a scale, then moves by scale
// steps and octaves. A note keeps its ids, so its note-off finds it.
#include "kernel_support.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::note_effects {
namespace {

namespace table = tables::scale;
using engine::BlockMidiEvent;
using engine::MidiEventKind;

constexpr auto root = tables::parameter_index(table::parameters, "scale.root");
constexpr auto scale_choice = tables::parameter_index(table::parameters, "scale.scale");
constexpr auto snap = tables::parameter_index(table::parameters, "scale.snap");
constexpr auto transpose = tables::parameter_index(table::parameters, "scale.transpose");
constexpr auto octave = tables::parameter_index(table::parameters, "scale.octave");

class ScaleKernel final : public NoteEffectKernel {
public:
  std::size_t process(std::span<const BlockMidiEvent> input, std::span<BlockMidiEvent> output,
                      const NoteClock& clock, const NoteParameters& parameters) noexcept override {
    (void)clock;
    support::Emitter out(output);
    for (const auto& event : input) {
      if (event.kind == MidiEventKind::note_off) {
        if (const auto* key = mapped_.find(event.clip_id_bits, event.note_id_bits)) {
          out.note_off(event.offset, event.clip_id_bits, event.note_id_bits, *key,
                       event.boundary_crop);
          mapped_.erase(event.clip_id_bits, event.note_id_bits);
        } else {
          out.pass(event);
        }
        continue;
      }
      const auto at = event.offset;
      const auto scale = support::Scale::from(parameters.index_at(root, at),
                                              parameters.index_at(scale_choice, at));
      int key = scale.snap(event.key, parameters.index_at(snap, at));
      key = scale.move(key, parameters.index_at(transpose, at));
      key += 12 * parameters.index_at(octave, at);
      if (key < 0 || key > 127)
        continue; // Moved off the keyboard: silent, and its note-off passes.
      if (out.note_on(at, event.clip_id_bits, event.note_id_bits, key, event.velocity))
        (void)mapped_.insert(event.clip_id_bits, event.note_id_bits,
                             static_cast<std::uint8_t>(key));
    }
    return out.finish();
  }

  void reset() noexcept override { mapped_.clear(); }

  std::size_t flush(std::span<BlockMidiEvent> output) noexcept override {
    support::Emitter out(output);
    for (const auto& entry : mapped_.entries())
      out.note_off(0, entry.clip, entry.note, entry.value);
    mapped_.clear();
    return out.finish();
  }

private:
  support::NoteMap<std::uint8_t, 256> mapped_;
};

using namespace std::string_view_literals;
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 1> c_minor{{{"scale.scale", tables::minor_scale}}};
constexpr std::array<V, 2> a_minor{{{"scale.root", 9}, {"scale.scale", tables::minor_scale}}};
constexpr std::array<V, 1> pentatonic{{{"scale.scale", 9}}};
constexpr std::array<V, 1> minor_pentatonic{{{"scale.scale", 10}}};
constexpr std::array<V, 1> blues{{{"scale.scale", 11}}};
constexpr std::array<V, 1> dorian{{{"scale.scale", 2}}};
constexpr std::array<V, 1> third_up{{{"scale.transpose", 2}}};
constexpr std::array<V, 1> octave_down{{{"scale.octave", -1}}};

constexpr std::array<synth::SynthPreset, 9> presets{{
    {"C Major", "Keys", {}},
    {"C Minor", "Keys", c_minor},
    {"A Minor", "Keys", a_minor},
    {"Major Pentatonic", "Keys", pentatonic},
    {"Minor Pentatonic", "Keys", minor_pentatonic},
    {"Blues", "Keys", blues},
    {"Dorian", "Modes", dorian},
    {"Up a Third", "Move", third_up},
    {"Octave Down", "Move", octave_down},
}};

constexpr EngineDescriptor descriptor{"scale", "Scale", 1, table::parameters, presets};

std::unique_ptr<NoteEffectKernel> create(foundation::IdBits generated_clip) {
  (void)generated_clip;
  return std::make_unique<ScaleKernel>();
}

double no_tail(std::span<const double> values) {
  (void)values;
  return 0.0;
}

} // namespace

extern const NoteEffectEngine scale_engine{&descriptor, create, no_tail};

} // namespace oscillate::native_dsp::note_effects
