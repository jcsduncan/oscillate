#include "transport_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oscillate::quick {

bool TransportController::playback_paused(const PlaybackView& view) const {
  return view.routed_present && !view.routed_stopping && view.routed_paused;
}

bool TransportController::playback_pause_supported(const PlaybackView& view) const {
  return !(view.direct_playing && !view.routed_present);
}

bool TransportController::routed_graph_current(const PlaybackView& view) const {
  if (!view.project_open || !view.routed_present || !view.graph || view.routed_stopping ||
      view.routed_paused || !view.routed_output_streaming)
    return false;
  return view.graph->application == application::PlaybackApplication::active &&
         view.graph->active_graph_revision >= view.audible_revision;
}

bool TransportController::playback_revision_inactive(const PlaybackView& view) const {
  if (!view.project_open || !view.active || !view.graph)
    return false;
  if (!view.routed_present && !view.direct_playing)
    return false;
  return view.graph->active_graph_revision < view.audible_revision ||
         view.graph->application == application::PlaybackApplication::failed;
}

QVariantMap TransportController::playback_state(const PlaybackView& view) const {
  const bool failed =
      view.plugin_fault ||
      (view.graph && view.graph->application == application::PlaybackApplication::failed);
  QString phase = QStringLiteral("stopped");
  if (view.routed_stopping || view.audio_edit_stopping)
    phase = QStringLiteral("stopping");
  else if (playback_paused(view))
    phase = QStringLiteral("paused");
  else if (failed && view.active)
    phase = QStringLiteral("fault");
  else if ((view.routed_preparing || view.direct_preparing) && !view.routed_present &&
           !view.direct_playing)
    phase = QStringLiteral("preparing");
  else if (view.active)
    phase = QStringLiteral("playing");

  QString application_state = QStringLiteral("idle");
  if (view.graph) {
    switch (view.graph->application) {
    case application::PlaybackApplication::pending:
      application_state = QStringLiteral("pending");
      break;
    case application::PlaybackApplication::active:
      application_state = QStringLiteral("active");
      break;
    case application::PlaybackApplication::failed:
      application_state = QStringLiteral("failed");
      break;
    }
  } else if (view.active || view.direct_preparing) {
    application_state = QStringLiteral("pending");
  }
  QString failure;
  if (view.plugin_fault)
    failure = QString::fromStdString(view.plugin_fault->message);
  else if (view.graph && view.graph->failure)
    failure = QString::fromStdString(view.graph->failure->message);
  return {{QStringLiteral("phase"), phase},
          {QStringLiteral("application"), application_state},
          {QStringLiteral("modelRevision"), static_cast<qulonglong>(view.model_revision)},
          {QStringLiteral("activeRevision"),
           static_cast<qulonglong>(view.graph ? view.graph->active_graph_revision : 0)},
          {QStringLiteral("pluginFault"), view.plugin_fault.has_value()},
          {QStringLiteral("failure"), failure}};
}

TransportController::PauseAction
TransportController::pause_action(const project::Snapshot* snapshot, PauseState state) const {
  if (state.transport_pending || state.routed_stopping)
    return PauseAction::none;
  if (state.deferred)
    return PauseAction::cancel_deferred;
  if (state.routed_preparing)
    return PauseAction::toggle_playback;
  if (state.routed_present)
    return state.routed_paused ? PauseAction::resume_routed : PauseAction::pause_routed;
  if (state.direct_playing)
    return PauseAction::direct_unavailable;
  if (snapshot && state.playback_enabled && state.direct_present) {
    const bool audio_track = std::ranges::any_of(
        snapshot->tracks, [](const auto& track) { return !track.clips.empty(); });
    const bool instrument_track = std::ranges::any_of(snapshot->tracks, [](const auto& track) {
      return track.kind == project::TrackKind::instrument;
    });
    if (audio_track && !instrument_track)
      return state.device_available ? PauseAction::start_routed : PauseAction::device_unavailable;
  }
  return PauseAction::toggle_playback;
}

TransportController::StopAction TransportController::stop_action(StopState state) const {
  if (state.transport_pending)
    return StopAction::request_transport_stop;
  if (state.routed_preparing)
    return StopAction::request_preparation_stop;
  if (state.routed_present)
    return state.routed_paused ? StopAction::stop_routed_immediately
                               : StopAction::stop_routed_gracefully;
  if (state.direct_playing || state.audio_edit_pending)
    return StopAction::stop_direct;
  return StopAction::none;
}

TransportController::OutputPollAction
TransportController::routed_output_action(const audio_io::OutputStatus& output,
                                          std::uint32_t project_rate,
                                          std::size_t prepared_block_limit) const {
  if (output.state == audio_io::OutputState::disconnected ||
      output.state == audio_io::OutputState::error)
    return OutputPollAction::recover_device;
  if (output.state == audio_io::OutputState::streaming &&
      ((output.negotiated_client_rate && *output.negotiated_client_rate != project_rate) ||
       (output.observed_block_frames && *output.observed_block_frames > prepared_block_limit)))
    return OutputPollAction::stop_invalid_format;
  return OutputPollAction::observe;
}

TransportController::OutputPollAction
TransportController::direct_output_action(const audio_io::OutputStatus& output) const {
  return output.state == audio_io::OutputState::disconnected ||
                 output.state == audio_io::OutputState::error
             ? OutputPollAction::recover_device
             : OutputPollAction::observe;
}

QString TransportController::routed_running_status(const RoutedPollView& view) const {
  const auto& graph = view.graph;
  const auto& output = view.output;
  if (graph.active_graph_revision == 0)
    return QStringLiteral("Priming routed audio for model revision %1").arg(view.model_revision);
  if (graph.active_graph_revision < view.audible_revision) {
    if (view.edit_pending) {
      return QStringLiteral("Edit revision %1 not active; playing routed revision %2 "
                            "(limiter %3). Pending at an audio block boundary.")
          .arg(view.model_revision)
          .arg(graph.active_graph_revision)
          .arg(view.limiter_active ? QStringLiteral("active") : QStringLiteral("off"));
    }
    const auto reason =
        graph.application == application::PlaybackApplication::failed && graph.failure
            ? QString::fromStdString(graph.failure->message)
            : QStringLiteral("change requires a fresh playback preparation");
    return QStringLiteral("Edit revision %1 saved but not active; playing routed revision %2: "
                          "%3. Stop and Play to apply it, or Undo.")
        .arg(view.model_revision)
        .arg(graph.active_graph_revision)
        .arg(reason);
  }
  if (output.render_failures || output.oversized_blocks || output.invalid_buffers ||
      view.source_missing_frames) {
    return QStringLiteral("Routed output errors: %1 render, %2 oversized, %3 invalid, %4 source "
                          "frames missing%5")
        .arg(output.render_failures)
        .arg(output.oversized_blocks)
        .arg(output.invalid_buffers)
        .arg(view.source_missing_frames)
        .arg(view.source_underrun_detail);
  }
  if (output.state == audio_io::OutputState::streaming) {
    return QStringLiteral("Playing routed revision %1, sample %2%3")
        .arg(graph.active_graph_revision)
        .arg(view.completed_sample)
        .arg(view.limiter_active ? QStringLiteral(" · limiter active") : QString{});
  }
  return QStringLiteral("Preparing routed audio output");
}

std::optional<QString>
TransportController::direct_running_status(const DirectPollView& view) const {
  const auto& graph = view.graph;
  const auto& output = view.output;
  if (graph.application == application::PlaybackApplication::failed && graph.failure) {
    return QStringLiteral("Edit revision %1 not active; playing revision %2: %3")
        .arg(graph.model_revision)
        .arg(graph.active_graph_revision)
        .arg(view.audio_edit_failure.isEmpty() ? QString::fromStdString(graph.failure->message)
                                               : view.audio_edit_failure);
  }
  if (output.render_failures || output.oversized_blocks || output.invalid_buffers ||
      view.source_underruns) {
    return QStringLiteral("Audio output errors: %1 render, %2 oversized, %3 invalid "
                          "buffers, %4 source underruns%5")
        .arg(output.render_failures)
        .arg(output.oversized_blocks)
        .arg(output.invalid_buffers)
        .arg(view.source_underruns)
        .arg(view.source_underrun_detail);
  }
  if (output.state == audio_io::OutputState::streaming) {
    return QStringLiteral("Playing revision %1, sample %2%3")
        .arg(graph.active_graph_revision)
        .arg(view.completed_sample)
        .arg(graph.application == application::PlaybackApplication::pending
                 ? QStringLiteral(" (edit pending)")
                 : QString{});
  }
  return std::nullopt;
}

double TransportController::playhead_bar(const project::Snapshot* snapshot,
                                         std::uint64_t sample) const {
  if (!snapshot || sample > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return 0.0;
  const auto tick =
      snapshot->tempo.sample_to_tick({static_cast<std::int64_t>(sample)}, snapshot->sample_rate);
  if (!tick)
    return 0.0;
  const auto location = snapshot->meter.locate(tick.value());
  if (!location || location.value().bar < 1 ||
      location.value().bar == std::numeric_limits<std::int64_t>::max())
    return 0.0;
  const auto first = snapshot->meter.tick_at({location.value().bar, 1, 0});
  const auto next = snapshot->meter.tick_at({location.value().bar + 1, 1, 0});
  if (!first || !next || next.value().value <= first.value().value)
    return static_cast<double>(location.value().bar - 1);
  return static_cast<double>(location.value().bar - 1) +
         static_cast<double>(tick.value().value - first.value().value) /
             static_cast<double>(next.value().value - first.value().value);
}

double TransportController::playhead_seconds(const project::Snapshot* snapshot,
                                             std::uint64_t sample) const {
  if (!snapshot || snapshot->sample_rate == 0)
    return 0.0;
  return static_cast<double>(sample) / static_cast<double>(snapshot->sample_rate);
}

double TransportController::elapsed_seconds_at_bar(const project::Snapshot* snapshot,
                                                   double zero_based_bar) const {
  if (!snapshot || snapshot->sample_rate == 0)
    return -1.0;
  const auto sample = sample_at_bar(snapshot, zero_based_bar);
  if (!sample)
    return -1.0;
  return static_cast<double>(*sample) / static_cast<double>(snapshot->sample_rate);
}

std::optional<std::uint64_t> TransportController::sample_at_bar(const project::Snapshot* snapshot,
                                                                double zero_based_bar) const {
  if (!snapshot || !std::isfinite(zero_based_bar) || zero_based_bar < 0.0 ||
      zero_based_bar >= static_cast<double>(std::numeric_limits<std::int64_t>::max() - 2))
    return std::nullopt;
  const auto whole = static_cast<std::int64_t>(std::floor(zero_based_bar));
  const auto first = snapshot->meter.tick_at({whole + 1, 1, 0});
  const auto next = snapshot->meter.tick_at({whole + 2, 1, 0});
  if (!first || !next || next.value().value <= first.value().value)
    return std::nullopt;
  const auto fraction = static_cast<long double>(zero_based_bar - static_cast<double>(whole));
  const auto tick = static_cast<long double>(first.value().value) +
                    fraction * static_cast<long double>(next.value().value - first.value().value);
  if (tick < 0.0L || tick >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  const auto sample = snapshot->tempo.tick_to_sample(
      {static_cast<std::int64_t>(std::llround(tick))}, snapshot->sample_rate);
  if (!sample || sample.value().value < 0)
    return std::nullopt;
  return static_cast<std::uint64_t>(sample.value().value);
}

TransportController::SeekAction TransportController::seek_action(std::uint64_t sample,
                                                                 SeekState state) const {
  if (!state.project_open ||
      sample >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return SeekAction::ignore;
  if (state.transport_busy)
    return SeekAction::wait_for_transport;
  if (state.routed_playback)
    return SeekAction::seek_routed;
  if (state.direct_playing)
    return SeekAction::direct_unavailable;
  return SeekAction::park_playhead;
}

} // namespace oscillate::quick
