#pragma once

#include "oscillate/application/project_session.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/master_assistant.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The master assistant on a project (ADR-0124): renders the mix as it
// enters the master chain, offline and off the audio thread, measures it
// with the Rebalance roles' share of it, plans a master chain toward a
// reference and a loudness target (native_dsp/master_assistant.hpp), and
// renders a level-matched A/B of the proposal against the current master.
// Nothing changes the project until accept(), which is one undo step.
//
// measure(), propose(), measure_reference() and verify() run on a worker;
// operations() and accept() on the control thread; Audition::render on the
// audio callback.
namespace oscillate::application::master_assistant {

namespace core = native_dsp::assistant;

// A part of the song in project frames, [first, last).
struct Range {
  std::uint64_t first = 0;
  std::uint64_t last = 0;
};
// The longest programme the assistant measures: a longer song needs a
// region. Its audio is kept in an unnamed file in the project's cache.
inline constexpr double max_programme_seconds = 30.0 * 60.0;
// A reference track's longest.
inline constexpr double reference_max_seconds = 20.0 * 60.0;
// The A/B plays the loudest stretch of this long.
inline constexpr double audition_seconds = 30.0;

// Called with (fraction 0..1, stage text); false cancels.
using Progress = std::function<bool(double, const std::string&)>;

// The mix entering the master chain (the chain left out and the master
// level at unity, the rebalance kept), measured.
struct Measured {
  std::shared_ptr<const PinnedSnapshot> pinned;
  std::optional<Range> range;
  // The project frame of the programme's first frame.
  std::uint64_t first_frame = 0;
  core::Analysis mix;
  core::RolePowers roles;
  std::shared_ptr<core::Store> programme;
};
[[nodiscard]] foundation::Result<Measured>
measure(PinnedSnapshot pinned, std::optional<Range> range, const Progress& progress);

// A reference track (WAV, or in codec builds AIFF, FLAC or MP3) measured as
// a mix is.
[[nodiscard]] foundation::Result<core::Analysis>
measure_reference(const std::filesystem::path& path, const std::function<bool()>& cancelled);

// Which of a proposal's devices and nudges to accept (empty: all).
struct Selection {
  std::vector<bool> devices;
  std::vector<bool> nudges;
};
[[nodiscard]] bool selected(const std::vector<bool>& flags, std::size_t index) noexcept;

struct Proposed {
  core::Proposal proposal;
  core::Learned learned;
  // The mix and the proposal's output against the reference and goals.
  core::Outcome before;
  core::Outcome predicted;
  // The A/B: the loudest audition_seconds of the programme (its frames),
  // through the current master and through the proposal, and their
  // integrated loudness there.
  core::Window window;
  core::Capture current;
  core::Capture proposed;
  double current_lufs = core::floor_db;
  double proposed_lufs = core::floor_db;
};
// Nudges are planned on the mix as measured; when there are any, the mix
// is rendered again with them before the chain is planned on it, so the
// chain is learned on what it will hear.
[[nodiscard]] foundation::Result<Proposed> propose(const Measured& measured,
                                                   const core::Reference& reference,
                                                   core::Goals goals, const Progress& progress);

// The project operations that accepting `selection` makes: each device
// edits the first device of its engine on the master, else is added in
// chain order (before the devices the proposal places after it), and the
// nudges move the roles' gains in the rebalance (switching it on).
[[nodiscard]] foundation::Result<std::vector<project::Operation>>
operations(const project::Snapshot& snapshot, const core::Proposal& proposal,
           const Selection& selection);
// One undo step. Refused when the mix changed since `measured`.
[[nodiscard]] foundation::Result<void> accept(ProjectSession& session,
                                              const core::Proposal& proposal,
                                              const Selection& selection,
                                              const project::Snapshot& measured);

// The master as it now renders, over the same range.
[[nodiscard]] foundation::Result<core::Analysis>
verify(const PinnedSnapshot& pinned, std::optional<Range> range, const Progress& progress);

// The A/B player: the current master and the proposal over the same window,
// each at its matched gain, looped. Switching sides crossfades over
// crossfade_seconds at the same position, so the comparison is gapless.
// Construction allocates (control); render() allocates nothing and takes no
// lock (callback).
class Audition {
public:
  static constexpr double crossfade_seconds = 0.02;
  Audition(const core::Capture& current, double current_gain_db, const core::Capture& proposed,
           double proposed_gain_db, std::uint32_t sample_rate);
  // Any thread.
  void choose_proposal(bool proposal) noexcept;
  [[nodiscard]] bool proposal_chosen() const noexcept;
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint64_t position() const noexcept;
  // Callback only.
  void render(std::span<engine::StereoFrame> out) noexcept;
  // For a playback output: a StereoRenderSource's render().
  static bool render_source(void* context, std::span<engine::StereoFrame> out,
                            engine::BlockContext block) noexcept;

private:
  std::vector<float> current_left_;
  std::vector<float> current_right_;
  std::vector<float> proposed_left_;
  std::vector<float> proposed_right_;
  std::uint64_t frames_ = 0;
  double step_ = 1.0;
  std::atomic<bool> proposal_{false};
  std::atomic<std::uint64_t> position_{0};
  // Callback state.
  double blend_ = 0.0;
  std::uint64_t at_ = 0;
};

} // namespace oscillate::application::master_assistant
