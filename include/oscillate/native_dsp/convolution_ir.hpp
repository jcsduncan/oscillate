#pragma once

#include "oscillate/native_dsp/convolution.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace oscillate::native_dsp {

// Metadata recorded by the owning asset layer for the original encoded WAV.
// The descriptor is verified against a pinned regular-file descriptor before
// preparing any callback-owned state. This API is control/worker side only.
struct ConvolutionIrIdentity {
  std::string sha256;
  std::uint64_t encoded_bytes = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  std::uint64_t frames = 0;
};

// Caller supplies measured, currently available callback headroom. Zero means
// unknown and fails closed. The initial admission policy permits one instance;
// replacing an existing instance requires the caller to budget both runtimes
// until the predecessor has finished draining and is reclaimed.
struct ConvolutionIrBudget {
  std::uint64_t available_preparation_bytes = 0;
  std::uint32_t available_callback_us = 0;
  std::uint32_t measured_worst_callback_us = 0;
  std::uint32_t active_instances = 0;
  // The cost observation must describe this exact IR and device format.
  // A measurement for another IR, rate or callback size is not transferable.
  std::string measured_ir_sha256;
  std::uint64_t measured_ir_frames = 0;
  std::uint32_t measured_sample_rate = 0;
  std::uint32_t measured_block_frames = 0;
};

struct PreparedConvolutionIr {
  ConvolutionIrIdentity identity;
  std::unique_ptr<Convolution> processor;
};

// Duplicates fd through WavReader. The caller retains ownership of fd. Only
// PCM/float mono or stereo WAV at the project rate is admitted. The entire
// encoded file is hashed before and after decoding; changed files fail closed.
[[nodiscard]] foundation::Result<PreparedConvolutionIr>
prepare_convolution_ir(int fd, const ConvolutionIrIdentity& expected,
                       const ConvolutionIrBudget& budget, std::uint32_t project_rate, float wet);

// Offline rendering has no device callback deadline. It still applies the
// descriptor, metadata, IR duration, and preparation-memory admission limits.
[[nodiscard]] foundation::Result<PreparedConvolutionIr>
prepare_convolution_ir_offline(int fd, const ConvolutionIrIdentity& expected,
                               std::uint64_t available_preparation_bytes,
                               std::uint32_t project_rate, float wet);

} // namespace oscillate::native_dsp
