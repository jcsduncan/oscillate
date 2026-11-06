#pragma once

#include "oscillate/media/prefetch.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/warp_dsp/stream.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace oscillate::processing {

// Where a live processed clip's frames come from.
enum class WarpLiveBacking : std::uint8_t {
  // A validated Q0 LDW2 render for exactly this clip's current inputs.
  cache,
  // A worker-side stream rendering in real time from the verified source.
  stream
};

struct WarpLiveStatus {
  project::WarpMode mode = project::WarpMode::complex;
  WarpLiveBacking backing = WarpLiveBacking::stream;
  warp_dsp::QualityTier tier = warp_dsp::QualityTier::q0;
  std::uint64_t step_downs = 0;
};

// Worker-side source of one processed clip's output frames for live
// playback (ADR-0026 point 8). Output frame 0 is the clip's first rendered
// frame. open() does every allocation, verification and analysis; read_at()
// renders or reads frames in the prefetch worker only. Nothing here may run
// on the audio callback.
//
// A valid Q0 cache render is preferred and is read bit-exactly. Otherwise
// streams for Q0, Q1 and Q2 are built up front; the worker plays Q0 and may
// step down one tier at a time when it falls behind. A step-down takes effect
// at the next read, which restarts the lower tier's stream at that output
// frame, and is counted in status(). Tiers never step back up within one
// reader; a new graph or source revision prepares a new reader at Q0.
class WarpLiveReader {
public:
  static constexpr std::size_t max_read_frames = 4'096;

  // `cache` may be null (no cache lookup). The stop token cancels opening
  // and analysis only; later reads are unaffected by it.
  [[nodiscard]] static foundation::Result<std::unique_ptr<WarpLiveReader>>
  open(const std::filesystem::path& project_root, media::CacheStore* cache,
       const WarpRenderRequest& request, const std::stop_token& stop = {});

  WarpLiveReader(const WarpLiveReader&) = delete;
  WarpLiveReader& operator=(const WarpLiveReader&) = delete;
  ~WarpLiveReader();

  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  // Worker only. Fills output with frames [frame, frame + output.size()),
  // at most max_read_frames, and returns the count (short only at the end).
  // A non-contiguous frame seeks. Source, cache and stream failures fail.
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t frame,
                                                        std::span<media::StereoFrame> output);
  // Worker only. Requests the next lower tier; false when already at Q2 or
  // cache-backed (a cache read cannot fall behind on DSP).
  bool step_down() noexcept;
  // Any thread.
  [[nodiscard]] WarpLiveStatus status() const noexcept;

private:
  WarpLiveReader(project::WarpMode mode, std::uint64_t frames) noexcept
      : mode_(mode), frames_(frames) {}
  [[nodiscard]] foundation::Result<std::size_t> read_stream(std::uint64_t frame,
                                                            std::span<media::StereoFrame> output);

  project::WarpMode mode_;
  std::uint64_t frames_;
  std::optional<WarpRenderCacheReader> cache_;
  std::unique_ptr<ProjectRateSourceView> view_;
  std::unique_ptr<warp_dsp::WarpTimeMap> map_;
  std::array<std::unique_ptr<warp_dsp::WarpStream>, 3> streams_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::size_t active_tier_ = 0; // worker-owned
  bool started_ = false;        // worker-owned: active stream has been started
  std::atomic<std::uint8_t> published_tier_{0};
  std::atomic<std::uint64_t> step_downs_{0};
};

} // namespace oscillate::processing
