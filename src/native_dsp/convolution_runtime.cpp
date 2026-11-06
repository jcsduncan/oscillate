#include "oscillate/native_dsp/convolution_runtime.hpp"

#include <algorithm>

namespace oscillate::native_dsp {

void ConvolutionRuntime::process(std::span<engine::StereoFrame> stereo,
                                 engine::BlockContext context) noexcept {
  if (stereo.size() > max_graph_block_frames()) {
    // The graph/device owner discards an out-of-contract buffer. Do not walk
    // an unbounded span on the callback.
    return;
  }
  if (context.state_generation() == 0) {
    std::ranges::fill(stereo, engine::StereoFrame{});
    return;
  }
  for (std::size_t offset = 0; offset < stereo.size(); offset += Convolution::max_block_frames()) {
    processor_->process(
        stereo.subspan(offset, std::min(Convolution::max_block_frames(), stereo.size() - offset)),
        context.state_generation());
  }
}

} // namespace oscillate::native_dsp
