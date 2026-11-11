#include "oscillate/audio_io/render_ahead.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <limits>

namespace oscillate::audio_io {
namespace {

constexpr double nanoseconds_per_second = 1e9;
constexpr std::uint64_t milliseconds_per_second = 1000;
// Interleaved stereo: two samples a frame.
constexpr std::size_t channels = 2;

} // namespace

RenderAhead::RenderAhead(StereoRenderSource source, TransportCursor& transport,
                         std::size_t max_block_frames)
    : source_(source), transport_(transport), max_block_frames_(max_block_frames),
      producer_scratch_(max_block_frames) {
  source_.ahead = nullptr;
  for (auto& slot : slots_)
    slot.samples.assign(max_block_frames * channels, 0.0F);
  seen_discontinuities_ = transport_.discontinuities();
  const auto completed = transport_.completed_snapshot();
  publish_position(completed.next_sample, completed.generation);
}

RenderAhead::~RenderAhead() { stop_owned_thread(); }

StereoRenderSource RenderAhead::device_source() noexcept {
  auto device = source_;
  device.ahead = this;
  return device;
}

bool RenderAhead::capture(void* context, std::span<engine::StereoFrame> output,
                          engine::BlockContext block) noexcept {
  auto& self = *static_cast<RenderAhead*>(context);
  const bool rendered = self.source_.render(self.source_.context, output, block);
  if (auto* slot = self.capturing_; slot != nullptr && slot->part_count < slot->parts.size())
    slot->parts[slot->part_count++] = {block, 0, output.size()};
  return rendered;
}

bool RenderAhead::capture_frozen(void* context) noexcept {
  auto& self = *static_cast<RenderAhead*>(context);
  self.capture_frozen_ =
      self.source_.freeze_cursor != nullptr && self.source_.freeze_cursor(self.source_.context);
  return self.capture_frozen_;
}

bool RenderAhead::acquire_render() noexcept {
  std::uint32_t free = 0;
  return rendering_.compare_exchange_strong(free, 1, std::memory_order_acquire,
                                            std::memory_order_relaxed);
}

void RenderAhead::release_render() noexcept {
  rendering_.store(0, std::memory_order_release);
  rendering_.notify_all();
}

void RenderAhead::wake_producer() noexcept {
  wake_.fetch_add(1, std::memory_order_release);
  wake_.notify_one();
}

std::size_t RenderAhead::target_blocks() const noexcept {
  const auto block = block_frames_.load(std::memory_order_acquire);
  if (block == 0)
    return 0;
  const auto cap =
      std::min(max_window_blocks, limit_frames_.load(std::memory_order_acquire) / block);
  return std::min(cap, window_blocks_.load(std::memory_order_acquire));
}

std::uint64_t RenderAhead::first_unread() const noexcept {
  return std::max(read_.load(std::memory_order_acquire),
                  discard_before_.load(std::memory_order_acquire));
}

bool RenderAhead::wants_block() const noexcept {
  if (stop_.load(std::memory_order_acquire) || !active_.load(std::memory_order_acquire) ||
      held_.load(std::memory_order_acquire) || fading_.load(std::memory_order_acquire))
    return false;
  // A seek drops the window and renders from its new position at once.
  if (transport_.discontinuities() != seen_discontinuities_)
    return true;
  if (live_input_.load(std::memory_order_acquire))
    return false;
  return write_.load(std::memory_order_relaxed) - first_unread() < target_blocks();
}

bool RenderAhead::produce_one() noexcept {
  const auto frames = block_frames_.load(std::memory_order_acquire);
  if (frames == 0 || frames > max_block_frames_ || !acquire_render())
    return false;
  const auto write = write_.load(std::memory_order_relaxed);
  // A seek queued since the last block: the window holds the old position.
  const auto discontinuities = transport_.discontinuities();
  if (discontinuities != seen_discontinuities_) {
    seen_discontinuities_ = discontinuities;
    discard_before_.store(write, std::memory_order_release);
  }
  // Room is counted from the block the callback may still be copying, even
  // one a seek dropped, so its slot is never written under it.
  if (write - read_.load(std::memory_order_acquire) + 1 >= slot_count) {
    release_render();
    return false;
  }
  auto& slot = slots_[write % slot_count];
  slot.part_count = 0;
  capturing_ = &slot;
  capture_frozen_ = false;
  const StereoRenderSource capturing{this, &RenderAhead::capture, &RenderAhead::capture_frozen,
                                     source_.workers, nullptr};
  const auto result = [&] {
    OSCILLATE_TRACE_SPAN("audio.render_ahead", frames);
    return render_transport_block(capturing, transport_, true, producer_scratch_,
                                  std::span(slot.samples).first(frames * channels));
  }();
  capturing_ = nullptr;
  if (capture_frozen_) {
    // The source holds (a seek primes its clips): the transport did not move,
    // so nothing is kept; the callback renders the hold until it lets go.
    held_.store(true, std::memory_order_release);
    release_render();
    return false;
  }
  // A count-in plays before the parts; the parts fill the rest in order.
  std::size_t rendered = 0;
  for (std::size_t part = 0; part < slot.part_count; ++part)
    rendered += slot.parts[part].frames;
  auto offset = frames - std::min(frames, rendered);
  for (std::size_t part = 0; part < slot.part_count; ++part) {
    slot.parts[part].offset = offset;
    offset += slot.parts[part].frames;
  }
  slot.frames = frames;
  slot.result = result;
  if (slot.part_count != 0) {
    // A seek applied by this block: the blocks before it are the old
    // position's.
    const auto first = slot.parts[0].context.state_generation();
    if (last_processing_generation_ != 0 && first != last_processing_generation_)
      discard_before_.store(write, std::memory_order_release);
    last_processing_generation_ = slot.parts[slot.part_count - 1].context.state_generation();
  }
  write_.store(write + 1, std::memory_order_release);
  // Released only once the block is in the window: a callback that renders
  // next must find it there, or it would render past it.
  release_render();
  return true;
}

void RenderAhead::run_producer() noexcept {
  foundation::trace::name_thread("audio render-ahead");
  const foundation::RealtimeThreadScope realtime;
  const engine::DenormalsFlushed flushed;
  while (!stop_.load(std::memory_order_acquire)) {
    const auto seen = wake_.load(std::memory_order_acquire);
    while (wants_block() && produce_one()) {
    }
    if (stop_.load(std::memory_order_acquire))
      break;
    // Until the next device block or a control change.
    wake_.wait(seen, std::memory_order_acquire);
  }
}

void RenderAhead::request_stop() noexcept {
  stop_.store(true, std::memory_order_release);
  wake_producer();
}

void RenderAhead::reset_after_join() noexcept { stop_.store(false, std::memory_order_release); }

void RenderAhead::start_owned_thread() {
  if (!owned_.joinable())
    owned_ = std::thread([this] { run_producer(); });
}

void RenderAhead::stop_owned_thread() noexcept {
  if (!owned_.joinable())
    return;
  request_stop();
  owned_.join();
  reset_after_join();
}

void RenderAhead::set_limit(std::uint32_t milliseconds, std::uint32_t sample_rate) noexcept {
  const auto clamped = std::min(milliseconds, max_limit_ms);
  limit_frames_.store(static_cast<std::size_t>(static_cast<std::uint64_t>(clamped) * sample_rate /
                                               milliseconds_per_second),
                      std::memory_order_release);
  wake_producer();
}

void RenderAhead::set_live_input(bool live) noexcept {
  live_input_.store(live, std::memory_order_release);
  wake_producer();
}

void RenderAhead::fade_out() noexcept {
  fading_.store(true, std::memory_order_release);
  wake_producer();
}

bool RenderAhead::faded() const noexcept { return faded_.load(std::memory_order_acquire); }

void RenderAhead::clear_after_quiesce() noexcept {
  const auto write = write_.load(std::memory_order_relaxed);
  read_.store(write, std::memory_order_relaxed);
  discard_before_.store(write, std::memory_order_relaxed);
  read_offset_ = 0;
  held_.store(false, std::memory_order_relaxed);
  active_.store(false, std::memory_order_relaxed);
  fading_.store(false, std::memory_order_relaxed);
  faded_.store(false, std::memory_order_relaxed);
  fade_remaining_ = 0;
  window_blocks_.store(max_window_blocks, std::memory_order_relaxed);
  blocks_since_empty_ = 0;
  served_last_ = false;
  last_processing_generation_ = 0;
  seen_discontinuities_ = transport_.discontinuities();
  const auto completed = transport_.completed_snapshot();
  publish_position(completed.next_sample, completed.generation);
}

RenderAhead::Status RenderAhead::status() const noexcept {
  const auto block = block_frames_.load(std::memory_order_acquire);
  const auto write = write_.load(std::memory_order_acquire);
  const auto read = first_unread();
  return {target_blocks(), write > read ? static_cast<std::size_t>(write - read) * block : 0,
          ahead_blocks_.load(std::memory_order_relaxed),
          just_in_time_blocks_.load(std::memory_order_relaxed),
          empty_window_blocks_.load(std::memory_order_relaxed)};
}

TransportCursor::CompletedPosition RenderAhead::audible_position() const noexcept {
  for (;;) {
    const auto before = audible_sequence_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0)
      continue;
    const TransportCursor::CompletedPosition position{
        audible_next_sample_.load(std::memory_order_seq_cst),
        audible_generation_.load(std::memory_order_seq_cst)};
    if (before == audible_sequence_.load(std::memory_order_seq_cst))
      return position;
  }
}

std::size_t RenderAhead::worst_edit_delay_frames() const noexcept {
  const auto block = block_frames_.load(std::memory_order_acquire);
  return std::min(limit_frames_.load(std::memory_order_acquire), max_window_blocks * block) + block;
}

void RenderAhead::publish_position(std::uint64_t next_sample, std::uint64_t generation) noexcept {
  audible_sequence_.fetch_add(1, std::memory_order_seq_cst);
  audible_next_sample_.store(next_sample, std::memory_order_seq_cst);
  audible_generation_.store(generation, std::memory_order_seq_cst);
  audible_sequence_.fetch_add(1, std::memory_order_seq_cst);
}

BlockResult RenderAhead::render(bool format_ready, std::span<engine::StereoFrame> stereo_scratch,
                                std::span<float> interleaved_output, BlockTiming timing) noexcept {
  if (interleaved_output.size() % channels != 0 ||
      interleaved_output.size() / channels > stereo_scratch.size() ||
      interleaved_output.size() / channels > max_block_frames_)
    return BlockResult::invalid_buffer;
  const auto frames = interleaved_output.size() / channels;
  if (!format_ready || faded_.load(std::memory_order_acquire)) {
    std::ranges::fill(interleaved_output, 0.0F);
    active_.store(false, std::memory_order_release);
    return format_ready ? BlockResult::rendered : BlockResult::silent_render_failure;
  }
  block_frames_.store(frames, std::memory_order_release);
  active_.store(true, std::memory_order_release);
  const auto heard_from = audible_position();
  auto result = BlockResult::rendered;
  // Blocks a seek dropped are never played.
  auto read = read_.load(std::memory_order_relaxed);
  if (const auto discard = discard_before_.load(std::memory_order_acquire); discard > read) {
    read = discard;
    read_offset_ = 0;
    read_.store(read, std::memory_order_release);
  }
  bool served = false;
  bool just_in_time = false;
  std::size_t done = 0;
  while (done < frames) {
    if (read < write_.load(std::memory_order_acquire)) {
      const auto& slot = slots_[read % slot_count];
      const auto count = std::min(slot.frames - read_offset_, frames - done);
      std::ranges::copy(std::span(slot.samples).subspan(read_offset_ * channels, count * channels),
                        interleaved_output.subspan(done * channels).begin());
      if (slot.result != BlockResult::rendered)
        result = slot.result;
      // What this copy plays: each part's frames that fall in it.
      for (std::size_t index = 0; index < slot.part_count; ++index) {
        const auto& part = slot.parts[index];
        const auto first = std::max(part.offset, read_offset_);
        const auto last = std::min(part.offset + part.frames, read_offset_ + count);
        if (first >= last)
          continue;
        const auto sample = part.context.project_sample_position + (first - part.offset);
        if (timing.first_frame_heard_ns != 0 && timing.sample_rate != 0)
          transport_.publish_audible(
              {sample,
               timing.first_frame_heard_ns +
                   static_cast<std::int64_t>(static_cast<double>(done + first - read_offset_) *
                                             nanoseconds_per_second / timing.sample_rate)});
        publish_position(sample + (last - first), part.context.transport_generation);
      }
      read_offset_ += count;
      done += count;
      if (read_offset_ == slot.frames) {
        ++read;
        read_offset_ = 0;
        read_.store(read, std::memory_order_release);
      }
      served = true;
      continue;
    }
    // The window is empty. One that was serving the last block ran dry: the
    // producer fell behind, so the window grows.
    if (served_last_ && !live_input_.load(std::memory_order_acquire) &&
        !fading_.load(std::memory_order_acquire) && target_blocks() != 0) {
      empty_window_blocks_.fetch_add(1, std::memory_order_relaxed);
      window_blocks_.store(
          std::min(max_window_blocks, window_blocks_.load(std::memory_order_relaxed) + 1),
          std::memory_order_release);
      blocks_since_empty_ = 0;
      served_last_ = false;
    }
    if (!acquire_render()) {
      // The producer is rendering, most likely the very block wanted here.
      rendering_.wait(1, std::memory_order_acquire);
      continue;
    }
    if (read < write_.load(std::memory_order_acquire)) {
      release_render();
      continue;
    }
    const BlockTiming later{
        timing.first_frame_heard_ns == 0
            ? 0
            : timing.first_frame_heard_ns +
                  static_cast<std::int64_t>(static_cast<double>(done) * nanoseconds_per_second /
                                            timing.sample_rate),
        timing.sample_rate};
    const auto rendered =
        render_transport_block(source_, transport_, true, stereo_scratch,
                               interleaved_output.subspan(done * channels), later);
    held_.store(source_.freeze_cursor != nullptr && source_.freeze_cursor(source_.context),
                std::memory_order_release);
    // Nothing is ahead: the device plays what was just rendered, and a seek
    // it applied is the producer's starting point too.
    last_processing_generation_ = 0;
    const auto completed = transport_.completed_snapshot();
    release_render();
    publish_position(completed.next_sample, completed.generation);
    if (rendered != BlockResult::rendered)
      result = rendered;
    done = frames;
    just_in_time = true;
  }
  served_last_ = served && !just_in_time;
  (served ? ahead_blocks_ : just_in_time_blocks_).fetch_add(1, std::memory_order_relaxed);
  if (++blocks_since_empty_ >= shrink_after_blocks) {
    blocks_since_empty_ = 0;
    const auto window = window_blocks_.load(std::memory_order_relaxed);
    if (window > min_window_blocks)
      window_blocks_.store(window - 1, std::memory_order_release);
  }
  if (fading_.load(std::memory_order_acquire)) {
    if (fade_remaining_ == 0)
      fade_remaining_ = stop_fade_frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto gain = static_cast<float>(fade_remaining_) / static_cast<float>(stop_fade_frames);
      interleaved_output[frame * channels] *= gain;
      interleaved_output[frame * channels + 1] *= gain;
      if (fade_remaining_ > 0 && --fade_remaining_ == 0) {
        std::ranges::fill(interleaved_output.subspan((frame + 1) * channels), 0.0F);
        faded_.store(true, std::memory_order_release);
        break;
      }
    }
  }
  if (tap_.played != nullptr) {
    auto played = stereo_scratch.first(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
      played[frame] = {interleaved_output[frame * channels],
                       interleaved_output[frame * channels + 1]};
    tap_.played(tap_.context, played, {heard_from.next_sample, heard_from.generation});
    for (std::size_t frame = 0; frame < frames; ++frame) {
      interleaved_output[frame * channels] = played[frame].left;
      interleaved_output[frame * channels + 1] = played[frame].right;
    }
  }
  wake_producer();
  return result;
}

BlockResult render_device_block(StereoRenderSource source, TransportCursor& transport,
                                bool format_ready, std::span<engine::StereoFrame> stereo_scratch,
                                std::span<float> interleaved_output, BlockTiming timing) noexcept {
  if (source.ahead != nullptr)
    return source.ahead->render(format_ready, stereo_scratch, interleaved_output, timing);
  return render_transport_block(source, transport, format_ready, stereo_scratch, interleaved_output,
                                timing);
}

void* render_ahead_entry(void* ahead) noexcept {
  static_cast<RenderAhead*>(ahead)->run_producer();
  return nullptr;
}

} // namespace oscillate::audio_io
