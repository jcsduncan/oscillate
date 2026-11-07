#pragma once

#include "oscillate/application/revision_status.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/project/model.hpp"

#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace oscillate::quick {

// UI-thread transport projection and seek policy. The playback worker and
// device remain owned by ProjectBridge; this class never runs on the callback.
// "Routed" is the RoutedPlayback run; "direct" is the flat
// application::InstrumentPlayback run used when nothing needs routing.
class TransportController final {
public:
  struct PlaybackView {
    bool project_open = false;
    std::uint64_t model_revision = 0;
    // The last audible revision: a graph built at or after it is current.
    std::uint64_t audible_revision = 0;
    bool active = false;
    bool routed_present = false;
    bool routed_stopping = false;
    bool routed_paused = false;
    bool routed_output_streaming = false;
    bool routed_preparing = false;
    bool direct_preparing = false;
    bool direct_playing = false;
    bool audio_edit_stopping = false;
    std::optional<application::RevisionStatus> graph;
    std::optional<foundation::Error> plugin_fault;
  };

  [[nodiscard]] QVariantMap playback_state(const PlaybackView& view) const;
  [[nodiscard]] bool routed_graph_current(const PlaybackView& view) const;
  [[nodiscard]] bool playback_revision_inactive(const PlaybackView& view) const;
  [[nodiscard]] bool playback_paused(const PlaybackView& view) const;
  [[nodiscard]] bool playback_pause_supported(const PlaybackView& view) const;

  enum class PauseAction : std::uint8_t {
    none,
    cancel_deferred,
    toggle_playback,
    pause_routed,
    resume_routed,
    direct_unavailable,
    start_routed,
    device_unavailable,
  };

  struct PauseState {
    bool transport_pending = false;
    bool routed_stopping = false;
    bool deferred = false;
    bool routed_preparing = false;
    bool routed_present = false;
    bool routed_paused = false;
    bool direct_playing = false;
    bool playback_enabled = false;
    bool direct_present = false;
    bool device_available = false;
  };

  [[nodiscard]] PauseAction pause_action(const project::Snapshot* snapshot, PauseState state) const;

  enum class StopAction : std::uint8_t {
    none,
    request_transport_stop,
    request_preparation_stop,
    stop_routed_gracefully,
    stop_routed_immediately,
    stop_direct,
  };

  struct StopState {
    bool transport_pending = false;
    bool routed_preparing = false;
    bool routed_present = false;
    bool routed_paused = false;
    bool direct_playing = false;
    bool audio_edit_pending = false;
  };

  [[nodiscard]] StopAction stop_action(StopState state) const;

  enum class OutputPollAction : std::uint8_t { observe, recover_device, stop_invalid_format };

  [[nodiscard]] OutputPollAction routed_output_action(const audio_io::OutputStatus& output,
                                                      std::uint32_t project_rate,
                                                      std::size_t prepared_block_limit) const;
  [[nodiscard]] OutputPollAction direct_output_action(const audio_io::OutputStatus& output) const;

  // A poll reads engine/device state once on the UI thread, then projects a
  // status. The bridge still owns worker joins, device recovery and stopping.
  struct RoutedPollView {
    const application::RevisionStatus& graph;
    const audio_io::OutputStatus& output;
    std::uint64_t model_revision;
    std::uint64_t audible_revision;
    std::uint64_t completed_sample;
    std::uint64_t source_missing_frames;
    bool edit_pending;
    bool limiter_active;
    const QString& source_underrun_detail;
  };

  [[nodiscard]] QString routed_running_status(const RoutedPollView& view) const;

  struct DirectPollView {
    const application::RevisionStatus& graph;
    const audio_io::OutputStatus& output;
    std::uint64_t completed_sample;
    std::uint64_t source_underruns;
    const QString& audio_edit_failure;
    const QString& source_underrun_detail;
  };

  [[nodiscard]] std::optional<QString> direct_running_status(const DirectPollView& view) const;

  enum class SeekAction : std::uint8_t {
    ignore,
    wait_for_transport,
    seek_routed,
    direct_unavailable,
    park_playhead,
  };

  struct SeekState {
    bool project_open = false;
    bool transport_busy = false;
    bool routed_playback = false;
    bool direct_playing = false;
  };

  [[nodiscard]] double playhead_bar(const project::Snapshot* snapshot, std::uint64_t sample) const;
  [[nodiscard]] double playhead_seconds(const project::Snapshot* snapshot,
                                        std::uint64_t sample) const;
  [[nodiscard]] double elapsed_seconds_at_bar(const project::Snapshot* snapshot,
                                              double zero_based_bar) const;
  [[nodiscard]] std::optional<std::uint64_t> sample_at_bar(const project::Snapshot* snapshot,
                                                           double zero_based_bar) const;
  [[nodiscard]] SeekAction seek_action(std::uint64_t sample, SeekState state) const;
};

} // namespace oscillate::quick
