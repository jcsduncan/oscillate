#include "../core/bandlimited_kernel.hpp"
#include "../core/bandlimited_read.hpp"
#include "../core/source_window.hpp"
#include "oscillate/foundation/stereo_span.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <numbers>

namespace oscillate::warp_dsp {
namespace {

using detail::KernelDesign;
using detail::SourceWindow;

// A splice-free run of segments, in output and source frames.
struct Run {
  std::size_t first = 0;
  std::size_t last = 0;
  std::int64_t output_begin = 0;
  std::int64_t output_end = 0;
  std::int64_t source_low = 0;
  std::int64_t source_high = 0;
  bool splice_before = false;
  bool splice_after = false;
};

} // namespace

struct TonesSeekIndex::Impl {
  struct Checkpoint {
    double position = 0.0;
  };
  struct Entry {
    Run run;
    std::vector<Checkpoint> checkpoints;
  };
  std::shared_ptr<const std::byte> map_identity;
  StreamLimits limits;
  QualityTier tier = QualityTier::q0;
  TonesSettings settings;
  std::vector<Entry> runs;
  std::size_t grain_count = 0;
};

const TonesSeekIndex::Impl& TonesSeekIndex::impl() const noexcept { return *impl_; }

TonesSeekIndex::Statistics TonesSeekIndex::statistics() const noexcept {
  Statistics stats;
  stats.runs = impl_->runs.size();
  stats.grains = impl_->grain_count;
  stats.estimated_bytes = sizeof(TonesSeekIndex) + sizeof(Impl) +
                          impl_->runs.capacity() * sizeof(Impl::Entry) +
                          impl_->settings.marks.capacity() * sizeof(analysis::PitchMark);
  for (const auto& entry : impl_->runs) {
    stats.checkpoints += entry.checkpoints.size();
    stats.estimated_bytes += entry.checkpoints.capacity() * sizeof(Impl::Checkpoint);
  }
  return stats;
}

namespace {

constexpr std::size_t tones_checkpoint_interval = 256;
constexpr std::size_t max_tones_checkpoints = 1U << 20;

[[nodiscard]] Run run_containing(const WarpTimeMap& map, std::size_t index) noexcept {
  const auto segments = map.segments();
  Run run{index, index};
  while (run.first > 0 && !segments[run.first].splice)
    --run.first;
  while (run.last + 1 < segments.size() && !segments[run.last + 1].splice)
    ++run.last;
  run.output_begin = static_cast<std::int64_t>(segments[run.first].output_begin);
  run.output_end = static_cast<std::int64_t>(segments[run.last].output_end);
  run.source_low =
      static_cast<std::int64_t>(segments[run.first].source_begin >> source_fraction_bits);
  run.source_high = static_cast<std::int64_t>((segments[run.last].source_end + source_unit - 1) >>
                                              source_fraction_bits);
  run.splice_before = run.first > 0;
  run.splice_after = run.last + 1 < segments.size();
  return run;
}

[[nodiscard]] std::size_t segment_at(const WarpTimeMap& map, std::int64_t output) noexcept {
  const auto end = static_cast<std::int64_t>(map.output_frames());
  return map.find_segment(static_cast<std::uint64_t>(std::clamp<std::int64_t>(output, 0, end - 1)));
}

[[nodiscard]] std::uint64_t source_at_output(const WarpTimeMap& map, std::int64_t output) noexcept {
  const auto end = static_cast<std::int64_t>(map.output_frames());
  const auto clamped = static_cast<std::uint64_t>(std::clamp<std::int64_t>(output, 0, end - 1));
  return WarpTimeMap::source_at(map.segments()[map.find_segment(clamped)], clamped);
}

struct Grain {
  std::int64_t centre = 0;  // output frame
  std::int64_t half = 0;    // half length in frames
  std::uint64_t source = 0; // Q48.16 source position read at the centre
  // Correlation with the previous grain (1 = seamless continuation, 0 =
  // unrelated content); selects amplitude or power normalisation.
  float coherence = 1.0F;
  Run run;
};

// Coherence of a grain whose read position departs from a seamless
// continuation of its predecessor by `departure` source frames.
[[nodiscard]] float coherence_of(double departure, double correlation_frames) noexcept {
  return static_cast<float>(std::exp(-std::fabs(departure) / correlation_frames));
}

// Produces grains in increasing centre order.
class GrainGenerator {
public:
  GrainGenerator() = default;
  GrainGenerator(const GrainGenerator&) = delete;
  GrainGenerator& operator=(const GrainGenerator&) = delete;
  GrainGenerator(GrainGenerator&&) = delete;
  GrainGenerator& operator=(GrainGenerator&&) = delete;
  virtual ~GrainGenerator() = default;
  // Positions the sequence so the next grain is the first that can reach
  // output frame `output`. Must reproduce exactly the grains a continuous
  // sequence would produce there.
  virtual void seek(const WarpTimeMap& map, SourceView& source, std::int64_t output) noexcept = 0;
  [[nodiscard]] virtual StreamStatus preparation_status(const WarpTimeMap&) const noexcept {
    return StreamStatus::ok;
  }
  [[nodiscard]] virtual bool next(Grain& grain) noexcept = 0;
  [[nodiscard]] virtual bool source_failed() const noexcept { return false; }
};

// Raised-cosine grain window w(x) = 0.5 + 0.5 cos(pi x), x in [-1, 1],
// tabulated for linear interpolation.
class GrainWindow {
public:
  static constexpr std::size_t resolution = 4'096;
  GrainWindow() : table_(resolution + 2) {
    for (std::size_t index = 0; index < table_.size(); ++index)
      table_[index] = static_cast<float>(
          0.5 + 0.5 * std::cos(std::numbers::pi * static_cast<double>(index) / resolution));
  }
  // Table steps per frame for a grain of half-length `half`.
  [[nodiscard]] static double step(std::int64_t half) noexcept {
    return static_cast<double>(resolution) / static_cast<double>(half);
  }
  // Weight at |offset| frames from the centre, given step(half).
  [[nodiscard]] float at(std::int64_t offset, double step) const noexcept {
    const double position = static_cast<double>(offset < 0 ? -offset : offset) * step;
    const auto index = static_cast<std::size_t>(position);
    if (index >= resolution)
      return 0.0F;
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    return table_[index] + fraction * (table_[index + 1] - table_[index]);
  }

private:
  std::vector<float> table_;
};

// Splice crossfade half-width at run boundaries (output frames).
constexpr std::int64_t splice_ramp = splice_crossfade_frames;

[[nodiscard]] float output_keep(const Run& run, std::int64_t output) noexcept {
  const auto low = run.output_begin - (run.splice_before ? splice_ramp : 0);
  const auto high = run.output_end + (run.splice_after ? splice_ramp : 0);
  if (output < low || output >= high)
    return 0.0F;
  if (run.splice_before && output < run.output_begin + splice_ramp)
    return static_cast<float>(output - low + 1) / static_cast<float>(2 * splice_ramp + 1);
  if (run.splice_after && output >= run.output_end - splice_ramp)
    return static_cast<float>(high - output) / static_cast<float>(2 * splice_ramp + 1);
  return 1.0F;
}

// Overlap-add of windowed grains, normalised by the window weight that
// actually contributed (content outside a grain's run counts as absent).
class GrainStream final : public WarpStream {
public:
  GrainStream(const StreamLimits& limits, const KernelDesign& design, double speed,
              std::int64_t max_half, std::unique_ptr<GrainGenerator> generator)
      : limits_(limits), reader_(design, speed), reach_(reader_.reach()), speed_(speed),
        speed_step_(
            static_cast<std::uint64_t>(std::llround(speed * static_cast<double>(source_unit)))),
        max_half_(max_half), generator_(std::move(generator)),
        ring_mask_(ring_size(static_cast<std::size_t>(4 * max_half_) + std::size_t{2} * 65'536) -
                   1),
        ring_left_(ring_mask_ + 1, 0.0F), ring_right_(ring_mask_ + 1, 0.0F),
        ring_weight_(ring_mask_ + 1, 0.0F), ring_power_(ring_mask_ + 1, 0.0F),
        ring_coherence_(ring_mask_ + 1, 0.0F),
        source_(static_cast<std::size_t>(
                    std::ceil(2.0 * static_cast<double>(max_half_) * std::max(speed, 1.0))) +
                2 * static_cast<std::size_t>(reach_) + 8) {}

  StreamStatus start(const WarpTimeMap& map, SourceView& source,
                     std::uint64_t output_frame) noexcept override {
    map_ = nullptr;
    view_ = nullptr;
    source_.reset();
    if (output_frame >= map.output_frames())
      return status_ = StreamStatus::out_of_range;
    if (const auto preparation = generator_->preparation_status(map);
        preparation != StreamStatus::ok)
      return status_ = preparation;
    map_ = &map;
    view_ = &source;
    next_ = output_frame;
    std::ranges::fill(ring_left_, 0.0F);
    std::ranges::fill(ring_right_, 0.0F);
    std::ranges::fill(ring_weight_, 0.0F);
    std::ranges::fill(ring_power_, 0.0F);
    std::ranges::fill(ring_coherence_, 0.0F);
    generator_->seek(map, source, static_cast<std::int64_t>(output_frame));
    has_pending_ = generator_->next(pending_);
    if (generator_->source_failed())
      return status_ = StreamStatus::source_failed;
    // Every grain the generator can return from here starts after this.
    consumed_ = static_cast<std::int64_t>(output_frame) - 4 * max_half_ - 2 * splice_ramp;
    return status_ = StreamStatus::ok;
  }

  std::size_t render(std::span<float> left, std::span<float> right) noexcept override {
    if (status_ != StreamStatus::ok || left.size() != right.size())
      return 0;
    const auto requested = std::min<std::size_t>(left.size(), limits_.max_render_frames);
    const auto end = static_cast<std::int64_t>(map_->output_frames());
    std::size_t written = 0;
    while (written < requested && static_cast<std::int64_t>(next_) < end) {
      const auto want = static_cast<std::int64_t>(next_);
      // Output before the next grain's earliest reach is final.
      const auto ready =
          has_pending_ ? pending_.centre - max_half_ : std::numeric_limits<std::int64_t>::max();
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
      if (!synthesise(pending_)) {
        status_ = StreamStatus::source_failed;
        break;
      }
      has_pending_ = generator_->next(pending_);
      if (generator_->source_failed()) {
        status_ = StreamStatus::source_failed;
        break;
      }
    }
    return written;
  }

  std::uint64_t next_output_frame() const noexcept override { return next_; }
  StreamStatus status() const noexcept override { return status_; }

private:
  static std::size_t ring_size(std::size_t size) noexcept {
    std::size_t ring = 1;
    while (ring < size)
      ring <<= 1;
    return ring;
  }

  [[nodiscard]] bool synthesise(const Grain& grain) noexcept {
    const auto half = grain.half;
    if (half <= 0)
      return true;
    const auto offset_first =
        static_cast<std::int64_t>(grain.source >> source_fraction_bits) -
        static_cast<std::int64_t>(std::ceil(static_cast<double>(half) * speed_)) - reach_ - 1;
    const auto count = static_cast<std::size_t>(
        2 * static_cast<std::int64_t>(std::ceil(static_cast<double>(half) * speed_)) + 2 * reach_ +
        4);
    if (!source_.ensure(*view_, offset_first, count))
      return false;
    const double window_step = GrainWindow::step(half);
    for (std::int64_t index = -half; index < half; ++index) {
      const auto output = grain.centre + index;
      const float keep = output_keep(grain.run, output);
      if (keep == 0.0F)
        continue;
      // Grain position relative to the centre, read at the stream's speed.
      const auto delta = static_cast<std::int64_t>(speed_step_) * index;
      const auto position = static_cast<std::int64_t>(grain.source) + delta;
      if (position < 0)
        continue;
      const auto frame = position >> source_fraction_bits;
      if (frame < grain.run.source_low || frame >= grain.run.source_high)
        continue;
      const float window = window_.at(index, window_step);
      const float weight = keep * window;
      if (weight == 0.0F)
        continue;
      const auto sample = reader_.read(source_, static_cast<std::uint64_t>(position));
      const auto slot = static_cast<std::size_t>(output) & ring_mask_;
      ring_left_[slot] += weight * sample.left;
      ring_right_[slot] += weight * sample.right;
      ring_weight_[slot] += weight;
      ring_power_[slot] += weight * weight;
      ring_coherence_[slot] += weight * grain.coherence;
    }
    return true;
  }

  void discard(std::int64_t until) noexcept {
    for (; consumed_ < until; ++consumed_) {
      const auto slot = static_cast<std::size_t>(consumed_) & ring_mask_;
      ring_left_[slot] = 0.0F;
      ring_right_[slot] = 0.0F;
      ring_weight_[slot] = 0.0F;
      ring_power_[slot] = 0.0F;
      ring_coherence_[slot] = 0.0F;
    }
  }

  void emit(std::int64_t first, foundation::StereoSpan<float> out) noexcept {
    const auto left = out.left;
    const auto right = out.right;
    for (std::size_t index = 0; index < left.size(); ++index) {
      const auto slot =
          static_cast<std::size_t>(first + static_cast<std::int64_t>(index)) & ring_mask_;
      // Coherent grains add in amplitude, unrelated ones in power: divide by
      // sqrt(sum w^2 + coherence * ((sum w)^2 - sum w^2)).
      const float weight = ring_weight_[slot];
      const bool valid = weight > 1e-4F;
      const float power = ring_power_[slot];
      const float coherence = valid ? ring_coherence_[slot] / weight : 0.0F;
      const float norm = std::sqrt(power + coherence * (weight * weight - power));
      left[index] = valid ? ring_left_[slot] / norm : 0.0F;
      right[index] = valid ? ring_right_[slot] / norm : 0.0F;
      ring_left_[slot] = 0.0F;
      ring_right_[slot] = 0.0F;
      ring_weight_[slot] = 0.0F;
      ring_power_[slot] = 0.0F;
      ring_coherence_[slot] = 0.0F;
    }
    consumed_ = first + static_cast<std::int64_t>(left.size());
  }

  StreamLimits limits_;
  detail::FixedRateReader reader_;
  std::int64_t reach_;
  double speed_;
  std::uint64_t speed_step_;
  std::int64_t max_half_;
  std::unique_ptr<GrainGenerator> generator_;
  std::size_t ring_mask_;
  std::vector<float> ring_left_;
  std::vector<float> ring_right_;
  GrainWindow window_;
  std::vector<float> ring_weight_;
  std::vector<float> ring_power_;
  std::vector<float> ring_coherence_;
  SourceWindow source_;
  const WarpTimeMap* map_ = nullptr;
  SourceView* view_ = nullptr;
  std::uint64_t next_ = 0;
  std::int64_t consumed_ = 0;
  Grain pending_;
  bool has_pending_ = false;
  StreamStatus status_ = StreamStatus::not_started;
};

// TD-PSOLA marks laid out from each run's start.
class TonesGenerator final : public GrainGenerator {
public:
  TonesGenerator(std::vector<analysis::PitchMark> marks, double pitch, std::int64_t unvoiced_half,
                 double correlation_frames, std::shared_ptr<const TonesSeekIndex> index = {})
      : marks_(std::move(marks)), pitch_(pitch), unvoiced_half_(unvoiced_half),
        correlation_frames_(correlation_frames), index_(std::move(index)) {
    double longest = static_cast<double>(unvoiced_half_);
    for (const auto& mark : marks_)
      longest = std::max(longest, static_cast<double>(mark.period) * std::max(1.0, 1.0 / pitch_));
    max_half_ = static_cast<std::int64_t>(std::ceil(longest)) + 1;
  }

  [[nodiscard]] StreamStatus preparation_status(const WarpTimeMap& map) const noexcept override {
    return index_ && index_->impl().map_identity.get() != map.identity().get()
               ? StreamStatus::unprepared
               : StreamStatus::ok;
  }

  // Only the off-thread index builder uses this direct run initialization.
  void prime(const WarpTimeMap& map, const Run& run, std::size_t run_index = 0) noexcept {
    map_ = &map;
    run_ = run;
    run_index_ = run_index;
    position_ = static_cast<double>(run.output_begin);
  }
  [[nodiscard]] double position() const noexcept { return position_; }

  [[nodiscard]] std::int64_t max_half() const noexcept { return max_half_; }

  void seek(const WarpTimeMap& map, SourceView& /*source*/, std::int64_t output) noexcept override {
    map_ = &map;
    const auto bound = max_half();
    const auto target = output - 2 * bound - splice_ramp;
    std::size_t selected_run = 0;
    Run run;
    if (index_) {
      const auto& runs = index_->impl().runs;
      const auto found = std::ranges::upper_bound(
          runs, target, {}, [](const auto& entry) { return entry.run.output_begin; });
      selected_run = found == runs.begin() ? 0 : static_cast<std::size_t>(found - runs.begin() - 1);
      run = runs[selected_run].run;
    } else {
      run = run_containing(map, segment_at(map, target));
    }
    // Grains of the previous run reach across its end by up to a grain plus
    // the splice crossfade.
    if (run.splice_before && target < run.output_begin + bound + splice_ramp) {
      if (index_)
        run = index_->impl().runs[--selected_run].run;
      else
        run = run_containing(map, run.first - 1);
    }
    prime(map, run, selected_run);
    if (index_) {
      const auto& checkpoints = index_->impl().runs[selected_run].checkpoints;
      const auto found = std::ranges::upper_bound(checkpoints, target, {},
                                                  &TonesSeekIndex::Impl::Checkpoint::position);
      if (found != checkpoints.begin())
        position_ = (found - 1)->position;
    }
    // Replay no more than one checkpoint interval, plus the bounded overlap.
    Grain grain;
    while (true) {
      const auto saved_position = position_;
      const auto saved_run = run_;
      const auto saved_run_index = run_index_;
      if (!next(grain))
        return;
      if (grain.centre + grain.half > output - splice_ramp) {
        position_ = saved_position;
        run_ = saved_run;
        run_index_ = saved_run_index;
        return;
      }
    }
  }

  bool next(Grain& grain) noexcept override {
    if (map_ == nullptr)
      return false;
    if (position_ >= static_cast<double>(run_.output_end)) {
      if (!run_.splice_after)
        return false;
      if (index_)
        run_ = index_->impl().runs[++run_index_].run;
      else
        run_ = run_containing(*map_, run_.last + 1);
      position_ = static_cast<double>(run_.output_begin);
    }
    const auto centre = static_cast<std::int64_t>(std::floor(position_));
    const auto source = source_at_output(*map_, centre);
    const auto frame = static_cast<std::int64_t>(source >> source_fraction_bits);
    grain.centre = centre;
    grain.run = run_;
    const auto* mark = nearest_mark(frame);
    if (mark != nullptr) {
      const double period = mark->period;
      const double spacing = period / pitch_;
      grain.source = static_cast<std::uint64_t>(mark->frame) << source_fraction_bits;
      grain.half = static_cast<std::int64_t>(std::lround(std::max(period, spacing)));
      position_ += spacing;
      grain.coherence = 1.0F;
    } else {
      // Whole-frame grain positions read as exact copies.
      grain.source = (source + (source_unit >> 1)) & ~(source_unit - 1);
      grain.half = unvoiced_half_;
      // Unvoiced grains continue seamlessly only when the map advances one
      // grain hop of source per hop of output.
      const auto previous = source_at_output(*map_, centre - unvoiced_half_);
      grain.coherence =
          coherence_of((static_cast<double>(source) - static_cast<double>(previous)) / source_unit -
                           static_cast<double>(unvoiced_half_),
                       correlation_frames_);
      position_ += static_cast<double>(unvoiced_half_);
    }
    return true;
  }

private:
  // The source mark nearest `frame`, if it is voiced there and in this run.
  [[nodiscard]] const analysis::PitchMark* nearest_mark(std::int64_t frame) const noexcept {
    if (marks_.empty())
      return nullptr;
    const auto upper = std::ranges::upper_bound(
        marks_, static_cast<std::uint64_t>(std::max<std::int64_t>(frame, 0)), {},
        &analysis::PitchMark::frame);
    const analysis::PitchMark* best = nullptr;
    double distance = std::numeric_limits<double>::max();
    for (const auto* candidate : {upper == marks_.begin() ? nullptr : &*(upper - 1),
                                  upper == marks_.end() ? nullptr : &*upper}) {
      if (candidate == nullptr)
        continue;
      const double gap =
          std::fabs(static_cast<double>(candidate->frame) - static_cast<double>(frame));
      if (gap < distance) {
        distance = gap;
        best = candidate;
      }
    }
    if (best == nullptr || distance > 0.75 * best->period ||
        static_cast<std::int64_t>(best->frame) < run_.source_low ||
        static_cast<std::int64_t>(best->frame) >= run_.source_high)
      return nullptr;
    return best;
  }

  std::vector<analysis::PitchMark> marks_;
  double pitch_;
  std::int64_t unvoiced_half_;
  double correlation_frames_;
  const WarpTimeMap* map_ = nullptr;
  Run run_;
  double position_ = 0.0;
  std::shared_ptr<const TonesSeekIndex> index_;
  std::size_t run_index_ = 0;
  std::int64_t max_half_ = 0;
};

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

// Fixed-grid granular grains. Fluctuation jitters each grain's source
// position (keyed on the grain index); WSOLA alignment (Verhelst & Roelands
// 1993) then shifts it by up to 10 ms to best match the waveform
// its predecessor would continue into, so sustained tones keep their pitch
// instead of drifting by the grain rate. The chain restarts at each run, and
// a seek pre-rolls a bounded number of grains.
class TextureGenerator final : public GrainGenerator {
public:
  TextureGenerator(std::int64_t half, double flux, std::uint64_t seed, double pitch,
                   std::uint32_t sample_rate)
      : half_(half), hop_(std::max<std::int64_t>(1, half / 2)), flux_(flux), seed_(seed),
        // Half a period of a 50 Hz fundamental, so any tone can be aligned.
        pitch_(pitch), search_(std::lround(0.01 * sample_rate)),
        compare_(std::min<std::int64_t>(2 * half, std::lround(0.01 * sample_rate))),
        preroll_grains_(std::lround(2.0 * sample_rate / static_cast<double>(hop_)) + 1),
        left_(static_cast<std::size_t>(compare_ + 2 * search_)), right_(left_.size()),
        candidate_(left_.size()), model_(static_cast<std::size_t>(compare_)),
        transform_size_(transform_size(left_.size())), fft_(transform_size_),
        candidate_spectrum_(transform_size_), model_spectrum_(transform_size_) {}

  void seek(const WarpTimeMap& map, SourceView& source, std::int64_t output) noexcept override {
    map_ = &map;
    view_ = &source;
    source_failed_ = false;
    has_run_ = false;
    // First grain whose window reaches `output`, reaching back over a splice
    // crossfade.
    const auto earliest = output - half_ - splice_ramp;
    const auto target = earliest >= 0 ? earliest / hop_ : -((-earliest + hop_ - 1) / hop_);
    // Restart the alignment chain at the grain's run start, or a bounded
    // pre-roll before it, and replay up to the target.
    const auto run = run_containing(map, segment_at(map, target * hop_));
    const auto run_first =
        run.output_begin >= 0 ? run.output_begin / hop_ : -((-run.output_begin + hop_ - 1) / hop_);
    index_ = std::max(run_first, target - preroll_grains_);
    chained_ = false;
    Grain grain;
    while (index_ < target && next(grain)) {
    }
  }

  bool next(Grain& grain) noexcept override {
    if (map_ == nullptr)
      return false;
    if (source_failed_)
      return false;
    const auto centre = index_ * hop_;
    const auto end = static_cast<std::int64_t>(map_->output_frames());
    if (centre - half_ >= end)
      return false;
    grain.centre = centre;
    grain.half = half_;
    if (!has_run_ || centre < run_.output_begin || centre >= run_.output_end) {
      run_ = run_containing(*map_, segment_at(*map_, centre));
      has_run_ = true;
    }
    grain.run = run_;
    const auto nominal = jittered(index_);
    grain.source = nominal;
    grain.coherence = 0.0F;
    const bool continues = chained_ && previous_run_first_ == grain.run.first;
    if (continues) {
      // Where the predecessor's read would be by this grain's centre.
      const auto continuation =
          static_cast<std::int64_t>(previous_source_) +
          static_cast<std::int64_t>(std::llround(static_cast<double>(hop_) * pitch_ * source_unit));
      float correlation = 0.0F;
      std::int64_t shift = 0;
      if (!align(nominal, continuation, shift, correlation)) {
        source_failed_ = true;
        return false;
      }
      grain.source = static_cast<std::uint64_t>(std::max<std::int64_t>(
          static_cast<std::int64_t>(nominal) + shift * static_cast<std::int64_t>(source_unit), 0));
      grain.coherence = std::clamp(correlation, 0.0F, 1.0F);
    }
    // At unity pitch whole-frame positions make every grain read an exact
    // copy; granular timing does not need sub-sample placement.
    if (pitch_ == 1.0)
      grain.source = (grain.source + (source_unit >> 1)) & ~(source_unit - 1);
    previous_source_ = grain.source;
    previous_run_first_ = grain.run.first;
    chained_ = true;
    ++index_;
    return true;
  }

  [[nodiscard]] bool source_failed() const noexcept override { return source_failed_; }

private:
  [[nodiscard]] static std::size_t transform_size(std::size_t span) noexcept {
    std::size_t size = 32;
    while (size < span)
      size <<= 1;
    return size;
  }

  // Jittered nominal source position (Q48.16) of grain `index`.
  [[nodiscard]] std::uint64_t jittered(std::int64_t index) const noexcept {
    const auto source = static_cast<std::int64_t>(source_at_output(*map_, index * hop_));
    const auto bits = mix(seed_ ^ static_cast<std::uint64_t>(index));
    const double unit = static_cast<double>(bits >> 11) / 4503599627370496.0 - 1.0;
    const auto jitter = static_cast<std::int64_t>(
        std::llround(flux_ * unit * static_cast<double>(half_) * static_cast<double>(source_unit)));
    return static_cast<std::uint64_t>(std::max<std::int64_t>(source + jitter, 0));
  }

  // Shift in [-search, search] frames maximising the normalised correlation
  // between the source around `nominal + shift` and around `continuation`.
  [[nodiscard]] bool align(std::uint64_t nominal, std::int64_t continuation,
                           std::int64_t& shift_result, float& correlation) noexcept {
    const auto model_first = (continuation >> source_fraction_bits) - compare_ / 2;
    const auto candidate_first =
        static_cast<std::int64_t>(nominal >> source_fraction_bits) - compare_ / 2 - search_;
    const auto model_count = static_cast<std::size_t>(compare_);
    if (!view_->read(model_first, std::span(left_).first(model_count),
                     std::span(right_).first(model_count)))
      return false;
    double model_energy = 0.0;
    for (std::size_t index = 0; index < model_count; ++index) {
      model_[index] = left_[index] + right_[index];
      model_energy += static_cast<double>(model_[index]) * model_[index];
    }
    if (!view_->read(candidate_first, left_, right_))
      return false;
    for (std::size_t index = 0; index < candidate_.size(); ++index)
      candidate_[index] = left_[index] + right_[index];
    if (model_energy < 1e-12)
      return true;
    // Cross-correlation for every shift at once: candidate * conj(model).
    std::ranges::fill(std::span(candidate_spectrum_.data(), transform_size_), 0.0F);
    std::ranges::fill(std::span(model_spectrum_.data(), transform_size_), 0.0F);
    std::ranges::copy(candidate_, candidate_spectrum_.data());
    std::ranges::copy(model_, model_spectrum_.data());
    fft_.forward(candidate_spectrum_.data(), candidate_spectrum_.data());
    fft_.forward(model_spectrum_.data(), model_spectrum_.data());
    float* product = candidate_spectrum_.data();
    const float* model = model_spectrum_.data();
    product[0] *= model[0];
    product[1] *= model[1];
    for (std::size_t bin = 1; bin < transform_size_ / 2; ++bin) {
      const float re = product[2 * bin];
      const float im = product[2 * bin + 1];
      const float model_re = model[2 * bin];
      const float model_im = -model[2 * bin + 1];
      product[2 * bin] = re * model_re - im * model_im;
      product[2 * bin + 1] = re * model_im + im * model_re;
    }
    fft_.inverse(product, product);
    const double scale = 1.0 / static_cast<double>(transform_size_);
    double best = -2.0;
    std::int64_t best_shift = 0;
    double window_energy = 0.0;
    for (std::size_t index = 0; index < model_count; ++index)
      window_energy += static_cast<double>(candidate_[index]) * candidate_[index];
    for (std::int64_t shift = -search_; shift <= search_; ++shift) {
      const auto offset = static_cast<std::size_t>(shift + search_);
      if (shift > -search_) {
        const auto leaving = candidate_[offset - 1];
        const auto entering = candidate_[offset + model_count - 1];
        window_energy +=
            static_cast<double>(entering) * entering - static_cast<double>(leaving) * leaving;
      }
      const double cross = static_cast<double>(product[offset]) * scale;
      const double score = cross / std::sqrt(model_energy * std::max(window_energy, 1e-12));
      // Ties prefer the smallest shift, keeping the nominal position.
      if (score > best + 1e-9 ||
          (std::fabs(score - best) <= 1e-9 && std::abs(shift) < std::abs(best_shift))) {
        best = score;
        best_shift = shift;
      }
    }
    correlation = static_cast<float>(best);
    shift_result = best_shift;
    return true;
  }

  std::int64_t half_;
  std::int64_t hop_;
  double flux_;
  std::uint64_t seed_;
  double pitch_;
  std::int64_t search_;
  std::int64_t compare_;
  std::int64_t preroll_grains_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::vector<float> candidate_;
  std::vector<float> model_;
  std::size_t transform_size_;
  RealFft fft_;
  AlignedBuffer candidate_spectrum_;
  AlignedBuffer model_spectrum_;
  const WarpTimeMap* map_ = nullptr;
  SourceView* view_ = nullptr;
  Run run_;
  bool has_run_ = false;
  bool source_failed_ = false;
  std::int64_t index_ = 0;
  bool chained_ = false;
  std::uint64_t previous_source_ = 0;
  std::size_t previous_run_first_ = 0;
};

[[nodiscard]] bool valid_limits(const StreamLimits& limits) noexcept {
  return limits.sample_rate >= 8'000 && limits.sample_rate <= 384'000 &&
         limits.max_render_frames > 0 && limits.max_render_frames <= 65'536;
}

using StreamResult = foundation::Result<std::unique_ptr<WarpStream>>;

// Decorrelation length for grain coherence: 2 ms.
[[nodiscard]] double correlation_length(const StreamLimits& limits) noexcept {
  return 0.002 * limits.sample_rate;
}

[[nodiscard]] StreamResult invalid(const char* message) {
  return StreamResult::failure({foundation::ErrorCode::invalid_argument, message});
}

[[nodiscard]] bool valid_tones_settings(const StreamLimits& limits, QualityTier tier,
                                        const TonesSettings& settings) noexcept {
  return valid_limits(limits) && valid_quality_tier(tier) &&
         std::isfinite(settings.grain_size_ms) && settings.grain_size_ms >= 5.0F &&
         settings.grain_size_ms <= 200.0F && settings.transpose_cents >= -max_transpose_cents &&
         settings.transpose_cents <= max_transpose_cents &&
         settings.marks.size() <= analysis::max_pitch_marks &&
         std::ranges::adjacent_find(settings.marks, std::ranges::greater_equal{},
                                    &analysis::PitchMark::frame) == settings.marks.end() &&
         std::ranges::none_of(settings.marks, [](const auto& mark) {
           return mark.frame >= max_source_position / source_unit || !std::isfinite(mark.period) ||
                  mark.period < 2.0F || mark.period > 4'096.0F;
         });
}

[[nodiscard]] bool matches(const TonesSeekIndex::Impl& index, const StreamLimits& limits,
                           QualityTier tier, const TonesSettings& settings) noexcept {
  if (index.tier != tier || index.limits.sample_rate != limits.sample_rate ||
      index.limits.max_render_frames != limits.max_render_frames ||
      index.settings.transpose_cents != settings.transpose_cents ||
      std::bit_cast<std::uint32_t>(index.settings.grain_size_ms) !=
          std::bit_cast<std::uint32_t>(settings.grain_size_ms) ||
      index.settings.marks.size() != settings.marks.size())
    return false;
  return std::ranges::equal(index.settings.marks, settings.marks, {}, &analysis::PitchMark::frame,
                            &analysis::PitchMark::frame) &&
         std::equal(index.settings.marks.begin(), index.settings.marks.end(),
                    settings.marks.begin(), [](const auto& left, const auto& right) {
                      return std::bit_cast<std::uint32_t>(left.period) ==
                             std::bit_cast<std::uint32_t>(right.period);
                    });
}

[[nodiscard]] StreamResult make_tones_stream(const StreamLimits& limits, QualityTier tier,
                                             TonesSettings settings,
                                             std::shared_ptr<const TonesSeekIndex> index) {
  if (!valid_tones_settings(limits, tier, settings))
    return invalid("Tones settings, quality tier or limits are out of range");
  if (index && !matches(index->impl(), limits, tier, settings))
    return invalid("Tones seek index does not match stream settings");
  try {
    const double pitch = std::exp2(settings.transpose_cents / 1200.0);
    const auto unvoiced = std::max<std::int64_t>(
        8, std::llround(static_cast<double>(settings.grain_size_ms) * 0.0005 * limits.sample_rate));
    auto generator = std::make_unique<TonesGenerator>(std::move(settings.marks), pitch, unvoiced,
                                                      correlation_length(limits), std::move(index));
    const auto bound = generator->max_half();
    return StreamResult::success(
        std::make_unique<GrainStream>(limits, detail::BandlimitedKernel::for_tier(tier)->design(),
                                      1.0, bound, std::move(generator)));
  } catch (const std::bad_alloc&) {
    return StreamResult::failure(
        {foundation::ErrorCode::resource_exhausted, "Tones stream allocation failed"});
  }
}

} // namespace

foundation::Result<std::shared_ptr<const TonesSeekIndex>>
prepare_tones_seek_index(const WarpTimeMap& map, const StreamLimits& limits, QualityTier tier,
                         const TonesSettings& settings) {
  if (!valid_tones_settings(limits, tier, settings))
    return foundation::Result<std::shared_ptr<const TonesSeekIndex>>::failure(
        {foundation::ErrorCode::invalid_argument, "Tones index settings or limits are invalid"});
  try {
    auto data = std::make_shared<TonesSeekIndex::Impl>();
    data->map_identity = map.identity();
    data->limits = limits;
    data->tier = tier;
    data->settings = settings;
    const auto segments = map.segments();
    data->runs.reserve(1 + static_cast<std::size_t>(std::ranges::count_if(
                               segments, [](const auto& segment) { return segment.splice; })));
    for (std::size_t segment = 0; segment < segments.size();) {
      auto run = run_containing(map, segment);
      data->runs.push_back({run, {}});
      segment = run.last + 1;
    }
    const double pitch = std::exp2(settings.transpose_cents / 1200.0);
    const auto unvoiced = std::max<std::int64_t>(
        8, std::llround(static_cast<double>(settings.grain_size_ms) * 0.0005 * limits.sample_rate));
    TonesGenerator generator(settings.marks, pitch, unvoiced, correlation_length(limits));
    Grain grain;
    for (std::size_t run_index = 0; run_index < data->runs.size(); ++run_index) {
      auto& entry = data->runs[run_index];
      generator.prime(map, entry.run);
      entry.checkpoints.push_back({generator.position()});
      while (generator.position() < static_cast<double>(entry.run.output_end)) {
        if (data->grain_count >= std::size_t{16} * 1024U * 1024U)
          return foundation::Result<std::shared_ptr<const TonesSeekIndex>>::failure(
              {foundation::ErrorCode::resource_exhausted, "Tones index grain limit exceeded"});
        (void)generator.next(grain);
        ++data->grain_count;
        if (data->grain_count % tones_checkpoint_interval == 0) {
          if (data->runs.size() + data->grain_count / tones_checkpoint_interval >=
              max_tones_checkpoints)
            return foundation::Result<std::shared_ptr<const TonesSeekIndex>>::failure(
                {foundation::ErrorCode::resource_exhausted,
                 "Tones index checkpoint limit exceeded"});
          entry.checkpoints.push_back({generator.position()});
        }
      }
    }
    return foundation::Result<std::shared_ptr<const TonesSeekIndex>>::success(
        std::shared_ptr<const TonesSeekIndex>(new TonesSeekIndex(std::move(data))));
  } catch (const std::bad_alloc&) {
    return foundation::Result<std::shared_ptr<const TonesSeekIndex>>::failure(
        {foundation::ErrorCode::resource_exhausted, "Tones index allocation failed"});
  }
}

foundation::Result<std::unique_ptr<WarpStream>>
create_tones_stream(const StreamLimits& limits, QualityTier tier, TonesSettings settings,
                    std::shared_ptr<const TonesSeekIndex> index) {
  if (!index)
    return invalid("Tones seek index is missing");
  return make_tones_stream(limits, tier, std::move(settings), std::move(index));
}

foundation::Result<std::unique_ptr<WarpStream>>
create_tones_stream(const StreamLimits& limits, QualityTier tier, TonesSettings settings) {
  return make_tones_stream(limits, tier, std::move(settings), {});
}

foundation::Result<std::unique_ptr<WarpStream>>
create_texture_stream(const StreamLimits& limits, QualityTier tier, TextureSettings settings) {
  if (!valid_limits(limits) || !valid_quality_tier(tier))
    return invalid("Texture stream limits are out of range");
  if (!std::isfinite(settings.grain_size_ms) || settings.grain_size_ms < 10.0F ||
      settings.grain_size_ms > 500.0F || !std::isfinite(settings.flux) || settings.flux < 0.0F ||
      settings.flux > 100.0F)
    return invalid("Texture grain size must be 10 to 500 ms and flux 0 to 100");
  if (settings.transpose_cents < -max_transpose_cents ||
      settings.transpose_cents > max_transpose_cents)
    return invalid("Texture transpose must be within four octaves");
  try {
    const double pitch = std::exp2(settings.transpose_cents / 1200.0);
    const auto half = std::max<std::int64_t>(
        8, std::llround(static_cast<double>(settings.grain_size_ms) * 0.0005 * limits.sample_rate));
    auto generator = std::make_unique<TextureGenerator>(half, settings.flux / 100.0, settings.seed,
                                                        pitch, limits.sample_rate);
    // Overlapping grains of decorrelated texture do not need a 100 dB
    // stopband: transposed reads use the lightest kernel at any tier.
    (void)tier;
    return StreamResult::success(std::make_unique<GrainStream>(
        limits, detail::BandlimitedKernel::for_tier(QualityTier::q2)->design(), pitch, half,
        std::move(generator)));
  } catch (const std::bad_alloc&) {
    return StreamResult::failure(
        {foundation::ErrorCode::resource_exhausted, "Texture stream allocation failed"});
  }
}

} // namespace oscillate::warp_dsp
