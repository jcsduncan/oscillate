#pragma once

#include "oscillate/engine/graph.hpp"

#include <cstddef>
#include <memory>

namespace oscillate::engine {

// Adapts a processor's prepared block cap to a larger device callback. The
// wrapped owner is processed serially, with absolute project positions and
// the same processing generation. A wrapped sidechain processor's detector
// input is chunked alongside.
class ChunkedProcessorRuntime final : public SidechainProcessorRuntime {
public:
  [[nodiscard]] static foundation::Result<std::shared_ptr<ChunkedProcessorRuntime>>
  prepare(std::shared_ptr<ProcessorRuntime> processor, std::size_t max_chunk_frames);

  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override;
  // Without a sidechain-capable owner the detector is ignored.
  void process_with_sidechain(std::span<StereoFrame> frames, std::span<const StereoFrame> detector,
                              BlockContext context) noexcept override;
  [[nodiscard]] bool renders_any_split() const noexcept override {
    return processor_->renders_any_split();
  }
  [[nodiscard]] bool contains(const ProcessorRuntime* stage) const noexcept override {
    return stage == this || processor_->contains(stage);
  }
  void graph_activated(std::uint64_t revision) noexcept override;
  void graph_deactivated(std::uint64_t revision) noexcept override;
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override {
    return processor_->control_overlay_revision_independent();
  }
  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept override {
    processor_->control_overlay_rebound(from, to);
  }

private:
  ChunkedProcessorRuntime(std::shared_ptr<ProcessorRuntime> processor,
                          std::size_t max_chunk_frames) noexcept;
  std::shared_ptr<ProcessorRuntime> processor_;
  SidechainProcessorRuntime* sidechain_;
  std::size_t max_chunk_frames_;
};

} // namespace oscillate::engine
