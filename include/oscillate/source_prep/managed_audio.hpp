#pragma once

#include <oscillate/engine/graph.hpp>
#include <oscillate/foundation/error.hpp>

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <vector>

namespace oscillate::persistence {
class ProjectStore;
}
namespace oscillate::project {
struct SourceRecord;
}

namespace oscillate::source_prep {

// A managed source decoded to stereo frames on a worker or the control thread.
// The audio callback never opens or decodes a managed source.
struct DecodedAudio {
  std::vector<engine::StereoFrame> frames;
  std::uint32_t source_rate = 0;
};

// Decode budget (32 MiB of stereo float frames). Callers may impose a smaller
// budget.
inline constexpr std::size_t max_managed_audio_frames = 4'194'304;

// Decodes an open descriptor (it is duplicated, not taken) whose audio must
// match `record`'s encoding, rate, channels and frames: a file being imported,
// read on its worker before it is a managed source. load_managed_audio
// verifies the managed copy first, then decodes it the same way.
[[nodiscard]] foundation::Result<DecodedAudio>
decode_audio_fd(int fd, const project::SourceRecord& record,
                std::size_t max_frames = max_managed_audio_frames,
                const std::stop_token& stop = {});

[[nodiscard]] foundation::Result<DecodedAudio>
load_managed_audio(const persistence::ProjectStore& store, const project::SourceRecord& record,
                   std::size_t max_frames = max_managed_audio_frames,
                   const std::stop_token& stop = {});

} // namespace oscillate::source_prep
