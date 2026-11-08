#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <span>
#include <vector>

namespace oscillate::routing {

// The channel-mute lanes of the tracks that key an effect's notes (ADR-0094),
// prepared from one saved revision on the control side. A key note sounds
// only where its track's lane leaves the channel unmuted, as the mixer's
// channel gain and the audio key apply the same lane.
class KeyMuteLanes {
public:
  // Prepares the mute lanes of `tracks`; a revision where none of them has a
  // mute lane prepares nothing.
  [[nodiscard]] static foundation::Result<KeyMuteLanes>
  prepare(const project::Snapshot& snapshot, std::span<const project::TrackId> tracks);

  // Whether `track`'s mute lane mutes it at `sample`; false for a track
  // without one.
  [[nodiscard]] foundation::Result<bool> mutes(project::TrackId track,
                                               timeline::SamplePosition sample) const;

private:
  std::vector<automation_project::PreparedTargetAutomation> lanes_;
};

} // namespace oscillate::routing
