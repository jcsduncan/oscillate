#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Crate's compiled document: the sample resident as padded planar floats at
// its own rate, and its chops in time order. Immutable once built.
namespace oscillate::native_dsp::synth::crate::detail {

// The fastest any voice reads its source, in source frames per output
// frame; faster reads are clamped. Two octaves above +48 st would alias more
// than the band-limited read is sized for, so the padding is bounded too.
inline constexpr double max_read_speed = 32.0;

// The resident sample: `padding` zero frames on each side of `frames`, so
// every band-limited read up to max_read_speed stays inside the buffers.
struct SampleData {
  std::uint32_t source_rate = 0;
  std::int64_t frames = 0;
  std::int64_t padding = 0;
  std::vector<float> left;
  std::vector<float> right;
  [[nodiscard]] const float* left_at(std::int64_t frame) const noexcept {
    return left.data() + (padding + frame);
  }
  [[nodiscard]] const float* right_at(std::int64_t frame) const noexcept {
    return right.data() + (padding + frame);
  }
};

// One chop, [start, end) in source frames; pad i is chops[i].
struct Chop {
  std::uint32_t slot = 0;
  std::int64_t start = 0;
  std::int64_t end = 0;
};

inline constexpr std::int8_t no_chop = -1;

struct CrateState final : SynthDocumentState {
  std::shared_ptr<const SampleData> sample;          // Null: nothing plays.
  std::string sample_key;                            // "builtin:<name>" or the managed sha256.
  std::vector<Chop> chops;                           // Time order.
  std::array<std::int8_t, max_chops> chop_of_slot{}; // Index into chops, or no_chop.
  double sample_bpm = 0.0;                           // 0: unknown.
};

[[nodiscard]] foundation::Result<void>
validate_document(const project::NativeSynthDocument& document);

[[nodiscard]] foundation::Result<std::shared_ptr<const SynthDocumentState>>
prepare_document(const project::NativeSynthDocument& document, const SynthMedia& media,
                 double sample_rate, const SynthDocumentState* previous);

[[nodiscard]] project::NativeSynthDocument preset_document(std::optional<std::size_t> preset);

// The sample's tempo from the document's label, or nullopt when absent;
// a present label must parse into [min_sample_bpm, max_sample_bpm].
[[nodiscard]] std::optional<double> parse_bpm(std::string_view text) noexcept;

} // namespace oscillate::native_dsp::synth::crate::detail
