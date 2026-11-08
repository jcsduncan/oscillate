#include "../core/bandlimited_kernel.hpp"
#include "../core/bandlimited_read.hpp"
#include "../core/real_fft.hpp"
#include "../core/source_window.hpp"
#include "oscillate/foundation/stereo_span.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <optional>

namespace oscillate::warp_dsp {
namespace {

using detail::AlignedBuffer;
using detail::RealFft;
using detail::SourceWindow;

constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;
// Seek pre-roll bound (frames at 48 kHz, doubled at high rates).
constexpr std::int64_t max_preroll_48k = std::int64_t{1} << 15;

[[nodiscard]] float wrap_phase(float phase) noexcept {
  return phase - two_pi * std::nearbyint(phase / two_pi);
}

[[nodiscard]] std::int64_t source_frame(std::uint64_t position) noexcept {
  return static_cast<std::int64_t>((position + (source_unit >> 1)) >> source_fraction_bits);
}

enum class AnchorKind : std::uint8_t { none, start, end, onset, splice };

// A boundary in output time where source content must switch cleanly.
struct Anchor {
  AnchorKind kind = AnchorKind::none;
  std::int64_t output = 0;
  std::int64_t source_before = 0; // source frame reached at `output` by the incoming run
  std::int64_t source_after = 0;  // source frame starting at `output`
  std::int64_t ramp = 0;          // crossfade half-width (splices)
};

// What one synthesis frame reads and keeps.
struct FramePlan {
  bool active = false;
  bool epoch = false;
  std::int64_t centre = 0; // analysis centre, source frames
  // Kept source range [source_low, source_high) with linear ramps of the given
  // widths outside it; output contributions are limited to [output_low,
  // output_high) the same way.
  std::int64_t source_low = std::numeric_limits<std::int64_t>::min() / 4;
  std::int64_t source_high = std::numeric_limits<std::int64_t>::max() / 4;
  std::int64_t output_low = std::numeric_limits<std::int64_t>::min() / 4;
  std::int64_t output_high = std::numeric_limits<std::int64_t>::max() / 4;
  std::int64_t ramp_low = 0;
  std::int64_t ramp_high = 0;
};

[[nodiscard]] float keep_gain(std::int64_t value, std::int64_t low, std::int64_t high,
                              std::int64_t ramp_low, std::int64_t ramp_high) noexcept {
  if (value < low - ramp_low || value >= high + ramp_high)
    return 0.0F;
  if (value < low)
    return static_cast<float>(value - (low - ramp_low)) / static_cast<float>(ramp_low);
  if (value >= high)
    return static_cast<float>(high + ramp_high - value) / static_cast<float>(ramp_high);
  return 1.0F;
}

// Settings shared by Complex and Complex Pro.
struct Config {
  std::vector<std::uint64_t> onsets;
  std::int32_t transpose_cents = 0;
  bool shape_formants = false;
  float formants = 0.0F;
  float envelope = 128.0F;
};

// Intermediate (pre-resampling) buffer for transposed output.
constexpr std::size_t intermediate_capacity = 16'384;
constexpr std::size_t output_chunk = 256;
// True-envelope iterations: enough to settle on harmonic peaks at the lifter
// sizes Complex Pro allows (measured on the synthetic vowel fixture).
constexpr int true_envelope_iterations = 8;
// Log-spectrum floor for envelope estimation: 60 dB below the frame peak.
constexpr float envelope_floor = 6.9078F;
// Spectral decimation for envelope estimation.
constexpr std::size_t envelope_decimation = 4;

class ComplexStream final : public WarpStream {
public:
  ComplexStream(const StreamLimits& limits, QualityTier tier, Config settings)
      : limits_(limits), settings_(std::move(settings)),
        pitch_(std::exp2(settings_.transpose_cents / 1200.0)),
        pitched_(settings_.transpose_cents != 0),
        reader_(detail::BandlimitedKernel::for_tier(tier)->design(), pitch_),
        reach_(reader_.reach()), size_(limits.sample_rate > 66'150 ? 8'192 : 4'096),
        hop_(static_cast<std::int64_t>(tier == QualityTier::q2 ? size_ / 4 : size_ / 8)),
        half_(size_ / 2), bins_(size_ / 2 + 1),
        preroll_(max_preroll_48k * (limits.sample_rate > 66'150 ? 2 : 1)), fft_(size_),
        window_(size_), left_(size_), right_(size_), lagged_(size_), rotation_(bins_, 0.0F),
        previous_mono_(2 * bins_, 0.0F), power_(bins_, 0.0F), peaks_(bins_),
        peak_rotation_(bins_, 0.0F), peak_cos_(bins_, 1.0F), peak_sin_(bins_, 0.0F),
        ring_mask_(ring_size(size_) - 1), ring_left_(ring_size(size_), 0.0F),
        ring_right_(ring_size(size_), 0.0F), ring_weight_(ring_size(size_), 0.0F),
        source_(size_ + static_cast<std::size_t>(hop_) + 4),
        cepstrum_(settings_.shape_formants ? size_ / envelope_decimation : 0),
        envelope_fft_(settings_.shape_formants
                          ? std::make_unique<RealFft>(size_ / envelope_decimation)
                          : nullptr),
        coarse_target_(settings_.shape_formants ? size_ / (2 * envelope_decimation) + 1 : 0, 0.0F),
        coarse_envelope_(coarse_target_.size(), 0.0F),
        envelope_(settings_.shape_formants ? bins_ : 0, 0.0F),
        target_(settings_.shape_formants ? bins_ : 0, 0.0F),
        intermediate_left_(pitched_ ? intermediate_capacity : 0, 0.0F),
        intermediate_right_(pitched_ ? intermediate_capacity : 0, 0.0F) {
    // Envelope detail maps to a cepstral cutoff of envelope / 2 samples at
    // 44.1 kHz: the default (128) resolves envelopes of voices up to about
    // 340 Hz without following individual harmonics.
    const auto lifter = std::llround(static_cast<double>(settings_.envelope) * limits.sample_rate /
                                     (2.0 * 44'100.0));
    lifter_ = static_cast<std::size_t>(std::clamp<long long>(
        lifter, 2, static_cast<long long>(size_ / (2 * envelope_decimation)) - 1));
    for (std::size_t index = 0; index < size_; ++index)
      window_[index] = static_cast<float>(
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                               static_cast<double>(size_)));
  }

  StreamStatus start(const WarpTimeMap& map, SourceView& source,
                     std::uint64_t output_frame) noexcept override {
    map_ = nullptr;
    view_ = nullptr;
    source_.reset();
    if (output_frame >= map.output_frames())
      return status_ = StreamStatus::out_of_range;
    index_splices(map);
    map_ = &map;
    view_ = &source;
    next_ = output_frame;
    std::ranges::fill(ring_left_, 0.0F);
    std::ranges::fill(ring_right_, 0.0F);
    std::ranges::fill(ring_weight_, 0.0F);
    // Frames contributing to intermediate position u have centres in
    // (u - N/2, u + N/2). Transposed output also needs the resampler's reach.
    const auto target =
        pitched_ ? static_cast<std::int64_t>(intermediate_position(output_frame) >> 16) - reach_
                 : static_cast<std::int64_t>(output_frame);
    intermediate_base_ = target;
    intermediate_filled_ = 0;
    const auto first = floor_div(target - static_cast<std::int64_t>(half_), hop_) + 1;
    frame_ = std::max(restart_frame(first), first - preroll_ / hop_);
    force_epoch_ = true;
    consumed_ = frame_ * hop_ - static_cast<std::int64_t>(half_);
    return status_ = StreamStatus::ok;
  }

  std::size_t render(std::span<float> left, std::span<float> right) noexcept override {
    if (status_ != StreamStatus::ok || left.size() != right.size())
      return 0;
    if (pitched_)
      return render_transposed({.left = left, .right = right});
    const auto requested = std::min<std::size_t>(left.size(), limits_.max_render_frames);
    std::size_t written = 0;
    const auto end = static_cast<std::int64_t>(map_->output_frames());
    while (written < requested && static_cast<std::int64_t>(next_) < end) {
      const auto want = static_cast<std::int64_t>(next_);
      // Output before the next frame's window start is final.
      const auto ready = frame_ * hop_ - static_cast<std::int64_t>(half_);
      // Pre-roll output before the requested frame is computed but dropped.
      if (consumed_ < want && consumed_ < ready) {
        discard(std::min(want, ready));
        continue;
      }
      if (want < ready) {
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(
            {ready - want, end - want, static_cast<std::int64_t>(requested - written)}));
        emit(want, {.left = left.subspan(written, count), .right = right.subspan(written, count)});
        written += count;
        next_ += count;
        continue;
      }
      if (!process_frame(frame_)) {
        status_ = StreamStatus::source_failed;
        break;
      }
      ++frame_;
    }
    return written;
  }

  std::uint64_t next_output_frame() const noexcept override { return next_; }
  StreamStatus status() const noexcept override { return status_; }

private:
  // Intermediate position (Q48.16) of output frame t: u = t * p.
  [[nodiscard]] std::uint64_t intermediate_position(std::uint64_t output) const noexcept {
    return static_cast<std::uint64_t>(
        std::llround(static_cast<double>(output) * pitch_ * static_cast<double>(source_unit)));
  }
  [[nodiscard]] std::int64_t to_intermediate(std::int64_t output) const noexcept {
    return pitched_ ? static_cast<std::int64_t>(std::llround(static_cast<double>(output) * pitch_))
                    : output;
  }
  [[nodiscard]] std::int64_t to_output(std::int64_t intermediate) const noexcept {
    return pitched_
               ? static_cast<std::int64_t>(std::llround(static_cast<double>(intermediate) / pitch_))
               : intermediate;
  }
  [[nodiscard]] std::int64_t intermediate_end() const noexcept {
    return to_intermediate(static_cast<std::int64_t>(map_->output_frames()));
  }

  // Makes intermediate samples [low, high) resident, running the vocoder as
  // needed; positions past the stream's end are silence.
  // Half-open range of intermediate positions.
  struct Range {
    std::int64_t low;
    std::int64_t high;
  };

  [[nodiscard]] bool fill_intermediate(Range range) noexcept {
    const auto low = range.low;
    const auto high = range.high;
    if (low > intermediate_base_) {
      const auto drop = static_cast<std::size_t>(std::min<std::int64_t>(
          low - intermediate_base_, static_cast<std::int64_t>(intermediate_filled_)));
      std::copy(intermediate_left_.begin() + static_cast<std::ptrdiff_t>(drop),
                intermediate_left_.begin() + static_cast<std::ptrdiff_t>(intermediate_filled_),
                intermediate_left_.begin());
      std::copy(intermediate_right_.begin() + static_cast<std::ptrdiff_t>(drop),
                intermediate_right_.begin() + static_cast<std::ptrdiff_t>(intermediate_filled_),
                intermediate_right_.begin());
      intermediate_filled_ -= drop;
      intermediate_base_ += static_cast<std::int64_t>(drop);
    }
    const auto end = intermediate_end();
    while (intermediate_base_ + static_cast<std::int64_t>(intermediate_filled_) < high) {
      const auto position = intermediate_base_ + static_cast<std::int64_t>(intermediate_filled_);
      if (intermediate_filled_ == intermediate_capacity)
        return false;
      if (position >= end) {
        intermediate_left_[intermediate_filled_] = 0.0F;
        intermediate_right_[intermediate_filled_] = 0.0F;
        ++intermediate_filled_;
        continue;
      }
      const auto ready = frame_ * hop_ - static_cast<std::int64_t>(half_);
      if (consumed_ < position && consumed_ < ready) {
        discard(std::min(position, ready));
        continue;
      }
      if (position < ready) {
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(
            {ready - position, end - position, high - position,
             static_cast<std::int64_t>(intermediate_capacity - intermediate_filled_)}));
        emit(position,
             {.left = std::span(intermediate_left_).subspan(intermediate_filled_, count),
              .right = std::span(intermediate_right_).subspan(intermediate_filled_, count)});
        intermediate_filled_ += count;
        continue;
      }
      if (!process_frame(frame_))
        return false;
      ++frame_;
    }
    return true;
  }

  // Transposed output: band-limited reads of the intermediate stream at
  // u = t * p.
  std::size_t render_transposed(foundation::StereoSpan<float> out) noexcept {
    const auto left = out.left;
    const auto right = out.right;
    const auto requested = std::min<std::size_t>(left.size(), limits_.max_render_frames);
    const auto end = map_->output_frames();
    std::size_t written = 0;
    while (written < requested && next_ < end) {
      const auto count = std::min<std::size_t>(
          {output_chunk, requested - written, static_cast<std::size_t>(end - next_)});
      const auto first = intermediate_position(next_);
      const auto last = intermediate_position(next_ + count - 1);
      const auto low = static_cast<std::int64_t>(first >> source_fraction_bits) - reach_;
      const auto high = static_cast<std::int64_t>(last >> source_fraction_bits) + reach_ + 1;
      if (!fill_intermediate({low, high})) {
        status_ = StreamStatus::source_failed;
        break;
      }
      for (std::size_t index = 0; index < count; ++index) {
        const auto position = intermediate_position(next_ + index);
        const auto offset = static_cast<std::size_t>(
            static_cast<std::int64_t>(position >> source_fraction_bits) - intermediate_base_);
        const auto sample = reader_.read_frame(
            intermediate_left_.data() + offset, intermediate_right_.data() + offset,
            static_cast<std::uint32_t>(position & (source_unit - 1)));
        left[written + index] = sample.left;
        right[written + index] = sample.right;
      }
      written += count;
      next_ += count;
    }
    return written;
  }

  static std::size_t ring_size(std::size_t size) noexcept {
    std::size_t ring = 1;
    while (ring < 2 * size)
      ring <<= 1;
    return ring;
  }

  static std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient * divisor > value ? quotient - 1 : quotient;
  }

  // Bounds of the splice-free run of segments containing `index`.
  struct Run {
    std::size_t first = 0;
    std::size_t last = 0;
  };

  // Three-level splice bit index. The map permits up to 2^20 segments, so
  // every predecessor/successor query visits at most three 64-bit words and
  // four top-level words, independent of the length of a splice-free run.
  static constexpr std::size_t splice_words = (max_time_map_segments + 63) / 64;
  static constexpr std::size_t splice_groups = (splice_words + 63) / 64;
  static constexpr std::size_t splice_roots = (splice_groups + 63) / 64;

  void index_splices(const WarpTimeMap& map) noexcept {
    splices_.fill(0);
    splice_groups_.fill(0);
    splice_roots_.fill(0);
    const auto segments = map.segments();
    for (std::size_t index = 1; index < segments.size(); ++index) {
      if (!segments[index].splice)
        continue;
      const auto word = index / 64;
      const auto group = word / 64;
      splices_[word] |= std::uint64_t{1} << (index % 64);
      splice_groups_[group] |= std::uint64_t{1} << (word % 64);
      splice_roots_[group / 64] |= std::uint64_t{1} << (group % 64);
    }
  }

  [[nodiscard]] static std::uint64_t through(std::size_t bit) noexcept {
    return ~std::uint64_t{0} >> (63 - bit);
  }

  [[nodiscard]] static std::uint64_t after(std::size_t bit) noexcept {
    return bit == 63 ? 0 : ~through(bit);
  }

  [[nodiscard]] std::optional<std::size_t> previous_splice(std::size_t index) const noexcept {
    auto word = index / 64;
    auto bits = splices_[word] & through(index % 64);
    if (bits == 0) {
      auto group = word / 64;
      auto words = splice_groups_[group] & (word % 64 == 0 ? 0 : through(word % 64 - 1));
      if (words == 0) {
        auto root = group / 64;
        auto groups = splice_roots_[root] & (group % 64 == 0 ? 0 : through(group % 64 - 1));
        if (groups == 0) {
          while (root > 0 && groups == 0)
            groups = splice_roots_[--root];
          if (groups == 0)
            return std::nullopt;
        }
        group = root * 64 + 63 - static_cast<std::size_t>(std::countl_zero(groups));
        words = splice_groups_[group];
      }
      word = group * 64 + 63 - static_cast<std::size_t>(std::countl_zero(words));
      bits = splices_[word];
    }
    return word * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits));
  }

  [[nodiscard]] std::optional<std::size_t> next_splice(std::size_t index) const noexcept {
    auto word = index / 64;
    auto bits = splices_[word] & after(index % 64);
    if (bits == 0) {
      auto group = word / 64;
      auto words = splice_groups_[group] & after(word % 64);
      if (words == 0) {
        auto root = group / 64;
        auto groups = splice_roots_[root] & after(group % 64);
        if (groups == 0) {
          while (groups == 0 && root + 1 < splice_roots)
            groups = splice_roots_[++root];
          if (groups == 0)
            return std::nullopt;
        }
        group = root * 64 + static_cast<std::size_t>(std::countr_zero(groups));
        words = splice_groups_[group];
      }
      word = group * 64 + static_cast<std::size_t>(std::countr_zero(words));
      bits = splices_[word];
    }
    return word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
  }

  [[nodiscard]] Run run_of(std::size_t index) const noexcept {
    const auto first = previous_splice(index).value_or(0);
    const auto next = next_splice(index);
    return {first, next ? *next - 1 : map_->segments().size() - 1};
  }

  // First output frame where the run reaches source frame `frame`.
  [[nodiscard]] std::int64_t output_of(const Run& run, std::int64_t frame) const noexcept {
    const auto segments = map_->segments();
    const auto position = static_cast<std::uint64_t>(frame) << source_fraction_bits;
    const auto found = std::ranges::upper_bound(segments.subspan(run.first, run.last - run.first),
                                                position, {}, &TimeMapSegment::source_end);
    const auto index = static_cast<std::size_t>(found - segments.begin());
    return to_intermediate(
        static_cast<std::int64_t>(WarpTimeMap::first_output_reaching(segments[index], position)));
  }

  [[nodiscard]] Anchor run_start(const Run& run) const noexcept {
    const auto& segment = map_->segments()[run.first];
    Anchor anchor;
    anchor.output = to_intermediate(static_cast<std::int64_t>(segment.output_begin));
    anchor.source_after = source_frame(segment.source_begin);
    if (run.first == 0) {
      anchor.kind = AnchorKind::start;
      anchor.source_before = anchor.source_after;
    } else {
      anchor.kind = AnchorKind::splice;
      anchor.source_before = source_frame(
          WarpTimeMap::extrapolate(map_->segments()[run.first - 1], segment.output_begin));
      anchor.ramp = splice_crossfade_frames;
    }
    return anchor;
  }

  [[nodiscard]] Anchor run_end(const Run& run) const noexcept {
    const auto segments = map_->segments();
    const auto& segment = segments[run.last];
    Anchor anchor;
    anchor.output = to_intermediate(static_cast<std::int64_t>(segment.output_end));
    anchor.source_before = source_frame(segment.source_end);
    if (run.last + 1 == segments.size()) {
      anchor.kind = AnchorKind::end;
      anchor.source_after = anchor.source_before;
    } else {
      anchor.kind = AnchorKind::splice;
      anchor.source_after = source_frame(segments[run.last + 1].source_begin);
      anchor.ramp = splice_crossfade_frames;
    }
    return anchor;
  }

  [[nodiscard]] Anchor onset_anchor(const Run& run, std::int64_t frame) const noexcept {
    Anchor anchor;
    anchor.kind = AnchorKind::onset;
    anchor.output = output_of(run, frame);
    anchor.source_before = frame;
    anchor.source_after = frame;
    return anchor;
  }

  // The anchors bracketing output centre c: the latest at or before it and
  // the earliest after it, from onsets inside the run and the run's bounds.
  void bracket(std::int64_t centre, Anchor& before, Anchor& after) const noexcept {
    const auto end = static_cast<std::int64_t>(map_->output_frames());
    const auto clamped =
        static_cast<std::uint64_t>(std::clamp<std::int64_t>(to_output(centre), 0, end - 1));
    const auto index = map_->find_segment(clamped);
    const auto run = run_of(index);
    const auto& segments = map_->segments();
    const auto position = source_frame(WarpTimeMap::source_at(segments[index], clamped));
    const auto run_low = source_frame(segments[run.first].source_begin);
    const auto run_high = source_frame(segments[run.last].source_end);
    before = run_start(run);
    after = run_end(run);
    const auto& onsets = settings_.onsets;
    const auto upper = std::ranges::upper_bound(
        onsets, static_cast<std::uint64_t>(std::max<std::int64_t>(position, 0)));
    if (upper != onsets.begin()) {
      const auto onset = static_cast<std::int64_t>(*(upper - 1));
      if (onset > run_low && onset < run_high) {
        const auto anchor = onset_anchor(run, onset);
        if (anchor.output <= centre && anchor.output > before.output)
          before = anchor;
      }
    }
    if (upper != onsets.end()) {
      const auto onset = static_cast<std::int64_t>(*upper);
      if (onset > run_low && onset < run_high) {
        const auto anchor = onset_anchor(run, onset);
        if (anchor.output > centre && anchor.output < after.output)
          after = anchor;
      }
    }
  }

  // Restart (phase-reset) frame for seeking: the first frame at or after the
  // latest anchor at or before frame `first`.
  [[nodiscard]] std::int64_t restart_frame(std::int64_t first) const noexcept {
    Anchor before;
    Anchor after;
    bracket(first * hop_, before, after);
    if (before.kind == AnchorKind::start)
      return floor_div(before.output - static_cast<std::int64_t>(half_), hop_) + 1;
    return std::min(first, -floor_div(-before.output, hop_));
  }

  [[nodiscard]] std::int64_t map_source(std::int64_t intermediate) const noexcept {
    const auto end = static_cast<std::int64_t>(map_->output_frames());
    const auto clamped =
        static_cast<std::uint64_t>(std::clamp<std::int64_t>(to_output(intermediate), 0, end - 1));
    return source_frame(
        WarpTimeMap::source_at(map_->segments()[map_->find_segment(clamped)], clamped));
  }

  // Transient-region time shifting: each anchor's zone of half a window plays
  // at unity, consuming source the map would spread out (or compress). Between
  // zones the map is followed with a correction that falls linearly from the
  // excess at one zone's edge to the deficit at the next, so analysis windows
  // meet an anchor only at their zero-weight edge: no internal mask steps and
  // no repeated attacks, while every anchor and the map between them hold.
  [[nodiscard]] std::int64_t between_anchors(std::int64_t centre, const Anchor& before,
                                             const Anchor& after) const noexcept {
    const auto half = static_cast<std::int64_t>(half_);
    const auto start = before.output + half;
    const auto stop = after.output - half;
    const auto plain = map_source(centre);
    if (stop <= start)
      return plain;
    const auto source_start = before.source_after + half;
    const auto source_stop = after.source_before - half;
    // Slowing a close pair of onsets can leave an output gap even though
    // their source-side unity zones overlap. There is then no source range
    // for the correction to traverse. Follow the map until the next anchor;
    // the frame's keep mask still confines energy to the bracketing run.
    if (source_stop <= source_start)
      return plain;
    const auto excess = source_start - map_source(start);
    const auto deficit = source_stop - map_source(stop);
    const double progress = static_cast<double>(centre - start) / static_cast<double>(stop - start);
    const double correction =
        static_cast<double>(excess) + progress * static_cast<double>(deficit - excess);
    const auto shifted = plain + static_cast<std::int64_t>(std::llround(correction));
    // Never read across the bracketing anchors' unity zones.
    return std::clamp(shifted, source_start, source_stop);
  }

  [[nodiscard]] FramePlan plan(std::int64_t frame) const noexcept {
    FramePlan result;
    const auto centre = frame * hop_;
    const auto half = static_cast<std::int64_t>(half_);
    Anchor before;
    Anchor after;
    bracket(centre, before, after);
    // Frames centred before the clip start or after its end only complete the
    // overlap; they align to that boundary like any other anchor.
    // Alignment: the nearest bracketing anchor within half a window plays at
    // unity speed so its boundary lands exactly on its output frame.
    const auto distance_before = centre - before.output;
    const auto distance_after = after.output - centre;
    const bool align_before = distance_before < half &&
                              (distance_before <= distance_after || after.kind == AnchorKind::none);
    const bool align_after = !align_before && distance_after < half;
    if (align_before)
      result.centre = before.source_after + distance_before;
    else if (align_after)
      result.centre = after.source_before - distance_after;
    else
      result.centre = between_anchors(centre, before, after);
    // Keep only the source between the bracketing anchors, and only output
    // between their output frames.
    result.source_low = before.source_after;
    result.output_low = before.output;
    result.ramp_low = before.ramp;
    result.source_high = after.source_before;
    result.output_high = after.output;
    result.ramp_high = after.ramp;
    result.active = true;
    // Phase resets: the first frame of the start zone, the first frame at or
    // after an onset or splice, and the first frame entering the end zone (its
    // aligned frames hold the final attack, which accumulated rotations would
    // disperse).
    const bool enters_end =
        after.kind == AnchorKind::end && align_after && centre - hop_ <= after.output - half;
    result.epoch =
        enters_end || (before.kind == AnchorKind::start
                           ? centre - hop_ <= before.output - half
                           : (before.kind != AnchorKind::none && centre - before.output < hop_));
    return result;
  }

  [[nodiscard]] bool process_frame(std::int64_t frame) noexcept {
    const auto plan_for = plan(frame);
    const bool epoch = plan_for.epoch || force_epoch_;
    force_epoch_ = false;
    const auto half = static_cast<std::int64_t>(half_);
    const auto first = plan_for.centre - half - hop_;
    if (!source_.ensure(*view_, first, size_ + static_cast<std::size_t>(hop_)))
      return false;
    // Zero-phase frames: sample centre at index 0.
    const float* in_left = source_.left(plan_for.centre - half);
    const float* in_right = source_.right(plan_for.centre - half);
    const float* lag_left = source_.left(plan_for.centre - half - hop_);
    const float* lag_right = source_.right(plan_for.centre - half - hop_);
    for (std::size_t index = 0; index < size_; ++index) {
      const auto at = plan_for.centre - half + static_cast<std::int64_t>(index);
      const float keep = keep_gain(at, plan_for.source_low, plan_for.source_high, plan_for.ramp_low,
                                   plan_for.ramp_high) *
                         window_[index];
      const float keep_lag = keep_gain(at - hop_, plan_for.source_low, plan_for.source_high,
                                       plan_for.ramp_low, plan_for.ramp_high) *
                             window_[index];
      const auto slot = (index + half_) % size_;
      left_[slot] = in_left[index] * keep;
      right_[slot] = in_right[index] * keep;
      lagged_[slot] = (lag_left[index] + lag_right[index]) * keep_lag;
    }
    fft_.forward(left_.data(), left_.data());
    fft_.forward(right_.data(), right_.data());
    fft_.forward(lagged_.data(), lagged_.data());
    lock_phases(epoch);
    if (settings_.shape_formants && pitched_ && settings_.formants > 0.0F)
      shape_formants();
    fft_.inverse(left_.data(), left_.data());
    fft_.inverse(right_.data(), right_.data());
    // Overlap-add with weighted normalisation: each output sample divides by
    // the sum of window products that actually contributed to it.
    const auto centre = frame * hop_;
    const float scale = 1.0F / static_cast<float>(size_);
    for (std::size_t index = 0; index < size_; ++index) {
      const auto at = centre - half + static_cast<std::int64_t>(index);
      const auto source_at = plan_for.centre - half + static_cast<std::int64_t>(index);
      const float output_keep = keep_gain(at, plan_for.output_low, plan_for.output_high,
                                          plan_for.ramp_low, plan_for.ramp_high);
      if (output_keep == 0.0F)
        continue;
      const float source_keep = keep_gain(source_at, plan_for.source_low, plan_for.source_high,
                                          plan_for.ramp_low, plan_for.ramp_high);
      const auto slot_in = (index + half_) % size_;
      const float synthesis = window_[index] * output_keep;
      const auto slot = static_cast<std::size_t>(at) & ring_mask_;
      ring_left_[slot] += synthesis * left_[slot_in] * scale;
      ring_right_[slot] += synthesis * right_[slot_in] * scale;
      ring_weight_[slot] += synthesis * window_[index] * source_keep;
    }
    return true;
  }

  // Identity phase locking (Laroche & Dolson 1999) shared by both channels:
  // peaks advance by their instantaneous frequency over one synthesis hop and
  // every bin in a peak's region takes that peak's rotation.
  void lock_phases(bool epoch) noexcept {
    float* left = left_.data();
    float* right = right_.data();
    const float* lag = lagged_.data();
    const auto power_at = [left, right](std::size_t bin) {
      const auto re_l = left[2 * bin];
      const auto im_l = left[2 * bin + 1];
      const auto re_r = right[2 * bin];
      const auto im_r = right[2 * bin + 1];
      return re_l * re_l + im_l * im_l + re_r * re_r + im_r * im_r;
    };
    float loudest = 0.0F;
    for (std::size_t bin = 1; bin + 1 < bins_; ++bin) {
      power_[bin] = power_at(bin);
      loudest = std::max(loudest, power_[bin]);
    }
    std::size_t peak_count = 0;
    const float floor = loudest * 1e-9F;
    for (std::size_t bin = 2; bin + 2 < bins_; ++bin) {
      const float value = power_[bin];
      if (value > floor && value > power_[bin - 1] && value >= power_[bin + 1] &&
          value > power_[bin - 2] && value >= power_[bin + 2])
        peaks_[peak_count++] = bin;
    }
    const float hop = static_cast<float>(hop_);
    const float bin_frequency = two_pi / static_cast<float>(size_);
    for (std::size_t index = 0; index < peak_count; ++index) {
      const auto bin = peaks_[index];
      float rotation = 0.0F;
      if (!epoch) {
        const float re = left[2 * bin] + right[2 * bin];
        const float im = left[2 * bin + 1] + right[2 * bin + 1];
        const float phase = std::atan2(im, re);
        const float lag_phase = std::atan2(lag[2 * bin + 1], lag[2 * bin]);
        const float expected = bin_frequency * static_cast<float>(bin) * hop;
        const float deviation = wrap_phase(phase - lag_phase - expected);
        const float advance = expected + deviation;
        const float previous_phase =
            std::atan2(previous_mono_[2 * bin + 1], previous_mono_[2 * bin]);
        const float synthesis = previous_phase + rotation_[bin] + advance;
        rotation = wrap_phase(synthesis - phase);
      }
      peak_rotation_[index] = rotation;
      peak_cos_[index] = std::cos(rotation);
      peak_sin_[index] = std::sin(rotation);
    }
    // Regions of influence: bins up to the midpoint between adjacent peaks.
    std::size_t peak = 0;
    for (std::size_t bin = 0; bin < bins_; ++bin) {
      while (peak + 1 < peak_count && bin * 2 >= peaks_[peak] + peaks_[peak + 1])
        ++peak;
      const float rotation = peak_count == 0 ? 0.0F : peak_rotation_[peak];
      const float c = peak_count == 0 ? 1.0F : peak_cos_[peak];
      const float s = peak_count == 0 ? 0.0F : peak_sin_[peak];
      // Keep the unrotated mono spectrum for the next frame's history.
      if (bin > 0 && bin + 1 < bins_) {
        previous_mono_[2 * bin] = left[2 * bin] + right[2 * bin];
        previous_mono_[2 * bin + 1] = left[2 * bin + 1] + right[2 * bin + 1];
      }
      rotation_[bin] = rotation;
      if (rotation == 0.0F || bin == 0 || bin + 1 == bins_)
        continue;
      const float re_l = left[2 * bin];
      const float im_l = left[2 * bin + 1];
      left[2 * bin] = re_l * c - im_l * s;
      left[2 * bin + 1] = re_l * s + im_l * c;
      const float re_r = right[2 * bin];
      const float im_r = right[2 * bin + 1];
      right[2 * bin] = re_r * c - im_r * s;
      right[2 * bin + 1] = re_r * s + im_r * c;
    }
  }

  // Formant preservation: the intermediate spectrum is multiplied by
  // (Env(f p) / Env(f))^amount, so after resampling by p the original
  // envelope sits back in place. Env is the true envelope (Röbel & Rodet
  // 2005): cepstral smoothing iterated on max(log spectrum, previous
  // envelope), which converges onto the harmonic peaks instead of averaging
  // them with the valleys between.
  //
  // Re-enveloping changes loudness by the interval: transposed down, 1/p
  // more partials sit under each formant, so a partial-rich frame gains
  // about 10 log10(1/p) dB (+1.8 dB a fifth down), and loses as much going
  // up. On a mastered mix that rides on the crest-factor growth any phase
  // vocoder shows and drove peaks to twice full scale. Each shaped frame is
  // therefore scaled back to the energy it had before shaping: one positive
  // scalar per frame cannot move the envelope, so formants stay where they
  // are while the level matches the same transposition without formants.
  void shape_formants() noexcept {
    float* cepstrum = cepstrum_.data();
    // Floor the log spectrum 60 dB below its peak: near-silent bins (below
    // the fundamental, between sparse partials) otherwise leave deep holes
    // that smoothing drags into the envelope of the lowest harmonics.
    float peak = -1e30F;
    for (std::size_t bin = 1; bin + 1 < bins_; ++bin) {
      target_[bin] = 0.5F * std::log(power_[bin] + 1e-20F);
      peak = std::max(peak, target_[bin]);
    }
    const float floor = peak - envelope_floor;
    for (std::size_t bin = 1; bin + 1 < bins_; ++bin)
      target_[bin] = std::max(target_[bin], floor);
    target_[0] = target_[1];
    target_[bins_ - 1] = target_[bins_ - 2];
    // The envelope is far smoother than the bin spacing, so it is estimated
    // on a 4x max-pooled spectrum (pooling keeps the peaks the true envelope
    // rides on) with quarter-size transforms; the lifter stays far below the
    // resulting quefrency alias at size / 4.
    const std::size_t coarse_bins = coarse_target_.size();
    for (std::size_t coarse = 0; coarse < coarse_bins; ++coarse) {
      const std::size_t low =
          coarse * envelope_decimation >= 2 ? coarse * envelope_decimation - 2 : 0;
      const std::size_t high = std::min(coarse * envelope_decimation + 2, bins_ - 1);
      float pooled = target_[low];
      for (auto bin = low + 1; bin <= high; ++bin)
        pooled = std::max(pooled, target_[bin]);
      coarse_target_[coarse] = pooled;
    }
    std::ranges::copy(coarse_target_, coarse_envelope_.begin());
    const std::size_t coarse_size = 2 * (coarse_bins - 1);
    const float scale = 1.0F / static_cast<float>(coarse_size);
    for (int iteration = 0; iteration < true_envelope_iterations; ++iteration) {
      // Smooth max(target, envelope) in the cepstral domain.
      cepstrum[0] = std::max(coarse_target_[0], coarse_envelope_[0]);
      cepstrum[1] = std::max(coarse_target_[coarse_bins - 1], coarse_envelope_[coarse_bins - 1]);
      for (std::size_t bin = 1; bin + 1 < coarse_bins; ++bin) {
        cepstrum[2 * bin] = std::max(coarse_target_[bin], coarse_envelope_[bin]);
        cepstrum[2 * bin + 1] = 0.0F;
      }
      envelope_fft_->inverse(cepstrum, cepstrum);
      for (std::size_t index = 0; index < coarse_size; ++index) {
        const bool keep = index < lifter_ || index > coarse_size - lifter_;
        cepstrum[index] = keep ? cepstrum[index] * scale : 0.0F;
      }
      envelope_fft_->forward(cepstrum, cepstrum);
      coarse_envelope_[0] = cepstrum[0];
      coarse_envelope_[coarse_bins - 1] = cepstrum[1];
      for (std::size_t bin = 1; bin + 1 < coarse_bins; ++bin)
        coarse_envelope_[bin] = cepstrum[2 * bin];
    }
    for (std::size_t bin = 0; bin < bins_; ++bin) {
      const auto coarse = bin / envelope_decimation;
      const auto next = std::min(coarse + 1, coarse_bins - 1);
      const float fraction =
          static_cast<float>(bin % envelope_decimation) / static_cast<float>(envelope_decimation);
      envelope_[bin] =
          coarse_envelope_[coarse] + fraction * (coarse_envelope_[next] - coarse_envelope_[coarse]);
    }
    const float amount = settings_.formants / 100.0F;
    const float last = static_cast<float>(bins_ - 1);
    double before = 0.0;
    double after = 0.0;
    for (std::size_t bin = 1; bin + 1 < bins_; ++bin) {
      const float target = std::min(static_cast<float>(bin) * static_cast<float>(pitch_), last);
      const auto below = static_cast<std::size_t>(target);
      const auto above = std::min(below + 1, bins_ - 1);
      const float fraction = target - static_cast<float>(below);
      const float shifted = envelope_[below] + fraction * (envelope_[above] - envelope_[below]);
      const float gain = std::exp(amount * (shifted - envelope_[bin]));
      // The phase-locked spectrum's power, before and after shaping.
      const auto power = static_cast<double>(power_[bin]);
      before += power;
      after += power * gain * gain;
      left_[2 * bin] *= gain;
      left_[2 * bin + 1] *= gain;
      right_[2 * bin] *= gain;
      right_[2 * bin + 1] *= gain;
    }
    // A silent frame has nothing to rescale.
    if (before <= 0.0 || after <= 0.0)
      return;
    const auto level = static_cast<float>(std::sqrt(before / after));
    for (std::size_t index = 0; index < size_; ++index) {
      left_[index] *= level;
      right_[index] *= level;
    }
  }

  void discard(std::int64_t until) noexcept {
    for (; consumed_ < until; ++consumed_) {
      const auto slot = static_cast<std::size_t>(consumed_) & ring_mask_;
      ring_left_[slot] = 0.0F;
      ring_right_[slot] = 0.0F;
      ring_weight_[slot] = 0.0F;
    }
  }

  void emit(std::int64_t first, foundation::StereoSpan<float> out) noexcept {
    const auto left = out.left;
    const auto right = out.right;
    for (std::size_t index = 0; index < left.size(); ++index) {
      const auto slot =
          static_cast<std::size_t>(first + static_cast<std::int64_t>(index)) & ring_mask_;
      const float weight = ring_weight_[slot];
      const bool valid = weight > 1e-6F;
      left[index] = valid ? ring_left_[slot] / weight : 0.0F;
      right[index] = valid ? ring_right_[slot] / weight : 0.0F;
      ring_left_[slot] = 0.0F;
      ring_right_[slot] = 0.0F;
      ring_weight_[slot] = 0.0F;
    }
    consumed_ = first + static_cast<std::int64_t>(left.size());
  }

  StreamLimits limits_;
  Config settings_;
  std::array<std::uint64_t, splice_words> splices_{};
  std::array<std::uint64_t, splice_groups> splice_groups_{};
  std::array<std::uint64_t, splice_roots> splice_roots_{};
  double pitch_;
  bool pitched_;
  detail::FixedRateReader reader_;
  std::int64_t reach_;
  std::size_t size_;
  std::int64_t hop_;
  std::size_t half_;
  std::size_t bins_;
  std::int64_t preroll_;
  RealFft fft_;
  std::vector<float> window_;
  AlignedBuffer left_;
  AlignedBuffer right_;
  AlignedBuffer lagged_;
  std::vector<float> rotation_;
  std::vector<float> previous_mono_;
  std::vector<float> power_;
  std::vector<std::size_t> peaks_;
  std::vector<float> peak_rotation_;
  std::vector<float> peak_cos_;
  std::vector<float> peak_sin_;
  std::size_t ring_mask_;
  std::vector<float> ring_left_;
  std::vector<float> ring_right_;
  std::vector<float> ring_weight_;
  SourceWindow source_;
  AlignedBuffer cepstrum_;
  std::unique_ptr<RealFft> envelope_fft_;
  std::vector<float> coarse_target_;
  std::vector<float> coarse_envelope_;
  std::vector<float> envelope_;
  std::vector<float> target_;
  std::size_t lifter_ = 2;
  std::vector<float> intermediate_left_;
  std::vector<float> intermediate_right_;
  std::int64_t intermediate_base_ = 0;
  std::size_t intermediate_filled_ = 0;
  const WarpTimeMap* map_ = nullptr;
  SourceView* view_ = nullptr;
  std::uint64_t next_ = 0;
  std::int64_t frame_ = 0;
  std::int64_t consumed_ = 0;
  bool force_epoch_ = false;
  StreamStatus status_ = StreamStatus::not_started;
};

foundation::Result<std::unique_ptr<WarpStream>> create(const StreamLimits& limits, QualityTier tier,
                                                       Config config) {
  using Result = foundation::Result<std::unique_ptr<WarpStream>>;
  const auto invalid = [](const char* message) {
    return Result::failure({foundation::ErrorCode::invalid_argument, message});
  };
  if (limits.sample_rate < 8'000 || limits.sample_rate > 384'000 || limits.max_render_frames == 0 ||
      limits.max_render_frames > 65'536)
    return invalid("Complex stream limits are out of range");
  if (!valid_quality_tier(tier))
    return invalid("Complex quality tier is invalid");
  if (config.onsets.size() > max_complex_onsets ||
      std::ranges::adjacent_find(config.onsets, std::ranges::greater_equal{}) !=
          config.onsets.end() ||
      (!config.onsets.empty() &&
       config.onsets.back() > (max_source_position >> source_fraction_bits)))
    return invalid("Complex onsets must be strictly increasing source frames");
  if (config.transpose_cents < -max_transpose_cents || config.transpose_cents > max_transpose_cents)
    return invalid("Complex transpose must be within four octaves");
  if (!std::isfinite(config.formants) || config.formants < 0.0F || config.formants > 100.0F ||
      !std::isfinite(config.envelope) || config.envelope < 8.0F || config.envelope > 256.0F)
    return invalid("Complex Pro formants must be 0 to 100 and envelope 8 to 256");
  try {
    return Result::success(std::make_unique<ComplexStream>(limits, tier, std::move(config)));
  } catch (const std::bad_alloc&) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Complex stream allocation failed"});
  }
}

} // namespace

foundation::Result<std::unique_ptr<WarpStream>>
create_complex_stream(const StreamLimits& limits, QualityTier tier, ComplexSettings settings) {
  return create(limits, tier,
                {std::move(settings.onsets), settings.transpose_cents, false, 0.0F, 128.0F});
}

foundation::Result<std::unique_ptr<WarpStream>>
create_complex_pro_stream(const StreamLimits& limits, QualityTier tier,
                          ComplexProSettings settings) {
  return create(limits, tier,
                {std::move(settings.onsets), settings.transpose_cents, true, settings.formants,
                 settings.envelope});
}

} // namespace oscillate::warp_dsp
