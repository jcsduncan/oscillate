#include "document.hpp"

#include "oscillate/warp_dsp/bandlimited.hpp"
#include "table.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::native_dsp::synth::crate {
namespace {

// The starter one-shots, ported line for line from
// scripts/generate-starter-sounds.py (generator revision 1), so a preset
// plays exactly what assets/starter bundles: the same synthesis in double
// precision, normalised to the same peak and rounded to the same 16-bit PCM.
constexpr double starter_rate = builtin_sample_rate;
constexpr double pcm_scale = 32'767.0;
constexpr double pcm_decode = 32'768.0;
constexpr double pcm_limit = 32'767.0;
constexpr int default_fade_frames = 480;

struct StarterNoise {
  std::uint32_t state;
  double next() noexcept {
    state = 1'664'525U * state + 1'013'904'223U; // Wraps modulo 2^32, as the generator masks.
    return static_cast<double>((state >> 8U) & 0xFF'FFFFU) / static_cast<double>(0x7F'FFFF) - 1.0;
  }
};

double fade_end(double value, int index, int frames, int fade_frames = default_fade_frames) {
  return value * std::min(1.0, static_cast<double>(frames - 1 - index) / fade_frames);
}

std::vector<double> starter_kick(int frames, std::uint32_t seed) {
  StarterNoise noise{seed};
  double phase = 0.0;
  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(frames));
  for (int index = 0; index < frames; ++index) {
    const double t = index / starter_rate;
    const double frequency = 47.0 + 153.0 * std::exp(-31.0 * t);
    phase += 2.0 * std::numbers::pi * frequency / starter_rate;
    const double body = std::sin(phase) * std::exp(-10.0 * t);
    const double click = noise.next() * std::exp(-1'800.0 * t) * 0.12;
    out.push_back(fade_end((body + click) * std::min(1.0, t * 2'000.0), index, frames));
  }
  return out;
}

std::vector<double> starter_snare(int frames, std::uint32_t seed) {
  StarterNoise noise{seed};
  double previous = 0.0;
  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(frames));
  for (int index = 0; index < frames; ++index) {
    const double t = index / starter_rate;
    const double current = noise.next();
    const double bright = current - previous * 0.82;
    previous = current;
    const double body = 0.27 * std::sin(2.0 * std::numbers::pi * 185.0 * t);
    out.push_back(fade_end((0.78 * bright + body) * std::exp(-17.0 * t), index, frames));
  }
  return out;
}

std::vector<double> starter_hat(int frames, std::uint32_t seed) {
  StarterNoise noise{seed};
  double previous = 0.0;
  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(frames));
  for (int index = 0; index < frames; ++index) {
    const double t = index / starter_rate;
    const double current = noise.next();
    const double bright = current - previous;
    previous = current;
    out.push_back(fade_end(bright * std::exp(-36.0 * t), index, frames, 240));
  }
  return out;
}

std::vector<double> starter_pluck(int frames, std::uint32_t seed) {
  StarterNoise noise{seed};
  const auto length = static_cast<std::size_t>(std::nearbyint(starter_rate / 220.0));
  std::vector<double> delay(length);
  for (auto& value : delay)
    value = noise.next();
  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(frames));
  for (int index = 0; index < frames; ++index) {
    const std::size_t position = static_cast<std::size_t>(index) % length;
    const std::size_t next_position = (position + 1) % length;
    const double current = delay[position];
    delay[position] = 0.9975 * (current + delay[next_position]) / 2.0;
    out.push_back(fade_end(current, index, frames, 960));
  }
  return out;
}

struct Starter {
  std::string_view name;
  int duration_ms;
  double target_peak;
  std::uint32_t seed;
  std::vector<double> (*synth)(int, std::uint32_t);
};
constexpr std::array<Starter, 4> starters{{
    {"kick", 450, 0.65, 0x1051, &starter_kick},
    {"snare", 280, 0.58, 0x2052, &starter_snare},
    {"hat", 140, 0.42, 0x3053, &starter_hat},
    {"pluck", 700, 0.62, 0x4054, &starter_pluck},
}};
static_assert(starters.size() == builtin_samples.size());

std::vector<float> decoded_starter(const Starter& starter) {
  const int frames = static_cast<int>(builtin_sample_rate) * starter.duration_ms / 1'000;
  const auto samples = starter.synth(frames, starter.seed);
  double raw_peak = 0.0;
  for (const double sample : samples)
    raw_peak = std::max(raw_peak, std::fabs(sample));
  const double scale = starter.target_peak / raw_peak;
  std::vector<float> out;
  out.reserve(samples.size());
  for (const double sample : samples) {
    // Python's round() is round-half-to-even, as nearbyint in the default mode.
    const double pcm =
        std::clamp(std::nearbyint(sample * scale * pcm_scale), -pcm_decode, pcm_limit);
    out.push_back(static_cast<float>(pcm / pcm_decode));
  }
  return out;
}

} // namespace

std::span<const float> builtin_sample(std::string_view name) {
  static const auto decoded = [] {
    std::array<std::vector<float>, starters.size()> made;
    for (std::size_t i = 0; i < starters.size(); ++i)
      made[i] = decoded_starter(starters[i]);
    return made;
  }();
  for (std::size_t i = 0; i < starters.size(); ++i) {
    if (starters[i].name == name)
      return decoded[i];
  }
  return {};
}

namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view builtin_prefix = "builtin:";
// Beyond the band-limited reach, the era reads touch one more frame.
constexpr std::int64_t read_guard_frames = 2;

[[nodiscard]] std::int64_t padding_frames() {
  const auto kernel = warp_dsp::BandlimitedKernel::for_tier(warp_dsp::QualityTier::q0);
  return warp_dsp::bandlimited_reach(*kernel, max_read_speed) + read_guard_frames;
}

// The media slot's frame count as the document declares it, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_frames(const project::NativeMediaSlot& slot) {
  if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media)) {
    const auto frames = builtin_sample(builtin->name).size();
    return frames == 0 ? std::nullopt : std::optional<std::uint64_t>(frames);
  }
  return std::get<project::ManagedAudioReference>(slot.media).frames;
}

[[nodiscard]] std::string key_of(const project::NativeMediaSlot& slot) {
  if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media))
    return std::string(builtin_prefix) + builtin->name;
  return std::get<project::ManagedAudioReference>(slot.media).sha256;
}

[[nodiscard]] std::shared_ptr<const SampleData>
planar(std::uint32_t source_rate, std::size_t frames, auto&& left_of, auto&& right_of) {
  auto data = std::make_shared<SampleData>();
  data->source_rate = source_rate;
  data->frames = static_cast<std::int64_t>(frames);
  data->padding = padding_frames();
  const auto total = frames + 2 * static_cast<std::size_t>(data->padding);
  data->left.assign(total, 0.0F);
  data->right.assign(total, 0.0F);
  const auto pad = static_cast<std::size_t>(data->padding);
  for (std::size_t i = 0; i < frames; ++i) {
    data->left[pad + i] = left_of(i);
    data->right[pad + i] = right_of(i);
  }
  return data;
}

} // namespace

std::optional<double> parse_bpm(std::string_view text) noexcept {
  double value = 0.0;
  const auto* end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || last != end || !std::isfinite(value) || value < min_sample_bpm ||
      value > max_sample_bpm)
    return std::nullopt;
  return value;
}

Result<void> validate_document(const project::NativeSynthDocument& document) {
  using R = Result<void>;
  const auto invalid = [](std::string message) {
    return R::failure({ErrorCode::invalid_argument, "Crate: " + std::move(message)});
  };
  if (!document.routes.empty() || !document.curves.empty() || !document.chains.empty() ||
      !document.sequences.empty() || !document.voice_lanes.empty())
    return invalid("the document holds only the sample, its chops and its tempo");
  if (document.media.size() > 1)
    return invalid("only the sample slot holds media");
  std::optional<std::uint64_t> frames;
  for (const auto& slot : document.media) {
    if (slot.id != sample_slot || slot.frame_length != 0)
      return invalid("the sample slot holds one sample, not frames");
    frames = declared_frames(slot);
    if (!frames || *frames == 0)
      return invalid("unknown or empty sample");
  }
  bool seen_bpm = false;
  bool seen_name = false;
  for (const auto& label : document.labels) {
    if (label.id == bpm_label && !seen_bpm) {
      if (!parse_bpm(label.text))
        return invalid("the sample tempo must be a number of BPM in range");
      seen_bpm = true;
    } else if (label.id == sample_name_label && !seen_name) {
      if (label.text.empty())
        return invalid("the sample's name is empty");
      seen_name = true;
    } else {
      return invalid("unknown or repeated label '" + label.id + "'");
    }
  }
  if (document.regions.size() > max_chops)
    return invalid("at most 32 chops");
  std::array<bool, max_chops> used{};
  std::vector<std::uint64_t> starts;
  starts.reserve(document.regions.size());
  for (const auto& region : document.regions) {
    if (region.slot >= max_chops || used[region.slot])
      return invalid("chop slots are 0..31 and unique");
    used[region.slot] = true;
    if (region.media != sample_slot || !frames)
      return invalid("chops cut the sample slot");
    if (region.start_frame >= *frames)
      return invalid("a chop starts beyond the sample's end");
    if (std::ranges::find(tags, region.tag) == tags.end())
      return invalid("a chop's tag is kick, snare, hat or other");
    starts.push_back(region.start_frame);
  }
  std::ranges::sort(starts);
  if (std::ranges::adjacent_find(starts) != starts.end())
    return invalid("two chops start at the same frame");
  return R::success();
}

Result<std::shared_ptr<const SynthDocumentState>>
prepare_document(const project::NativeSynthDocument& document, const SynthMedia& media,
                 double sample_rate, const SynthDocumentState* previous) {
  using R = Result<std::shared_ptr<const SynthDocumentState>>;
  (void)sample_rate; // Voices read at the source rate; nothing is resampled here.
  if (auto valid = validate_document(document); !valid)
    return R::failure(valid.error());
  auto state = std::make_shared<CrateState>();
  state->chop_of_slot.fill(no_chop);
  for (const auto& label : document.labels) {
    if (label.id == bpm_label)
      state->sample_bpm = parse_bpm(label.text).value_or(0.0);
  }
  if (document.media.empty())
    return R::success(std::shared_ptr<const SynthDocumentState>(std::move(state)));

  const auto& slot = document.media.front();
  state->sample_key = key_of(slot);
  const auto* before = dynamic_cast<const CrateState*>(previous);
  if (before != nullptr && before->sample && before->sample_key == state->sample_key) {
    state->sample = before->sample; // Unchanged media keep their resident copy.
  } else if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media)) {
    const auto mono = builtin_sample(builtin->name);
    state->sample = planar(
        builtin_sample_rate, mono.size(), [&](std::size_t i) { return mono[i]; },
        [&](std::size_t i) { return mono[i]; });
  } else {
    const auto found = std::ranges::find(media, std::string(sample_slot), &SynthMediaEntry::slot);
    if (found == media.end() || !found->asset || found->asset->source_rate == 0 ||
        found->asset->frames.empty())
      return R::failure({ErrorCode::unavailable, "Crate: the sample is not loaded"});
    const auto& frames = found->asset->frames;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames.size(), project::max_native_synth_media_frames));
    state->sample = planar(
        found->asset->source_rate, count, [&](std::size_t i) { return frames[i].left; },
        [&](std::size_t i) { return frames[i].right; });
  }

  const auto length = state->sample->frames;
  std::vector<project::NativeRegion> regions = document.regions;
  // A loaded sample that is not cut yet plays whole, as slot 0's chop.
  if (regions.empty())
    regions.push_back({0, std::string(sample_slot), 0, std::string(tags.back()), {}});
  std::ranges::sort(regions, {}, &project::NativeRegion::start_frame);
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto start = static_cast<std::int64_t>(regions[i].start_frame);
    if (start >= length)
      return R::failure({ErrorCode::invalid_argument, "Crate: a chop starts beyond the sample"});
    const auto end = i + 1 < regions.size()
                         ? std::min(length, static_cast<std::int64_t>(regions[i + 1].start_frame))
                         : length;
    state->chop_of_slot[regions[i].slot] = static_cast<std::int8_t>(state->chops.size());
    state->chops.push_back({regions[i].slot, start, end});
  }
  return R::success(std::shared_ptr<const SynthDocumentState>(std::move(state)));
}

project::NativeSynthDocument preset_document(std::optional<std::size_t> preset) {
  project::NativeSynthDocument document;
  const auto starter = preset_sample(preset.value_or(default_document_preset));
  if (!starter)
    return document;
  document.media.push_back(
      {std::string(sample_slot), project::NativeBuiltinMedia{std::string(starter->sample)}, 0});
  document.regions.push_back(
      {0, std::string(sample_slot), 0, std::string(starter->tag), std::string(starter->name)});
  return document;
}

} // namespace detail
} // namespace oscillate::native_dsp::synth::crate
