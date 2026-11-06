#pragma once

#include <oscillate/engine/graph.hpp>
#include <oscillate/media/prefetch.hpp>
#include <oscillate/project/model.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace oscillate::application {

// The control sequencer sets steady only after the ring has been primed.
// Clearing it before a seek keeps expected priming silence out of underrun
// accounting. These operations are lock-free and may race a callback boundary.
class PlaybackReadiness {
public:
  void set_steady(bool steady) noexcept { steady_.store(steady, std::memory_order_release); }
  [[nodiscard]] bool steady() const noexcept { return steady_.load(std::memory_order_acquire); }

private:
  static_assert(std::atomic<bool>::is_always_lock_free);
  std::atomic<bool> steady_{false};
};

// Control-thread preparation of one M2 WAV clip source. The caller owns the
// numeric graph instance ID and must keep it stable across edits to this clip.
// The ring is dedicated to this clip and worker priming uses the same transport
// generation passed to GraphPublisher::render_block. Only unwarped, same-rate
// clips whose source range equals their rendered duration are accepted here.
// A missing source record remains a project placeholder, not a render node.
[[nodiscard]] foundation::Result<engine::NodeRequest>
make_clip_request(const project::Snapshot& snapshot, project::ClipId clip_id,
                  std::shared_ptr<media::PrefetchRing> ring,
                  std::shared_ptr<PlaybackReadiness> readiness, std::uint64_t instance_id,
                  std::size_t max_frames);

} // namespace oscillate::application
