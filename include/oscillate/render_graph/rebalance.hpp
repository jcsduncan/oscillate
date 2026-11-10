#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/rebalance.hpp"
#include "oscillate/project/mix_roles.hpp"
#include "oscillate/project/snapshot.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// The master rebalance stages of a graph (ADR-0123): one per track, on its
// output after the fader, each at the settings of the role project::mix_roles
// gives its stage (neutral for none, and for every track while the rebalance
// is off). Live playback keeps one set for the session and offers edits to
// the running stages; an offline render builds a set for its snapshot.
// Control side only.
namespace oscillate::render_graph {

// A role's settings as the stage takes them.
[[nodiscard]] native_dsp::RoleTone role_tone(const project::RoleBalance& balance) noexcept;

class RebalanceStages {
public:
  // Follows `snapshot`: each stage already made is offered its target when
  // its role or that role's settings changed. Fails, changing nothing, when
  // a setting cannot be prepared at the snapshot's rate.
  [[nodiscard]] foundation::Result<void> update(const project::Snapshot& snapshot);

  // The stage on `track`'s output, made at its current target the first
  // time; none for a track the last update did not see.
  [[nodiscard]] std::shared_ptr<native_dsp::RoleStage> stage(project::TrackId track);

  // Frames `track`'s stage keeps sounding after its input stops: its EQ's
  // settling time, or zero while it is flat.
  [[nodiscard]] std::uint64_t tail_frames(project::TrackId track) const;

  // From the last update, in its snapshot's track order.
  [[nodiscard]] std::span<const project::TrackRole> roles() const noexcept { return roles_; }

private:
  struct Entry {
    project::MixRole stage = project::MixRole::none;
    std::shared_ptr<native_dsp::RoleStage> runtime;
  };
  [[nodiscard]] const native_dsp::RoleTarget& target(project::MixRole role) const;

  std::uint32_t sample_rate_ = 0;
  // Per role, the tone its target was made from (neutral while off), and
  // the target; then the neutral target.
  std::array<std::optional<native_dsp::RoleTone>, project::mix_role_count> tones_{};
  std::vector<native_dsp::RoleTarget> targets_;
  std::vector<project::TrackRole> roles_;
  std::map<project::TrackId, Entry> entries_;
};

} // namespace oscillate::render_graph
