#pragma once

#include "oscillate/native_dsp/convolution.hpp"

namespace oscillate::native_dsp {

// Optional graph adapter for a prepared Convolution. Construct/reclaim on the
// control side; one callback exclusively owns process(). A topology-only graph
// change may reuse this runtime to retain its IR history and finite tail.
class ConvolutionRuntime final : public engine::ProcessorRuntime {
public:
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }
  explicit ConvolutionRuntime(std::unique_ptr<Convolution> processor) noexcept
      : processor_(std::move(processor)) {}

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;

  // Control side only, after callback quiescence. A seek/new generation is
  // silent until this reset completes; resetting FFT history in the callback
  // would create an unbounded scheduling spike for a long IR.
  bool reset_after_quiesce(std::uint64_t generation) noexcept {
    return processor_->reset_after_quiesce(generation);
  }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept { return processor_->tail_frames(); }
  [[nodiscard]] static constexpr std::uint32_t latency_frames() noexcept {
    return Convolution::latency_frames();
  }
  [[nodiscard]] static constexpr std::size_t max_graph_block_frames() noexcept { return 8192; }
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept {
    return processor_->guarded_frames_after_quiesce();
  }

private:
  std::unique_ptr<Convolution> processor_;
};

} // namespace oscillate::native_dsp
