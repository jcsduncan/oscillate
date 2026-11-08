#include "../core/bandlimited_kernel.hpp"
#include "../core/bandlimited_read.hpp"
#include "../core/source_window.hpp"
#include "oscillate/foundation/stereo_span.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace oscillate::warp_dsp {
namespace {

using detail::BandlimitedKernel;
using detail::KernelDesign;
using detail::SourceWindow;

constexpr std::size_t chunk_frames = 256;
constexpr double maximum_pitch_speed = 16.0;

enum class EventEnd : std::uint8_t { slice, splice, map_end };

// One slice as laid out in output time: it starts sounding at `begin` from
// source position `play` and is replaced at `end`.
struct SliceEvent {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t play = 0;      // Q48.16 source position at `begin`
  std::uint64_t remaining = 0; // natural source span before the next slice (Q48.16)
  EventEnd end_kind = EventEnd::map_end;
  bool fade_in = false; // entered mid-slice (splice or map start)
};

class BeatsStream final : public WarpStream {
public:
  BeatsStream(const StreamLimits& limits, const KernelDesign& design, BeatsSettings settings)
      : limits_(limits), settings_(std::move(settings)),
        pitch_step_(static_cast<std::uint64_t>(
            std::llround(std::exp2(settings_.transpose_cents / 1200.0) * source_unit))),
        fade_frames_(std::max<std::uint64_t>(16, (limits.sample_rate + 250) / 500)),
        // Transposition is fixed for the stream's life, so one precomputed
        // polyphase bank serves every read.
        reader_(design, static_cast<double>(pitch_step_) / source_unit), reach_(reader_.reach()),
        main_(chunk_frames * static_cast<std::size_t>(maximum_pitch_speed) +
              2 * static_cast<std::size_t>(reach_) + 4),
        blend_(main_.capacity()), tail_(main_.capacity()) {
    slices_.reserve(settings_.slice_starts.size());
    for (const auto start : settings_.slice_starts)
      slices_.push_back(start << source_fraction_bits);
  }

  StreamStatus start(const WarpTimeMap& map, SourceView& source,
                     std::uint64_t output_frame) noexcept override {
    map_ = nullptr;
    source_ = nullptr;
    event_valid_ = false;
    main_.reset();
    blend_.reset();
    tail_.reset();
    if (output_frame >= map.output_frames())
      return status_ = StreamStatus::out_of_range;
    map_ = &map;
    source_ = &source;
    source_end_ = source.frames() << source_fraction_bits;
    next_ = output_frame;
    return status_ = StreamStatus::ok;
  }

  std::size_t render(std::span<float> left, std::span<float> right) noexcept override {
    if (status_ != StreamStatus::ok || left.size() != right.size())
      return 0;
    const auto requested = std::min<std::size_t>(left.size(), limits_.max_render_frames);
    std::size_t written = 0;
    while (written < requested && next_ < map_->output_frames()) {
      if (!event_valid_ || next_ < event_.begin || next_ >= event_.end) {
        event_ = locate(next_);
        event_valid_ = true;
        // A slice cut short keeps sounding under the new one for a short
        // fade, so the incoming attack is never preceded by a dip.
        tail_active_ = false;
        if (event_.begin > 0) {
          previous_ = locate(event_.begin - 1);
          tail_active_ = previous_.end == event_.begin && previous_.end_kind != EventEnd::map_end &&
                         !continuous(previous_);
        }
      }
      const auto frames = static_cast<std::size_t>(
          std::min<std::uint64_t>({event_.end - next_, requested - written, chunk_frames}));
      if (!render_chunk(
              {.left = left.subspan(written, frames), .right = right.subspan(written, frames)})) {
        status_ = StreamStatus::source_failed;
        break;
      }
      written += frames;
      next_ += frames;
    }
    return written;
  }

  std::uint64_t next_output_frame() const noexcept override { return next_; }
  StreamStatus status() const noexcept override { return status_; }

private:
  [[nodiscard]] std::uint64_t slice_position(std::ptrdiff_t index) const noexcept {
    return index < 0 ? 0 : slices_[static_cast<std::size_t>(index)];
  }

  // Derives the slice event containing output frame t from the map alone, so
  // any seek reproduces exactly what continuous rendering produced.
  [[nodiscard]] SliceEvent locate(std::uint64_t t) const noexcept {
    const auto segments = map_->segments();
    const auto current = map_->find_segment(t);
    const auto position = WarpTimeMap::source_at(segments[current], t);
    const auto slice =
        static_cast<std::ptrdiff_t>(std::ranges::upper_bound(slices_, position) - slices_.begin()) -
        1;
    const auto slice_start = slice_position(slice);
    SliceEvent event;
    for (auto index = current;; --index) {
      const auto& segment = segments[index];
      if (slice_start >= segment.source_begin) {
        event.begin = WarpTimeMap::first_output_reaching(segment, slice_start);
        event.play = slice_start;
        break;
      }
      if (segment.splice || index == 0) {
        event.begin = segment.output_begin;
        event.play = segment.source_begin;
        event.fade_in = true;
        break;
      }
    }
    const bool has_next = static_cast<std::size_t>(slice + 1) < slices_.size();
    const auto next_start = has_next ? slices_[static_cast<std::size_t>(slice + 1)] : source_end_;
    event.remaining = next_start > event.play ? next_start - event.play : 0;
    for (auto index = current;; ++index) {
      const auto& segment = segments[index];
      if (has_next) {
        const auto crossing = WarpTimeMap::first_output_reaching(segment, next_start);
        if (crossing < segment.output_end) {
          event.end = crossing;
          event.end_kind = EventEnd::slice;
          break;
        }
      }
      if (index + 1 == segments.size()) {
        event.end = segment.output_end;
        event.end_kind = EventEnd::map_end;
        break;
      }
      if (segments[index + 1].splice) {
        event.end = segment.output_end;
        event.end_kind = EventEnd::splice;
        break;
      }
    }
    return event;
  }

  struct Voice {
    std::uint64_t base = 0;  // read offset into the slice (Q48.16)
    std::uint64_t blend = 0; // crossfade partner offset
    float blend_weight = 0.0F;
    float gain = 1.0F;
  };

  [[nodiscard]] bool continuous(const SliceEvent& event) const noexcept {
    return event.end_kind == EventEnd::slice &&
           (event.end - event.begin) * pitch_step_ == event.remaining;
  }

  // Playback offset and gains for output frame t of `event`; t may lie past
  // the event's end while it fades out under its successor.
  [[nodiscard]] Voice voice(const SliceEvent& event, std::uint64_t t) const noexcept {
    Voice voice;
    const auto rel = t - event.begin;
    const auto duration = event.end - event.begin;
    const auto length = event.remaining;
    const auto virtual_offset = rel * pitch_step_;
    const auto crossfade = fade_frames_ * pitch_step_;
    const auto middle = (length / 2) & ~(source_unit - 1);
    const auto period = length - middle;
    // Loop only when the slice runs out early and can hold two crossfades;
    // that also guarantees a non-zero loop period.
    const bool can_loop = settings_.loop != BeatsLoop::off && crossfade != 0 &&
                          length >= 4 * crossfade && period != 0 && duration * pitch_step_ > length;
    if (virtual_offset < length) {
      voice.base = virtual_offset;
      if (can_loop && settings_.loop == BeatsLoop::forward &&
          virtual_offset + crossfade >= length) {
        // Blend into the audio leading up to the loop start so the first
        // wrap is seamless.
        voice.blend = middle - (length - virtual_offset);
        voice.blend_weight =
            static_cast<float>(virtual_offset + crossfade - length) / static_cast<float>(crossfade);
      }
      // With no loop, a slice that runs out before its event ends fades to
      // silence at its natural end.
      if (!can_loop && duration * pitch_step_ > length) {
        const auto frames_left =
            (length - virtual_offset) / std::max<std::uint64_t>(pitch_step_, 1);
        voice.gain *=
            std::min(1.0F, static_cast<float>(frames_left) / static_cast<float>(fade_frames_));
      }
    } else if (!can_loop) {
      voice.gain = 0.0F;
    } else if (settings_.loop == BeatsLoop::forward) {
      const auto cycle = (virtual_offset - length) % period;
      voice.base = middle + cycle;
      if (cycle + crossfade >= period) {
        voice.blend = middle - (period - cycle);
        voice.blend_weight =
            static_cast<float>(cycle + crossfade - period) / static_cast<float>(crossfade);
      }
    } else {
      const auto cycle = (virtual_offset - length) % (2 * period);
      voice.base = cycle < period ? length - cycle : middle + (cycle - period);
    }
    if (event.fade_in)
      voice.gain *= std::min(1.0F, static_cast<float>(rel + 1) / static_cast<float>(fade_frames_));
    if (settings_.envelope < 100.0F) {
      const double decay = std::max(static_cast<double>(fade_frames_),
                                    settings_.envelope / 100.0 * static_cast<double>(duration));
      voice.gain *= static_cast<float>(std::max(0.0, 1.0 - static_cast<double>(rel) / decay));
    }
    return voice;
  }

  [[nodiscard]] bool contiguous(std::uint64_t before, std::uint64_t after) const noexcept {
    const auto gap = before > after ? before - after : after - before;
    return gap <= 2 * pitch_step_ + source_unit;
  }

  // Renders runs of contiguous read positions; a loop wrap starts a new run so
  // every run fits the preallocated windows.
  bool render_chunk(foundation::StereoSpan<float> out) noexcept {
    const auto left = out.left;
    const auto right = out.right;
    const auto frames = left.size();
    std::array<Voice, chunk_frames> voices{};
    std::array<Voice, chunk_frames> tails{};
    std::array<float, chunk_frames> tail_weights{};
    for (std::size_t index = 0; index < frames; ++index) {
      const auto t = next_ + index;
      voices[index] = voice(event_, t);
      if (tail_active_ && t - event_.begin < fade_frames_) {
        tails[index] = voice(previous_, t);
        tail_weights[index] =
            1.0F - (static_cast<float>(t - event_.begin) + 0.5F) / static_cast<float>(fade_frames_);
      }
    }
    const auto ensure_at = [this](SourceWindow& window, std::uint64_t play, std::uint64_t first,
                                  std::uint64_t last) {
      const auto begin = static_cast<std::int64_t>((play + first) >> source_fraction_bits) - reach_;
      const auto end =
          static_cast<std::int64_t>((play + last) >> source_fraction_bits) + reach_ + 1;
      return window.ensure(*source_, begin, static_cast<std::size_t>(end - begin));
    };
    const auto ensure = [this, &ensure_at](SourceWindow& window, std::uint64_t first,
                                           std::uint64_t last) {
      return ensure_at(window, event_.play, first, last);
    };

    std::size_t run = 0;
    while (run < frames) {
      auto run_end = run + 1;
      // A loop wrap in either the sounding slice or the fading tail starts a
      // new run.
      while (run_end < frames && contiguous(voices[run_end - 1].base, voices[run_end].base) &&
             contiguous(tails[run_end - 1].base, tails[run_end].base))
        ++run_end;
      std::uint64_t low = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t high = 0;
      std::uint64_t blend_low = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t blend_high = 0;
      std::uint64_t tail_low = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t tail_high = 0;
      for (auto index = run; index < run_end; ++index) {
        if (tail_weights[index] > 0.0F && tails[index].gain != 0.0F) {
          tail_low = std::min(tail_low, tails[index].base);
          tail_high = std::max(tail_high, tails[index].base);
        }
        const auto& current = voices[index];
        if (current.gain == 0.0F)
          continue;
        low = std::min(low, current.base);
        high = std::max(high, current.base);
        if (current.blend_weight > 0.0F) {
          blend_low = std::min(blend_low, current.blend);
          blend_high = std::max(blend_high, current.blend);
        }
      }
      if (low <= high && !ensure(main_, low, high))
        return false;
      if (blend_low <= blend_high && !ensure(blend_, blend_low, blend_high))
        return false;
      if (tail_low <= tail_high && !ensure_at(tail_, previous_.play, tail_low, tail_high))
        return false;
      for (auto index = run; index < run_end; ++index) {
        const auto& current = voices[index];
        float out_left = 0.0F;
        float out_right = 0.0F;
        if (current.gain != 0.0F) {
          auto sample = reader_.read(main_, event_.play + current.base);
          if (current.blend_weight > 0.0F) {
            const auto partner = reader_.read(blend_, event_.play + current.blend);
            sample.left += current.blend_weight * (partner.left - sample.left);
            sample.right += current.blend_weight * (partner.right - sample.right);
          }
          out_left = current.gain * sample.left;
          out_right = current.gain * sample.right;
        }
        if (tail_weights[index] > 0.0F && tails[index].gain != 0.0F) {
          const auto tail = reader_.read(tail_, previous_.play + tails[index].base);
          const float gain = tail_weights[index] * tails[index].gain;
          out_left += gain * tail.left;
          out_right += gain * tail.right;
        }
        left[index] = out_left;
        right[index] = out_right;
      }
      run = run_end;
    }
    return true;
  }

  StreamLimits limits_;
  BeatsSettings settings_;
  std::vector<std::uint64_t> slices_;
  std::uint64_t pitch_step_;
  std::uint64_t fade_frames_;
  detail::FixedRateReader reader_;
  std::int64_t reach_;
  SourceWindow main_;
  SourceWindow blend_;
  SourceWindow tail_;
  const WarpTimeMap* map_ = nullptr;
  SourceView* source_ = nullptr;
  std::uint64_t source_end_ = 0;
  std::uint64_t next_ = 0;
  SliceEvent event_;
  SliceEvent previous_;
  bool event_valid_ = false;
  bool tail_active_ = false;
  StreamStatus status_ = StreamStatus::not_started;
};

} // namespace

foundation::Result<std::unique_ptr<WarpStream>>
create_beats_stream(const StreamLimits& limits, QualityTier tier, BeatsSettings settings) {
  using Result = foundation::Result<std::unique_ptr<WarpStream>>;
  const auto invalid = [](const char* message) {
    return Result::failure({foundation::ErrorCode::invalid_argument, message});
  };
  if (!valid_quality_tier(tier))
    return invalid("Beats quality tier is invalid");
  if (limits.sample_rate < 8'000 || limits.sample_rate > 384'000 || limits.max_render_frames == 0 ||
      limits.max_render_frames > 65'536)
    return invalid("Beats stream limits are out of range");
  if (settings.slice_starts.size() > max_beats_slices ||
      std::ranges::adjacent_find(settings.slice_starts, std::ranges::greater_equal{}) !=
          settings.slice_starts.end() ||
      (!settings.slice_starts.empty() &&
       settings.slice_starts.back() > (max_source_position >> source_fraction_bits)))
    return invalid("Beats slices must be strictly increasing source frames");
  if (!std::isfinite(settings.envelope) || settings.envelope < 0.0F || settings.envelope > 100.0F)
    return invalid("Beats envelope must be 0 to 100");
  if (settings.transpose_cents < -max_transpose_cents ||
      settings.transpose_cents > max_transpose_cents)
    return invalid("Beats transpose must be within four octaves");
  try {
    return Result::success(std::make_unique<BeatsStream>(
        limits, detail::BandlimitedKernel::for_tier(tier)->design(), std::move(settings)));
  } catch (const std::bad_alloc&) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Beats stream allocation failed"});
  }
}

} // namespace oscillate::warp_dsp
