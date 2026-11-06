#include "oscillate/engine/chunked_processor.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace oscillate::engine {

ChunkedProcessorRuntime::ChunkedProcessorRuntime(std::shared_ptr<ProcessorRuntime> processor,
                                                 std::size_t max_chunk_frames) noexcept
    : processor_(std::move(processor)),
      sidechain_(dynamic_cast<SidechainProcessorRuntime*>(processor_.get())),
      max_chunk_frames_(max_chunk_frames) {}

foundation::Result<std::shared_ptr<ChunkedProcessorRuntime>>
ChunkedProcessorRuntime::prepare(std::shared_ptr<ProcessorRuntime> processor,
                                 std::size_t max_chunk_frames) {
  using Result = foundation::Result<std::shared_ptr<ChunkedProcessorRuntime>>;
  if (!processor || max_chunk_frames == 0 || max_chunk_frames > 8192)
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "Chunked processor needs an owner and a 1..8192 frame cap"});
  try {
    return Result::success(std::shared_ptr<ChunkedProcessorRuntime>(
        new ChunkedProcessorRuntime(std::move(processor), max_chunk_frames)));
  } catch (const std::bad_alloc&) {
    return Result::failure({foundation::ErrorCode::resource_exhausted,
                            "Chunked processor preparation ran out of memory"});
  }
}

void ChunkedProcessorRuntime::process(std::span<StereoFrame> frames,
                                      BlockContext context) noexcept {
  if (frames.size() > 8192 ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    std::ranges::fill(frames, StereoFrame{});
    return;
  }
  for (std::size_t offset = 0; offset < frames.size();) {
    const auto count = std::min(max_chunk_frames_, frames.size() - offset);
    auto chunk_context = context;
    chunk_context.project_sample_position += offset;
    chunk_context.planned_loop_wrap = context.planned_loop_wrap && offset == 0;
    processor_->process(frames.subspan(offset, count), chunk_context);
    offset += count;
  }
}

void ChunkedProcessorRuntime::process_with_sidechain(std::span<StereoFrame> frames,
                                                     std::span<const StereoFrame> detector,
                                                     BlockContext context) noexcept {
  if (sidechain_ == nullptr || detector.size() < frames.size()) {
    process(frames, context);
    return;
  }
  if (frames.size() > 8192 ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    std::ranges::fill(frames, StereoFrame{});
    return;
  }
  for (std::size_t offset = 0; offset < frames.size();) {
    const auto count = std::min(max_chunk_frames_, frames.size() - offset);
    auto chunk_context = context;
    chunk_context.project_sample_position += offset;
    chunk_context.planned_loop_wrap = context.planned_loop_wrap && offset == 0;
    sidechain_->process_with_sidechain(frames.subspan(offset, count),
                                       detector.subspan(offset, count), chunk_context);
    offset += count;
  }
}

void ChunkedProcessorRuntime::graph_activated(std::uint64_t revision) noexcept {
  processor_->graph_activated(revision);
}

void ChunkedProcessorRuntime::graph_deactivated(std::uint64_t revision) noexcept {
  processor_->graph_deactivated(revision);
}

} // namespace oscillate::engine
