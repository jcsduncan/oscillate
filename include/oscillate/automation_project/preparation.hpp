#pragma once

#include "oscillate/automation/curve.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::automation_project {

// Immutable control-thread result. It is not a processor binding: the graph
// must publish it with the matching snapshot and apply its values to the target.
// Clip-local ticks/samples are anchored to the rendered clip start. A move
// changes that anchor; a trim retains stored points beyond the active interval
// so an extension can reveal them. UI choices for ambiguous clip edits are
// separate, unimplemented application work.
class PreparedTargetAutomation {
public:
  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  [[nodiscard]] const project::AutomationTarget& target() const noexcept { return target_; }
  [[nodiscard]] std::uint64_t snapshot_revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t tempo_revision() const noexcept { return tempo_revision_; }
  [[nodiscard]] std::uint64_t semantics_key() const noexcept { return semantics_key_; }

  // Caller supplies the same authoritative tick block to every target. No
  // allocation, locks, I/O or mutable project access occurs on this path.
  [[nodiscard]] bool evaluate_block(timeline::SamplePosition first,
                                    std::span<const timeline::TickPosition> ticks,
                                    std::span<double> output) const;
  [[nodiscard]] bool evaluate_block(timeline::SamplePosition first, std::span<double> output) const;

private:
  // A macro lane (ADR-0059) prepared for one mapped parameter: the lane is
  // evaluated in the macro's 0..1, then mapped into the parameter's units.
  struct MacroMapping {
    automation::Warp warp;
    double minimum;
    double maximum;
    double domain_minimum;
    double domain_maximum;
    std::uint32_t steps; // Zero for a continuous parameter.
  };
  void map_macro(std::span<double> output) const noexcept;

  struct ClipRegion {
    project::ClipId clip;
    timeline::SamplePosition begin;
    timeline::SamplePosition end;
    automation::PreparedAutomation prepared;
  };

  PreparedTargetAutomation(project::TrackId track, project::AutomationTarget target,
                           const project::Snapshot& snapshot,
                           automation::PreparedAutomation baseline, std::vector<ClipRegion> clips,
                           std::uint64_t semantics_key,
                           std::optional<MacroMapping> macro = std::nullopt)
      : track_(track), target_(target), revision_(snapshot.revision),
        tempo_revision_(snapshot.tempo.revision()), baseline_(std::move(baseline)),
        clips_(std::move(clips)), semantics_key_(semantics_key), macro_(macro) {}

  project::TrackId track_;
  project::AutomationTarget target_;
  std::uint64_t revision_;
  std::uint64_t tempo_revision_;
  automation::PreparedAutomation baseline_;
  std::vector<ClipRegion> clips_;
  std::uint64_t semantics_key_;
  std::optional<MacroMapping> macro_;

  friend foundation::Result<std::vector<PreparedTargetAutomation>>
  prepare_project_automation(const project::Snapshot&, std::uint64_t);
};

// Builds every saved lane or fails the entire requested revision. In
// particular, a VST3 lane cannot be silently dropped. A macro lane becomes
// one prepared lane per mapped parameter, targeting that parameter, so no
// result targets a macro. Consumers must bind a
// supported parameter event sink or explicitly reject the requested revision.
[[nodiscard]] foundation::Result<std::vector<PreparedTargetAutomation>>
prepare_project_automation(const project::Snapshot& snapshot, std::uint64_t expected_revision);

// The value a macro at `value` (0..1) gives a mapped parameter (ADR-0059):
// the blend from the mapping's minimum to its maximum in the domain's scale,
// on the domain's steps when discrete, within the domain. Sessions writing a
// macro's static value and prepared macro lanes both use this.
[[nodiscard]] double macro_parameter_value(const project::MacroMapping& mapping,
                                           double value) noexcept;

} // namespace oscillate::automation_project
