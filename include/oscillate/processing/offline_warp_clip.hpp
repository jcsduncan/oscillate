#pragma once

#include "oscillate/media/prefetch.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/warp_dsp/stream.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace oscillate::processing {

// Offline (export) render of one processed clip, frame-exact, at the
// request's quality tier (Q0 for every export). It reads the verified source
// bytes behind `verified_fd` through the exact project-rate view, runs the
// shared analysis once and renders through the shared stream factory, so an
// export hears the same frames as an offline cache render or an unstepped
// live render. It never consults the render cache: export succeeds from the
// managed source alone. Output frame 0 is the clip's first rendered frame.
// Worker/control side only; never on the audio callback.
class OfflineWarpClip {
public:
  static constexpr std::size_t render_chunk_frames = 8'192;

  // The descriptor is duplicated, not adopted; the caller closes it.
  [[nodiscard]] static foundation::Result<std::unique_ptr<OfflineWarpClip>>
  open(int verified_fd, const WarpRenderRequest& request);

  OfflineWarpClip(const OfflineWarpClip&) = delete;
  OfflineWarpClip& operator=(const OfflineWarpClip&) = delete;
  ~OfflineWarpClip();

  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  // Fills output with frames [frame, frame + output.size()), any length;
  // short only past the clip end. Exports read in order, which streams
  // continuously; a non-contiguous frame restarts the stream there. A source
  // read or stream failure fails the call; it never yields silence.
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t frame,
                                                        std::span<media::StereoFrame> output);

private:
  explicit OfflineWarpClip(std::uint64_t frames) noexcept : frames_(frames) {}

  std::uint64_t frames_;
  std::unique_ptr<ProjectRateSourceView> view_;
  std::unique_ptr<warp_dsp::WarpTimeMap> map_;
  std::unique_ptr<warp_dsp::WarpStream> stream_;
  std::vector<float> left_;
  std::vector<float> right_;
  bool started_ = false;
};

} // namespace oscillate::processing
