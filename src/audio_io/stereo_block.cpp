#include <oscillate/audio_io/stereo_block.hpp>

#include <oscillate/audio_io/metronome.hpp>

#include <oscillate/engine/denormals.hpp>

#include <algorithm>
#include <limits>

namespace oscillate::audio_io {

TransportCursor::TransportCursor(std::uint64_t first_sample) noexcept
    : position_(first_sample), completed_next_sample_(first_sample) {}

TransportCursor::CompletedPosition TransportCursor::completed_snapshot() const noexcept {
  for (;;) {
    const auto before = completed_sequence_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0) {
      continue;
    }
    const CompletedPosition result{completed_next_sample_.load(std::memory_order_seq_cst),
                                   completed_generation_.load(std::memory_order_seq_cst)};
    // One total order places both payload reads between the two sequence
    // reads. An intervening writer changes the sequence and forces a retry.
    if (before == completed_sequence_.load(std::memory_order_seq_cst)) {
      return result;
    }
  }
}

void TransportCursor::publish_completed(CompletedPosition value) noexcept {
  completed_sequence_.fetch_add(1, std::memory_order_seq_cst);
  completed_next_sample_.store(value.next_sample, std::memory_order_seq_cst);
  completed_generation_.store(value.generation, std::memory_order_seq_cst);
  completed_sequence_.fetch_add(1, std::memory_order_seq_cst);
}

std::optional<TransportCursor::AudiblePoint> TransportCursor::audible_point() const noexcept {
  for (;;) {
    const auto before = audible_sequence_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0)
      continue;
    const AudiblePoint point{audible_sample_.load(std::memory_order_seq_cst),
                             audible_heard_ns_.load(std::memory_order_seq_cst)};
    if (before == audible_sequence_.load(std::memory_order_seq_cst))
      return point.heard_ns != 0 ? std::optional{point} : std::nullopt;
  }
}

void TransportCursor::publish_audible(AudiblePoint point) noexcept {
  audible_sequence_.fetch_add(1, std::memory_order_seq_cst);
  audible_sample_.store(point.sample, std::memory_order_seq_cst);
  audible_heard_ns_.store(point.heard_ns, std::memory_order_seq_cst);
  audible_sequence_.fetch_add(1, std::memory_order_seq_cst);
}

void TransportCursor::complete_block(engine::BlockContext context, std::size_t frames,
                                     bool frozen) noexcept {
  frozen_ = frozen;
  if (frozen) {
    // A planned wrap the source could not play at once is no longer a
    // continuous wrap: the replayed block starts a new processor history,
    // exactly like a seek, instead of a position jump without a wrap flag.
    if (context.planned_loop_wrap)
      processing_generation_ = context.transport_generation;
    position_ = context.project_sample_position;
    publish_completed({context.project_sample_position, context.transport_generation});
  } else {
    publish_completed({context.project_sample_position + frames, context.transport_generation});
  }
}

std::optional<TransportCursor::CompletedPosition>
TransportCursor::restart_from_completed_after_quiesce() noexcept {
  return seek_after_quiesce(completed_snapshot().next_sample);
}

std::optional<TransportCursor::CompletedPosition>
TransportCursor::seek_after_quiesce(std::uint64_t first_sample) noexcept {
  const auto completed = completed_snapshot();
  if (completed.generation == std::numeric_limits<std::uint64_t>::max() ||
      first_sample == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  // The callback is quiesced: discard any pending request.
  if (const auto state = request_state_.load(std::memory_order_acquire); (state & 1U) != 0)
    request_state_.store(state + 1, std::memory_order_release);
  position_ = first_sample;
  generation_ = completed.generation + 1;
  processing_generation_ = generation_;
  generation_exhausted_ = false;
  frozen_ = false;
  const CompletedPosition resumed{position_, generation_};
  publish_completed(resumed);
  return resumed;
}

bool TransportCursor::queue_request(RequestKind kind, std::uint64_t boundary,
                                    std::uint64_t first_sample) noexcept {
  const auto state = request_state_.load(std::memory_order_acquire);
  if ((state & 1U) != 0)
    return false;
  requested_kind_.store(kind, std::memory_order_relaxed);
  requested_boundary_.store(boundary, std::memory_order_relaxed);
  requested_sample_.store(first_sample, std::memory_order_relaxed);
  request_state_.store(state + 1, std::memory_order_release);
  // A seek or restart moves away from what has been rendered ahead
  // (ADR-0128); a planned loop wrap continues it.
  if (kind != RequestKind::loop_wrap)
    discontinuities_.fetch_add(1, std::memory_order_release);
  return true;
}

bool TransportCursor::request_seek(std::uint64_t first_sample) noexcept {
  return queue_request(RequestKind::seek, 0, first_sample);
}

bool TransportCursor::request_loop_wrap(std::uint64_t first_sample) noexcept {
  return queue_request(RequestKind::loop_wrap, 0, first_sample);
}

bool TransportCursor::request_loop_wrap_at(std::uint64_t boundary,
                                           std::uint64_t first_sample) noexcept {
  return boundary != 0 && queue_request(RequestKind::loop_wrap, boundary, first_sample);
}

bool TransportCursor::request_restart_in_place() noexcept {
  return queue_request(RequestKind::restart_in_place, 0, 0);
}

bool TransportCursor::cancel_loop_wrap() noexcept {
  auto state = request_state_.load(std::memory_order_acquire);
  if ((state & 1U) == 0 ||
      requested_kind_.load(std::memory_order_relaxed) != RequestKind::loop_wrap)
    return false;
  return request_state_.compare_exchange_strong(state, state + 1, std::memory_order_acq_rel,
                                                std::memory_order_acquire);
}

TransportCursor::LoopWrapCounts TransportCursor::loop_wrap_counts() const noexcept {
  return {applied_loop_wraps_.load(std::memory_order_acquire),
          late_loop_wraps_.load(std::memory_order_acquire)};
}

std::optional<engine::BlockContext> TransportCursor::next_block(std::size_t frames) noexcept {
  const auto part = take_part(frames, false);
  if (!part)
    return std::nullopt;
  return part->context;
}

std::optional<TransportCursor::Part> TransportCursor::next_part(std::size_t frames) noexcept {
  return take_part(frames, true);
}

std::optional<TransportCursor::Part> TransportCursor::take_part(std::size_t frames,
                                                                bool split) noexcept {
  bool wrapping = false;
  if (auto state = request_state_.load(std::memory_order_acquire); (state & 1U) != 0) {
    const auto kind = requested_kind_.load(std::memory_order_relaxed);
    const auto boundary = requested_boundary_.load(std::memory_order_relaxed);
    const auto sample = requested_sample_.load(std::memory_order_relaxed);
    if (kind == RequestKind::loop_wrap && position_ < boundary) {
      // End this part exactly at the loop end; the next part wraps. This
      // also applies while a held part replays. A stale boundary (withdrawn
      // meanwhile) only splits a block, which is harmless.
      if (split && frames > boundary - position_ &&
          request_state_.load(std::memory_order_acquire) == state)
        frames = static_cast<std::size_t>(boundary - position_);
    } else if (!frozen_ && // queued requests wait while a held part replays
                           // The fields belong to this request only if it is still the
                           // pending one; a failed exchange means it was withdrawn.
               request_state_.compare_exchange_strong(state, state + 1, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
      wrapping = kind == RequestKind::loop_wrap;
      if (wrapping) {
        applied_loop_wraps_.fetch_add(1, std::memory_order_release);
        if (boundary != 0 && position_ > boundary)
          late_loop_wraps_.fetch_add(1, std::memory_order_release);
      }
      if (kind != RequestKind::restart_in_place)
        position_ = sample;
      if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        generation_exhausted_ = true;
      } else {
        ++generation_;
        if (!wrapping)
          processing_generation_ = generation_;
      }
    }
  }
  if (generation_exhausted_ || frames == 0 ||
      frames > std::numeric_limits<std::uint64_t>::max() - position_) {
    return std::nullopt;
  }
  const engine::BlockContext context{position_, generation_, processing_generation_, wrapping};
  position_ += frames;
  return Part{context, frames};
}

BlockResult render_stereo_block(StereoRenderSource source, engine::BlockContext context,
                                std::span<engine::StereoFrame> stereo_scratch,
                                std::span<float> interleaved_output) noexcept {
  if (interleaved_output.size() % 2 != 0 || interleaved_output.size() / 2 > stereo_scratch.size()) {
    return BlockResult::invalid_buffer;
  }
  const auto frames = interleaved_output.size() / 2;
  auto stereo = stereo_scratch.first(frames);
  if (source.render == nullptr || context.transport_generation == 0 ||
      frames > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position ||
      !source.render(source.context, stereo, context)) {
    std::ranges::fill(interleaved_output, 0.0F);
    return BlockResult::silent_render_failure;
  }
  for (std::size_t frame = 0; frame < frames; ++frame) {
    interleaved_output[frame * 2] = stereo[frame].left;
    interleaved_output[frame * 2 + 1] = stereo[frame].right;
  }
  return BlockResult::rendered;
}

BlockResult render_transport_block(StereoRenderSource source, TransportCursor& transport,
                                   bool format_ready, std::span<engine::StereoFrame> stereo_scratch,
                                   std::span<float> interleaved_output,
                                   BlockTiming timing) noexcept {
  const engine::DenormalsFlushed flushed;
  if (interleaved_output.size() % 2 != 0 || interleaved_output.size() / 2 > stereo_scratch.size()) {
    return BlockResult::invalid_buffer;
  }
  if (!format_ready) {
    std::ranges::fill(interleaved_output, 0.0F);
    return BlockResult::silent_render_failure;
  }
  const auto total = interleaved_output.size() / 2;
  auto result = BlockResult::rendered;
  std::size_t done = 0;
  auto* metronome = transport.metronome();
  // A count-in holds the transport: only its clicks play until it is done.
  if (metronome != nullptr)
    done = metronome->render_count_in(interleaved_output);
  if (done == total)
    return result;
  constexpr double nanoseconds = 1e9;
  // At most two parts: one scheduled boundary can fall inside a block, and
  // the wrap it applies leaves no request pending.
  do {
    const auto part = transport.next_part(total - done);
    if (!part) {
      std::ranges::fill(interleaved_output.subspan(done * 2), 0.0F);
      return BlockResult::silent_render_failure;
    }
    const auto output = interleaved_output.subspan(done * 2, part->frames * 2);
    if (render_stereo_block(source, part->context, stereo_scratch, output) != BlockResult::rendered)
      result = BlockResult::silent_render_failure;
    const bool frozen = source.freeze_cursor != nullptr && source.freeze_cursor(source.context);
    if (!frozen) {
      if (metronome != nullptr)
        metronome->add_clicks(part->context.project_sample_position, output);
      if (timing.first_frame_heard_ns != 0 && timing.sample_rate != 0)
        transport.publish_audible(
            {part->context.project_sample_position,
             timing.first_frame_heard_ns +
                 static_cast<std::int64_t>(static_cast<double>(done) * nanoseconds /
                                           timing.sample_rate)});
    }
    transport.complete_block(part->context, part->frames, frozen);
    done += part->frames;
    if (frozen) {
      // The held part replays next callback; nothing after it may play now.
      std::ranges::fill(interleaved_output.subspan(done * 2), 0.0F);
      break;
    }
  } while (done < total);
  return result;
}

} // namespace oscillate::audio_io
