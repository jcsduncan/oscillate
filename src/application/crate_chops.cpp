#include "oscillate/application/crate_chops.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <numbers>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace oscillate::application::crate {
namespace {

using foundation::ErrorCode;
using StartsResult = foundation::Result<ChopStarts>;

// Sensitivity 0..1 maps to the detector's threshold scale 2^(octaves * (1 - 2s)):
// 0 is four times the default threshold, 0.5 the default, 1 a quarter of it.
constexpr double sensitivity_scale_octaves = 2.0;

// The analysis rates the warp analyses accept.
constexpr std::uint32_t minimum_analysis_rate = 8'000;
constexpr std::uint32_t maximum_analysis_rate = 384'000;

constexpr double seconds_per_minute = 60.0;

// The sample's tempo label is written in hundredths of a BPM; its longest
// text is "999.99".
constexpr double bpm_label_steps_per_beat = 100.0;
constexpr std::size_t bpm_text_capacity = 32;
constexpr double quarters_per_bar = 4.0;

// Classification bands (spec 09 §3) and window.
constexpr double low_band_hz = 150.0;
constexpr double mid_band_top_hz = 2'000.0;
constexpr double high_band_hz = 6'000.0;
constexpr double butterworth_q = std::numbers::sqrt2 / 2.0;
// The hat band needs headroom below Nyquist.
constexpr std::uint32_t minimum_classify_rate = 16'000;
// Only the chop's first second decides its tag.
constexpr double classify_window_seconds = 1.0;
constexpr double envelope_block_seconds = 0.005;
// -20 dB in amplitude.
constexpr double decay_floor_ratio = 0.1;
constexpr double silence_energy = 1e-12;
// Hat: most energy above 6 kHz.
constexpr double hat_minimum_high_share = 0.5;
// Kick: most energy below 150 Hz, gone within half a second.
constexpr double kick_minimum_low_share = 0.5;
constexpr double drum_maximum_decay_seconds = 0.5;
// Snare: a mid-band body plus noise above 2 kHz.
constexpr double snare_minimum_noise_share = 0.2;
constexpr double snare_minimum_mid_share = 0.1;

// Root detection.
constexpr float root_minimum_hz = 40.0F;
constexpr float root_maximum_hz = 2'000.0F;
constexpr double root_maximum_rate_share = 0.25; // track_pitch's limit
constexpr std::size_t minimum_voiced_frames = 3;
constexpr double a4_hz = 440.0;
constexpr double a4_key = 69.0;

constexpr std::array<std::string_view, 4> tag_ids{"kick", "snare", "hat", "other"};
constexpr std::array<std::string_view, 4> tag_names{"Kick", "Snare", "Hat", "Chop"};

struct Candidate {
  std::uint64_t frame = 0;
  double strength = 0.0;
};

std::uint64_t merge_frames(const SampleAudio& audio) {
  return std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(
             std::llround(chop_merge_seconds * static_cast<double>(audio.source_rate))));
}

foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

bool usable(const SampleAudio& audio) {
  return !audio.frames.empty() && audio.source_rate >= minimum_analysis_rate &&
         audio.source_rate <= maximum_analysis_rate;
}

// Drops starts outside the sample, adds frame 0, merges starts closer than
// chop_merge_seconds into the earlier one (keeping the stronger strength) and
// keeps frame 0 plus the strongest others up to max_crate_chops.
ChopStarts normalise(std::vector<Candidate> candidates, const SampleAudio& audio) {
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  std::erase_if(candidates, [frames](const Candidate& entry) { return entry.frame >= frames; });
  std::ranges::stable_sort(candidates, {}, &Candidate::frame);
  const auto gap = merge_frames(audio);
  std::vector<Candidate> merged{{0, std::numeric_limits<double>::infinity()}};
  for (const auto& entry : candidates) {
    if (entry.frame - merged.back().frame < gap) {
      merged.back().strength = std::max(merged.back().strength, entry.strength);
      continue;
    }
    merged.push_back(entry);
  }
  if (merged.size() > max_crate_chops) {
    std::ranges::stable_sort(merged, [](const Candidate& left, const Candidate& right) {
      return left.strength > right.strength;
    });
    merged.resize(max_crate_chops);
    std::ranges::sort(merged, {}, &Candidate::frame);
  }
  ChopStarts result;
  result.reserve(merged.size());
  for (const auto& entry : merged)
    result.push_back(entry.frame);
  return result;
}

// Earlier positions win the cap (grid and equal chops).
Candidate ordered(std::uint64_t frame) { return {frame, -static_cast<double>(frame)}; }

// RBJ cookbook biquad, transposed direct form II.
class Biquad {
public:
  static Biquad lowpass(double rate, double hz) { return make(rate, hz, false); }
  static Biquad highpass(double rate, double hz) { return make(rate, hz, true); }
  double process(double input) noexcept {
    const double output = b0_ * input + z1_;
    z1_ = b1_ * input - a1_ * output + z2_;
    z2_ = b2_ * input - a2_ * output;
    return output;
  }

private:
  static Biquad make(double rate, double hz, bool high) {
    const double omega = 2.0 * std::numbers::pi * hz / rate;
    const double alpha = std::sin(omega) / (2.0 * butterworth_q);
    const double cosine = std::cos(omega);
    const double a0 = 1.0 + alpha;
    Biquad filter;
    const double edge = high ? (1.0 + cosine) / 2.0 : (1.0 - cosine) / 2.0;
    filter.b0_ = edge / a0;
    filter.b1_ = (high ? -2.0 * edge : 2.0 * edge) / a0;
    filter.b2_ = edge / a0;
    filter.a1_ = -2.0 * cosine / a0;
    filter.a2_ = (1.0 - alpha) / a0;
    return filter;
  }
  double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
  double z1_ = 0.0, z2_ = 0.0;
};

// Two cascaded Butterworth sections: a fourth-order band edge.
class Edge {
public:
  Edge(double rate, double hz, bool high)
      : first_(high ? Biquad::highpass(rate, hz) : Biquad::lowpass(rate, hz)),
        second_(high ? Biquad::highpass(rate, hz) : Biquad::lowpass(rate, hz)) {}
  double process(double input) noexcept { return second_.process(first_.process(input)); }

private:
  Biquad first_;
  Biquad second_;
};

} // namespace

std::string_view chop_tag_id(ChopTag tag) noexcept {
  return tag_ids[static_cast<std::size_t>(tag)];
}

std::string_view chop_tag_name(ChopTag tag) noexcept {
  return tag_names[static_cast<std::size_t>(tag)];
}

StartsResult chop_transients(const SampleAudio& audio, float sensitivity) {
  if (!usable(audio))
    return StartsResult::failure(invalid("Crate cannot chop an empty or unsupported sample"));
  if (!std::isfinite(sensitivity) || sensitivity < 0.0F || sensitivity > 1.0F)
    return StartsResult::failure(invalid("Chop sensitivity must be between 0 and 1"));
  try {
    std::vector<float> left(audio.frames.size());
    std::vector<float> right(audio.frames.size());
    for (std::size_t index = 0; index < audio.frames.size(); ++index) {
      left[index] = audio.frames[index].left;
      right[index] = audio.frames[index].right;
    }
    warp_dsp::MemorySourceView view(left, right);
    warp_dsp::analysis::TransientOptions options;
    options.sample_rate = audio.source_rate;
    options.threshold_scale = static_cast<float>(
        std::exp2(sensitivity_scale_octaves * (1.0 - 2.0 * static_cast<double>(sensitivity))));
    auto detected = warp_dsp::analysis::detect_transients(view, options);
    if (!detected)
      return StartsResult::failure(std::move(detected.error()));
    std::vector<Candidate> candidates;
    candidates.reserve(detected.value().size());
    for (const auto& transient : detected.value())
      candidates.push_back({transient.frame, static_cast<double>(transient.strength)});
    return StartsResult::success(normalise(std::move(candidates), audio));
  } catch (const std::bad_alloc&) {
    return StartsResult::failure({ErrorCode::resource_exhausted, "Chopping ran out of memory"});
  }
}

StartsResult chop_grid(const SampleAudio& audio, double bpm, std::uint32_t division,
                       std::uint64_t first_beat_frame) {
  if (!usable(audio))
    return StartsResult::failure(invalid("Crate cannot chop an empty or unsupported sample"));
  if (!std::isfinite(bpm) || bpm < minimum_chop_bpm || bpm > maximum_chop_bpm)
    return StartsResult::failure(invalid("Chop tempo is out of range"));
  if (division != grid_division_quarter && division != grid_division_eighth &&
      division != grid_division_sixteenth)
    return StartsResult::failure(invalid("Chop grid must be quarters, eighths or sixteenths"));
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  if (first_beat_frame >= frames)
    return StartsResult::failure(invalid("First beat is outside the sample"));
  const double step = static_cast<double>(audio.source_rate) * seconds_per_minute / bpm *
                      quarters_per_bar / static_cast<double>(division);
  const auto first = static_cast<double>(first_beat_frame);
  // Lines before the first beat (a pickup) count too.
  const auto before = static_cast<std::int64_t>(std::floor(first / step));
  std::vector<Candidate> candidates;
  // The earliest lines win the cap, so one more than it is enough.
  for (std::int64_t line = -before; candidates.size() <= max_crate_chops; ++line) {
    const auto position = std::llround(first + static_cast<double>(line) * step);
    if (position < 0)
      continue;
    const auto frame = static_cast<std::uint64_t>(position);
    if (frame >= frames)
      break;
    candidates.push_back(ordered(frame));
  }
  return StartsResult::success(normalise(std::move(candidates), audio));
}

StartsResult chop_equal(const SampleAudio& audio, std::size_t count) {
  if (!usable(audio))
    return StartsResult::failure(invalid("Crate cannot chop an empty or unsupported sample"));
  if (count == 0 || count > max_crate_chops)
    return StartsResult::failure(invalid("Equal chops must number 1 to 32"));
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index)
    candidates.push_back(ordered(index * frames / count));
  return StartsResult::success(normalise(std::move(candidates), audio));
}

ChopStarts add_chop_start(const ChopStarts& starts, std::uint64_t frame, const SampleAudio& audio) {
  if (audio.frames.empty() || starts.size() >= max_crate_chops)
    return starts;
  frame = std::min<std::uint64_t>(frame, audio.frames.size() - 1);
  const auto gap = merge_frames(audio);
  const auto next = std::ranges::lower_bound(starts, frame);
  if (next != starts.end() && *next - frame < gap)
    return starts;
  if (next != starts.begin() && frame - *std::prev(next) < gap)
    return starts;
  ChopStarts result;
  result.reserve(starts.size() + 1);
  result.insert(result.end(), starts.begin(), next);
  result.push_back(frame);
  result.insert(result.end(), next, starts.end());
  return result;
}

ChopStarts move_chop_start(const ChopStarts& starts, std::size_t index, std::uint64_t frame,
                           const SampleAudio& audio) {
  if (index == 0 || index >= starts.size() || audio.frames.empty())
    return starts;
  const auto gap = merge_frames(audio);
  const auto lowest = starts[index - 1] + gap;
  const auto end = index + 1 < starts.size() ? starts[index + 1]
                                             : static_cast<std::uint64_t>(audio.frames.size());
  const std::uint64_t highest = index + 1 < starts.size() ? (end >= gap ? end - gap : 0) : end - 1;
  if (lowest > highest)
    return starts;
  ChopStarts result = starts;
  result[index] = std::clamp(frame, lowest, highest);
  return result;
}

ChopStarts remove_chop_start(const ChopStarts& starts, std::size_t index) {
  if (index == 0 || index >= starts.size())
    return starts;
  ChopStarts result = starts;
  result.erase(result.begin() + static_cast<std::ptrdiff_t>(index));
  return result;
}

ChopTag classify_chop(const SampleAudio& audio, std::uint64_t start, std::uint64_t end) {
  const auto rate = static_cast<double>(audio.source_rate);
  if (audio.source_rate < minimum_classify_rate)
    return ChopTag::other;
  end = std::min<std::uint64_t>(end, audio.frames.size());
  if (start >= end)
    return ChopTag::other;
  end = std::min<std::uint64_t>(
      end, start + static_cast<std::uint64_t>(std::llround(classify_window_seconds * rate)));
  Edge low(rate, low_band_hz, false);
  Edge mid_floor(rate, low_band_hz, true);
  Edge mid_ceiling(rate, mid_band_top_hz, false);
  Edge noise(rate, mid_band_top_hz, true);
  Edge high(rate, high_band_hz, true);
  double total = 0.0;
  double low_energy = 0.0;
  double mid_energy = 0.0;
  double noise_energy = 0.0;
  double high_energy = 0.0;
  const auto block = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::llround(envelope_block_seconds * rate)));
  std::vector<double> envelope;
  double block_energy = 0.0;
  std::uint64_t in_block = 0;
  for (auto frame = start; frame < end; ++frame) {
    const auto& sample = audio.frames[frame];
    const double input = 0.5 * (static_cast<double>(sample.left) + sample.right);
    const double low_out = low.process(input);
    const double mid_out = mid_ceiling.process(mid_floor.process(input));
    const double noise_out = noise.process(input);
    const double high_out = high.process(input);
    total += input * input;
    low_energy += low_out * low_out;
    mid_energy += mid_out * mid_out;
    noise_energy += noise_out * noise_out;
    high_energy += high_out * high_out;
    block_energy += input * input;
    if (++in_block == block) {
      envelope.push_back(std::sqrt(block_energy / static_cast<double>(block)));
      block_energy = 0.0;
      in_block = 0;
    }
  }
  if (in_block > 0)
    envelope.push_back(std::sqrt(block_energy / static_cast<double>(in_block)));
  if (total <= silence_energy)
    return ChopTag::other;
  // Time from the loudest block to the first block 20 dB below it; a chop
  // that never falls that far counts as sustained.
  const auto peak = std::ranges::max_element(envelope);
  const auto quiet = *peak * decay_floor_ratio;
  const auto fallen =
      std::find_if(peak, envelope.end(), [quiet](double level) { return level <= quiet; });
  const double decay_seconds = fallen == envelope.end() ? std::numeric_limits<double>::infinity()
                                                        : static_cast<double>(fallen - peak) *
                                                              static_cast<double>(block) / rate;
  const bool short_decay = decay_seconds <= drum_maximum_decay_seconds;
  if (high_energy / total >= hat_minimum_high_share)
    return ChopTag::hat;
  if (low_energy / total >= kick_minimum_low_share && short_decay)
    return ChopTag::kick;
  if (noise_energy / total >= snare_minimum_noise_share &&
      mid_energy / total >= snare_minimum_mid_share && short_decay)
    return ChopTag::snare;
  return ChopTag::other;
}

std::uint8_t detect_root(const SampleAudio& audio) {
  if (!usable(audio))
    return default_root_key;
  try {
    std::vector<float> left(audio.frames.size());
    std::vector<float> right(audio.frames.size());
    for (std::size_t index = 0; index < audio.frames.size(); ++index) {
      left[index] = audio.frames[index].left;
      right[index] = audio.frames[index].right;
    }
    warp_dsp::MemorySourceView view(left, right);
    warp_dsp::analysis::PitchOptions options;
    options.sample_rate = audio.source_rate;
    options.minimum_hz = root_minimum_hz;
    options.maximum_hz =
        std::min(root_maximum_hz, static_cast<float>(root_maximum_rate_share *
                                                     static_cast<double>(audio.source_rate)));
    const auto track = warp_dsp::analysis::track_pitch(view, options);
    if (!track)
      return default_root_key;
    std::vector<float> voiced;
    for (const auto& frame : track.value()) {
      if (frame.frequency > 0.0F)
        voiced.push_back(frame.frequency);
    }
    if (voiced.size() < minimum_voiced_frames)
      return default_root_key;
    const auto middle = voiced.begin() + static_cast<std::ptrdiff_t>(voiced.size() / 2);
    std::ranges::nth_element(voiced, middle);
    const auto key = std::lround(a4_key + static_cast<double>(foundation::semitones_per_octave) *
                                              std::log2(static_cast<double>(*middle) / a4_hz));
    return static_cast<std::uint8_t>(
        std::clamp(key, 0L, static_cast<long>(foundation::max_midi_key)));
  } catch (const std::bad_alloc&) {
    return default_root_key;
  }
}

foundation::Result<Rechop> make_regions(std::span<const project::NativeRegion> previous,
                                        const ChopStarts& starts, const SampleAudio& audio) {
  using RechopResult = foundation::Result<Rechop>;
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  if (starts.empty() || starts.size() > max_crate_chops || starts.front() != 0 ||
      starts.back() >= frames ||
      std::ranges::adjacent_find(starts, std::ranges::greater_equal{}) != starts.end())
    return RechopResult::failure(invalid("Chop starts are not normalised for this sample"));
  const auto default_name = [](std::string_view name) {
    return name.empty() || std::ranges::find(tag_names, name) != tag_names.end();
  };
  const auto eligible = [](const project::NativeRegion& region) {
    return region.media == crate_sample_media && region.slot < max_crate_chops;
  };
  std::bitset<max_crate_chops> used;
  std::vector<const project::NativeRegion*> kept(starts.size(), nullptr);
  for (std::size_t index = 0; index < starts.size(); ++index) {
    for (const auto& region : previous) {
      if (eligible(region) && region.start_frame == starts[index] && !used.test(region.slot)) {
        kept[index] = &region;
        used.set(region.slot);
        break;
      }
    }
  }
  Rechop result;
  for (const auto& region : previous) {
    const bool survived = std::ranges::find(kept, &region) != kept.end();
    if (eligible(region) && !survived)
      result.freed_slots.push_back(region.slot);
  }
  std::ranges::sort(result.freed_slots);
  const auto [unique_end, last] = std::ranges::unique(result.freed_slots);
  result.freed_slots.erase(unique_end, last);
  result.regions.reserve(starts.size());
  std::uint32_t next_free = 0;
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const auto end = index + 1 < starts.size() ? starts[index + 1] : frames;
    const auto tag = classify_chop(audio, starts[index], end);
    project::NativeRegion region;
    region.media = std::string(crate_sample_media);
    region.start_frame = starts[index];
    region.tag = std::string(chop_tag_id(tag));
    region.name = std::string(chop_tag_name(tag));
    if (kept[index] != nullptr) {
      region.slot = kept[index]->slot;
      if (!default_name(kept[index]->name))
        region.name = kept[index]->name;
    } else {
      while (used.test(next_free))
        ++next_free;
      region.slot = next_free;
      used.set(next_free);
    }
    result.regions.push_back(std::move(region));
  }
  return RechopResult::success(std::move(result));
}

ChopStarts region_starts(std::span<const project::NativeRegion> regions) {
  ChopStarts starts;
  for (const auto& region : regions) {
    if (region.media == crate_sample_media)
      starts.push_back(region.start_frame);
  }
  std::ranges::sort(starts);
  return starts;
}

std::vector<project::NativeRegion> move_region(std::span<const project::NativeRegion> regions,
                                               std::size_t index, std::uint64_t frame,
                                               const SampleAudio& audio) {
  std::vector<project::NativeRegion> result(regions.begin(), regions.end());
  std::ranges::stable_sort(result, {}, &project::NativeRegion::start_frame);
  if (std::ranges::any_of(result, [](const project::NativeRegion& region) {
        return region.media != crate_sample_media;
      }))
    return result;
  const auto starts = region_starts(result);
  const auto moved = move_chop_start(starts, index, frame, audio);
  if (moved == starts)
    return result;
  result[index].start_frame = moved[index];
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  // The moved region and the one before it end elsewhere now.
  for (const auto changed : {index - 1, index}) {
    auto& region = result[changed];
    const auto end = changed + 1 < result.size() ? result[changed + 1].start_frame : frames;
    const auto tag = classify_chop(audio, region.start_frame, end);
    const bool default_name =
        region.name.empty() || std::ranges::find(tag_names, region.name) != tag_names.end();
    region.tag = std::string(chop_tag_id(tag));
    if (default_name)
      region.name = std::string(chop_tag_name(tag));
  }
  return result;
}

foundation::Result<ChopClip> chop_to_midi(std::span<const project::NativeRegion> regions,
                                          std::uint64_t frames, std::uint32_t source_rate,
                                          double bpm) {
  using ClipResult = foundation::Result<ChopClip>;
  if (regions.empty() || regions.size() > max_crate_chops)
    return ClipResult::failure(invalid("Chop to MIDI needs 1 to 32 chops"));
  if (frames == 0 || source_rate == 0)
    return ClipResult::failure(invalid("Chop to MIDI needs a sample"));
  if (!std::isfinite(bpm) || bpm < minimum_chop_bpm || bpm > maximum_chop_bpm)
    return ClipResult::failure(invalid("Chop to MIDI needs the sample's tempo"));
  const double ticks_per_frame = bpm / seconds_per_minute / static_cast<double>(source_rate) *
                                 static_cast<double>(timeline::ticks_per_quarter);
  const auto tick_of = [ticks_per_frame](std::uint64_t frame) {
    return timeline::TickPosition{
        static_cast<std::int64_t>(std::llround(static_cast<double>(frame) * ticks_per_frame))};
  };
  const auto placeholder = project::NoteId::from_bits({0, 1});
  if (!placeholder)
    return ClipResult::failure(placeholder.error());
  ChopClip clip;
  clip.notes.reserve(regions.size());
  for (std::size_t index = 0; index < regions.size(); ++index) {
    const auto start = regions[index].start_frame;
    const auto end = index + 1 < regions.size() ? regions[index + 1].start_frame : frames;
    if (start >= end || end > frames)
      return ClipResult::failure(invalid("Chops are not in time order inside the sample"));
    const auto from = tick_of(start);
    const auto to = tick_of(end);
    if (to <= from)
      return ClipResult::failure(invalid("A chop is too short for a note"));
    clip.notes.push_back(
        {placeholder.value(), from, to,
         static_cast<std::uint8_t>(native_dsp::synth::crate::first_pad_key + index),
         chop_note_velocity});
  }
  const auto bar_ticks = static_cast<std::int64_t>(quarters_per_bar) * timeline::ticks_per_quarter;
  const auto end = clip.notes.back().end.value;
  clip.length = timeline::TickPosition{(end + bar_ticks - 1) / bar_ticks * bar_ticks};
  return ClipResult::success(std::move(clip));
}

foundation::Result<ChopStarts> range_chop_starts(const ChopStarts& hits, std::uint64_t first,
                                                 std::uint64_t end, const SampleAudio& audio) {
  const auto frames = static_cast<std::uint64_t>(audio.frames.size());
  if (first >= end || end > frames)
    return StartsResult::failure(invalid("The clip plays outside its sample"));
  const auto gap = merge_frames(audio);
  ChopStarts starts{0};
  // The first played chop starts with the clip, or at 0 when the clip
  // starts too close to it for a lead-in.
  if (first >= gap)
    starts.push_back(first);
  const auto played_start = starts.back();
  for (const auto hit : hits) {
    if (hit == 0 || hit >= end - first)
      continue;
    const auto frame = first + hit;
    if (frame - starts.back() >= gap)
      starts.push_back(frame);
  }
  const bool tail = end < frames;
  if (tail) {
    while (starts.back() != played_start && end - starts.back() < gap)
      starts.pop_back();
    starts.push_back(end);
  }
  // The latest hits give way; the lead-in, first played and tail starts stay.
  const auto kept_after = static_cast<std::ptrdiff_t>(tail ? 2 : 1);
  while (starts.size() > max_crate_chops)
    starts.erase(starts.end() - kept_after);
  return StartsResult::success(std::move(starts));
}

std::string utf8_prefix(std::string text, std::size_t bytes) {
  if (text.size() <= bytes)
    return text;
  constexpr unsigned char continuation_mask = 0xC0U;
  constexpr unsigned char continuation_bits = 0x80U;
  std::size_t end = bytes;
  while (end > 0 &&
         (static_cast<unsigned char>(text[end]) & continuation_mask) == continuation_bits)
    --end;
  text.resize(end);
  return text;
}

void reset_chop_slots(const native_dsp::EngineDescriptor& engine, std::vector<double>& values,
                      std::span<const std::uint32_t> chop_slots) {
  for (const auto slot : chop_slots) {
    for (const auto control : native_dsp::synth::crate::chop_controls) {
      const auto index = native_dsp::find_parameter(
          engine, native_dsp::synth::crate::chop_parameter_id(slot, control));
      if (index && *index < values.size())
        values[*index] = engine.parameters[*index].default_value;
    }
  }
}

foundation::Result<SampleState> new_sample_state(const native_dsp::EngineDescriptor& engine,
                                                 std::vector<double> values,
                                                 std::span<const std::uint32_t> previous_slots,
                                                 const ChopStarts& starts, const SampleAudio& audio,
                                                 std::optional<double> bpm, std::string name) {
  using StateResult = foundation::Result<SampleState>;
  namespace facts = native_dsp::synth::crate;
  // A new sample: none of the old chops carry over.
  auto made = make_regions({}, starts, audio);
  if (!made)
    return StateResult::failure(made.error());
  std::vector<std::uint32_t> chop_slots(previous_slots.begin(), previous_slots.end());
  for (const auto& region : made.value().regions)
    chop_slots.push_back(region.slot);
  reset_chop_slots(engine, values, chop_slots);
  if (const auto root = native_dsp::find_parameter(engine, root_parameter);
      root && *root < values.size())
    values[*root] = static_cast<double>(detect_root(audio));
  SampleState state;
  if (bpm && std::isfinite(*bpm) && *bpm >= facts::min_sample_bpm &&
      *bpm <= facts::max_sample_bpm) {
    const double rounded = std::round(*bpm * bpm_label_steps_per_beat) / bpm_label_steps_per_beat;
    std::array<char, bpm_text_capacity> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), rounded);
    if (error == std::errc{})
      state.labels.push_back({std::string(facts::bpm_label), std::string(buffer.data(), end)});
  }
  if (auto label = utf8_prefix(std::move(name), project::max_native_label_length); !label.empty())
    state.labels.push_back({std::string(facts::sample_name_label), std::move(label)});
  state.regions = std::move(made.value().regions);
  state.values = std::move(values);
  return StateResult::success(std::move(state));
}

} // namespace oscillate::application::crate
