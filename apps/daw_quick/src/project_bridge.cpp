#include "project_bridge.hpp"
#include "recording_controller.hpp"

#include "anvil_panel_model.hpp"
#include "arrange_controller.hpp"
#include "arrangement_automation_controller.hpp"
#include "browser_preview_controller.hpp"
#include "clay_controller.hpp"
#include "clip_automation_controller.hpp"
#include "clip_tempo_controller.hpp"
#include "clip_warp_controller.hpp"
#include "command_palette.hpp"
#include "computer_keyboard.hpp"
#include "freeze_controller.hpp"
#include "groove_capture_controller.hpp"
#include "harmony_controller.hpp"
#include "history_controller.hpp"
#include "jack_playback_output.hpp"
#include "key_match_controller.hpp"
#include "keyed_list_model.hpp"
#include "knock_panel_model.hpp"
#include "loop_transport.hpp"
#include "macro_controller.hpp"
#include "master_section.hpp"
#include "meter_model.hpp"
#include "mix_check_controller.hpp"
#include "native_synth_audition.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "note_effects_controller.hpp"
#include "note_tools_controller.hpp"
#include "plugin_automation_controller.hpp"
#include "project_bridge_projection.hpp"
#include "rack_library_controller.hpp"
#include "relay_panel_model.hpp"
#include "route_commands.hpp"
#include "saved_processing.hpp"
#include "tempo_map_commands.hpp"
#include "track_automation_controller.hpp"
#include "track_commands.hpp"
#include "ui_contracts.hpp"
#include "vst3_qualification_policy.hpp"
#include "waveform_controller.hpp"
#include "workspace_layout_controller.hpp"

#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/model.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/vst3_catalogue.hpp"
#endif
#include <QDir>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oscillate::quick {
namespace {
// UI-thread polling: playback and meters, a finished save, the monitor run.
constexpr int playback_poll_ms = 50;
constexpr int job_poll_ms = 50;
constexpr int monitor_poll_ms = 20;
// A burst of edits saves once, this long after the last one.
constexpr int background_save_delay_ms = 180;
constexpr int default_autosave_seconds = 120;
constexpr int min_autosave_seconds = 15;
constexpr int max_autosave_seconds = 1800;
constexpr int milliseconds_per_second = 1000;

[[nodiscard]] QString track_heights_key(const project::ProjectId& id) {
  return QStringLiteral("trackRowHeights/%1").arg(QString::fromStdString(id.to_string()));
}

[[nodiscard]] application::PlaybackOutputFactory
metered_jack_factory(application::PlaybackOutputFactory factory,
                     std::shared_ptr<native_dsp::StereoMeter> meter,
                     std::shared_ptr<application::SpectrumMonitor> spectrum,
                     std::shared_ptr<LiveTruePeakMonitor> true_peak,
                     std::shared_ptr<std::atomic<bool>> mono_listen) {
  if (!factory)
    factory = make_jack_playback_output_factory();
  return factory ? make_metered_output_factory(std::move(factory), std::move(meter),
                                               std::move(spectrum), std::move(true_peak),
                                               std::move(mono_listen))
                 : application::PlaybackOutputFactory{};
}

foundation::Result<timeline::TickPosition> timeline_position(const project::Snapshot& snapshot,
                                                             double normalized, bool snap,
                                                             int timeline_bars) {
  if (!std::isfinite(normalized) || normalized > 1.0 ||
      normalized * timeline_bars < -static_cast<double>(project::max_lead_bars)) {
    return foundation::Result<timeline::TickPosition>::failure(
        {foundation::ErrorCode::invalid_argument, "Audio edit is outside the visible timeline"});
  }
  const double bars = normalized * timeline_bars;
  if (bars < 0.0) {
    // Before zero, in the opening meter's bars: the session adds bars
    // before bar 1 for it (ADR-0053).
    const auto& opening = snapshot.meter.events().front();
    const auto beat =
        4 * timeline::ticks_per_quarter / static_cast<std::int64_t>(opening.denominator);
    const auto ticks = bars * static_cast<double>(beat * opening.numerator);
    return foundation::Result<timeline::TickPosition>::success(
        {snap ? std::llround(ticks / static_cast<double>(beat)) * beat
              : static_cast<std::int64_t>(std::llround(ticks))});
  }
  const auto bar_index = std::min(static_cast<int>(std::floor(bars)), timeline_bars - 1);
  const auto first = snapshot.meter.tick_at({bar_index + 1, 1, 0});
  const auto last = snapshot.meter.tick_at({bar_index + 2, 1, 0});
  if (!first || !last) {
    return foundation::Result<timeline::TickPosition>::failure(!first ? first.error()
                                                                      : last.error());
  }
  const auto offset = static_cast<std::int64_t>(std::llround(
      (bars - bar_index) * static_cast<double>(last.value().value - first.value().value)));
  const timeline::TickPosition unsnapped{first.value().value + offset};
  if (!snap) {
    return foundation::Result<timeline::TickPosition>::success(unsnapped);
  }
  const auto location = snapshot.meter.locate(unsnapped);
  if (!location) {
    return foundation::Result<timeline::TickPosition>::failure(location.error());
  }
  const auto beat_start = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
  auto next = snapshot.meter.tick_at(
      {location.value().bar, static_cast<std::uint8_t>(location.value().beat + 1), 0});
  if (!next) {
    next = snapshot.meter.tick_at({location.value().bar + 1, 1, 0});
  }
  if (!beat_start || !next) {
    return foundation::Result<timeline::TickPosition>::failure(!beat_start ? beat_start.error()
                                                                           : next.error());
  }
  return foundation::Result<timeline::TickPosition>::success(
      unsnapped.value - beat_start.value().value <= next.value().value - unsnapped.value
          ? beat_start.value()
          : next.value());
}

} // namespace

ProjectBridge::ProjectBridge(const std::filesystem::path& home_directory, QObject* parent,
                             application::PlaybackOutputFactory output_factory,
                             AudioDeviceDiscovery audio_device_discovery,
                             application::PlaybackOutputFactory jack_output_factory,
                             std::function<bool()> jack_server_probe, Dependencies dependencies)
    : QObject(parent), audio_edit_runner_(dependencies.audio_edit_runner
                                              ? std::move(dependencies.audio_edit_runner)
                                              : WorkerRunner{[](std::function<void()> job) {
                                                  return std::jthread(std::move(job));
                                                }}),
      before_live_edit_handoff_(std::move(dependencies.before_live_edit_handoff)),
      playback_timer_(this), autosave_timer_(this), autosave_poll_timer_(this),
      background_save_timer_(this), background_save_poll_timer_(this),
      projects_root_((home_directory.empty() ? std::filesystem::path(QDir::homePath().toStdString())
                                             : home_directory) /
                     ".oscillate" / "projects") {
  register_ui_contracts();
  workspace_layout_ = std::make_unique<WorkspaceLayoutController>(this);
  vst3_live_qualification_ = verified_vst3_live_qualification();
  if (dependencies.fixed_latency_vst3_binaries)
    vst3_live_qualification_.fixed_latency_binaries =
        std::move(*dependencies.fixed_latency_vst3_binaries);
  start_timers();
  create_audio_output(std::move(output_factory), std::move(jack_output_factory),
                      std::move(jack_server_probe), std::move(audio_device_discovery));
  waveform_ = std::make_unique<WaveformController>(
      WaveformController::Host{
          .session = [this] { return session_.get(); },
          .refresh = [this] { refresh(); },
          .refresh_waveforms = [this] { refresh_clip_waveforms(); },
          .status = [this](QString message) { set_status(std::move(message)); }},
      this);
  track_model_ = new KeyedListModel({QStringLiteral("clips")}, this);
  create_meter_model();
  cache_ui_ = std::make_unique<CacheUiController>(projects_root_.parent_path(), this);
  key_review_ = std::make_unique<KeyReviewController>(KeyReviewController::Analyzer{}, this);
  connect(key_review_.get(), &KeyReviewController::projectRevisionChanged, this,
          &ProjectBridge::refresh);
  create_clip_controllers();
  create_mix_controllers();
  create_note_controllers();
  create_song_controllers();
  create_job_controllers();
  create_lifecycle();
  // Whether a chain may be edited depends on playback, export, import, the
  // VST3 browser, device discovery and media repair: re-announce it
  // whenever one changes.
  for (const auto signal : {&ProjectBridge::playbackChanged, &ProjectBridge::projectChanged,
                            &ProjectBridge::insertChainsChanged})
    connect(this, signal, this, &ProjectBridge::insertChainEditEnabledChanged);
  connect(exports_.get(), &ExportController::changed, this,
          &ProjectBridge::insertChainEditEnabledChanged);
  connect(imports_.get(), &ImportController::changed, this,
          &ProjectBridge::insertChainEditEnabledChanged);
  connect(vst3_browser_.get(), &Vst3BrowserController::changed, this,
          &ProjectBridge::insertChainEditEnabledChanged);
  connect(audio_output_.get(), &AudioOutputController::audioDevicesChanged, this,
          &ProjectBridge::insertChainEditEnabledChanged);
  connect(managed_media_.get(), &ManagedMediaController::changed, this,
          &ProjectBridge::insertChainEditEnabledChanged);
  log_controller_statuses();
  set_status(QStringLiteral("Choose a project to begin"));
  monitor_timer_.setInterval(monitor_poll_ms);
  connect(&monitor_timer_, &QTimer::timeout, this, &ProjectBridge::poll_monitor);
  create_live_input();
}

void ProjectBridge::start_timers() {
  playback_timer_.setInterval(playback_poll_ms);
  connect(&playback_timer_, &QTimer::timeout, this, &ProjectBridge::poll_playback);
  // Connected second, so each tick's meter snapshot follows its playback poll
  // (including the poll that stops the timer and retires the run).
  connect(&playback_timer_, &QTimer::timeout, this, &ProjectBridge::poll_meters);
  const int saved_autosave_interval =
      QSettings{}
          .value(QStringLiteral("autosaveIntervalSeconds"), default_autosave_seconds)
          .toInt();
  autosave_interval_seconds_ =
      saved_autosave_interval == 0 || (saved_autosave_interval >= min_autosave_seconds &&
                                       saved_autosave_interval <= max_autosave_seconds)
          ? saved_autosave_interval
          : default_autosave_seconds;
  autosave_timer_.setInterval(std::max(1, autosave_interval_seconds_) * milliseconds_per_second);
  connect(&autosave_timer_, &QTimer::timeout, this, &ProjectBridge::start_autosave);
  autosave_poll_timer_.setInterval(job_poll_ms);
  connect(&autosave_poll_timer_, &QTimer::timeout, this, &ProjectBridge::poll_autosave);
  background_save_timer_.setSingleShot(true);
  background_save_timer_.setInterval(background_save_delay_ms);
  connect(&background_save_timer_, &QTimer::timeout, this, &ProjectBridge::start_background_save);
  background_save_poll_timer_.setInterval(job_poll_ms);
  connect(&background_save_poll_timer_, &QTimer::timeout, this,
          &ProjectBridge::poll_background_save);
}

void ProjectBridge::create_audio_output(application::PlaybackOutputFactory output_factory,
                                        application::PlaybackOutputFactory jack_output_factory,
                                        std::function<bool()> jack_server_probe,
                                        AudioDeviceDiscovery discovery) {
  audio_output_ = std::make_unique<AudioOutputController>(
      AudioOutputController::Host{
          .selection_allowed =
              [this] {
                return !playback_busy() && !export_busy() && !import_busy() &&
                       !close_after_playback_ && !closing_after_export() &&
                       !imports_->audio().closing();
              },
          .observed_block_frames = [this]() -> std::optional<std::uint32_t> {
            if (routed_playback_ && !routed_stop_completion_)
              return routed_playback_->output_status().observed_block_frames;
            if (playback_ && playback_->playing())
              return playback_->output_status().observed_block_frames;
            return std::nullopt;
          },
          .sample_rate = [this]() -> std::optional<std::uint32_t> {
            if (!session_)
              return std::nullopt;
            return session_->snapshot().sample_rate;
          },
          .backend_changed =
              [this] {
                if (session_)
                  playback_ = std::make_unique<application::InstrumentPlayback>(
                      *session_, audio_output_->factory(), audio_output_->buffer_frames());
              },
          .apply_buffer_frames = [this](std::uint32_t frames) -> foundation::Result<void> {
            if (!playback_)
              return foundation::Result<void>::success();
            return playback_->set_requested_buffer_frames(frames);
          },
          .settings_changed = [this] { emit playbackChanged(); }},
      make_metered_output_factory(std::move(output_factory), meter_, spectrum_, true_peak_,
                                  mono_listen_),
      metered_jack_factory(std::move(jack_output_factory), meter_, spectrum_, true_peak_,
                           mono_listen_),
      jack_server_probe ? std::move(jack_server_probe)
                        : std::function<bool()>{jack_server_available},
      std::move(discovery), this);
  connect(this, &ProjectBridge::playbackChanged, audio_output_.get(),
          &AudioOutputController::notify_playback);
}

void ProjectBridge::create_meter_model() {
  meter_model_ = std::make_unique<MeterModel>(
      [this](const QString& id) -> std::optional<MeterReading> {
        if (id.isEmpty()) {
          const auto reading = meter_->latest();
          if (!reading || reading->transport_generation == 0)
            return std::nullopt;
          return MeterReading{reading->left_peak, reading->right_peak, reading->left_rms,
                              reading->right_rms, reading->clipped};
        }
        const auto track = project::TrackId::parse(id.toStdString());
        if (!track || !track_meters_live())
          return std::nullopt;
        const auto reading = live_run()->track_meter(track.value());
        if (!reading || reading->transport_generation == 0)
          return std::nullopt;
        return MeterReading{reading->left_peak, reading->right_peak, reading->left_rms,
                            reading->right_rms, reading->clipped};
      },
      this);
}

// Audio clip editing: automation, tempo, warp, the browser's previews and
// the loop.
void ProjectBridge::create_clip_controllers() {
  const auto session = [this] { return session_.get(); };
  const auto quiet = [this] { return edit_gate_open(); };
  const auto report = [this](QString message) { set_status(std::move(message)); };
  const auto commit = [this] { commit_edit(); };
  const auto commit_audio = [this] {
    refresh();
    submit_audio_clip_edit();
  };
  clip_automation_ =
      std::make_unique<ClipAutomationController>(session, quiet, report, commit, this);
  arrangement_automation_ =
      std::make_unique<ArrangementAutomationController>(session, quiet, report, commit, this);
  clip_tempo_ = std::make_unique<ClipTempoController>(
      session, [this] { return waveform_->root(); }, quiet, report, commit_audio,
      TempoWorker::Analyzer{}, this,
      [this](project::SourceId source, project::SourceRange range, std::size_t columns) {
        return waveform_->view_range(source, range, columns);
      });
  // Each clip's tempo strip reads its own row's `tempo` role.
  connect(clip_tempo_.get(), &ClipTempoController::changed, this,
          &ProjectBridge::publish_clip_tempo);
  create_browser_previews();
  clip_warp_ = std::make_unique<ClipWarpController>(
      session, [this] { return waveform_->root(); }, quiet, report, commit_audio,
      [this](const project::Clip& clip, std::size_t columns) {
        return waveform_->view_for_warp(clip, columns);
      },
      [this] { return warp_playback(); },
      [this](const project::SourceRecord& source) -> std::optional<analysis::TempoEstimate> {
        return clip_tempo_->estimate_for(source);
      },
      this);
  loop_transport_ = std::make_unique<LoopTransport>(
      [this]() -> const project::Snapshot* { return session_ ? &session_->snapshot() : nullptr; },
      [this] {
        return routed_playback_ && !routed_stop_completion_ && !routed_transport_completion_
                   ? routed_playback_.get()
                   : nullptr;
      },
      [this] {
        return !routed_completion_ && !routed_stop_completion_ && !routed_transport_completion_ &&
               !audio_edit_completion_ && !playback_completion_;
      },
      [this](std::uint64_t sample) { seek_to_sample(static_cast<qulonglong>(sample)); }, this);
}

WarpPlaybackObservation ProjectBridge::warp_playback() const {
  WarpPlaybackObservation observed;
  if (routed_playback_ && !routed_stop_completion_) {
    const auto status = routed_playback_->status();
    observed.path = WarpPlaybackObservation::Path::routed;
    observed.active_revision = status.active_graph_revision;
    observed.edit_pending = routed_playback_->edit_pending();
    if (status.application == application::PlaybackApplication::failed && status.failure)
      observed.failure = QString::fromStdString(status.failure->message);
    if (const auto failure = routed_playback_->source_failure())
      observed.failure = QString::fromStdString(failure->message);
    observed.processed = routed_playback_->processed_clip_status();
  } else if (playback_ && playback_->playing()) {
    observed.path = WarpPlaybackObservation::Path::direct;
    observed.active_revision = playback_->status().model_revision;
  }
  return observed;
}

// Tracks, routing, automation, history and the master section.
void ProjectBridge::create_mix_controllers() {
  const auto session = [this] { return session_.get(); };
  const auto quiet = [this] { return edit_gate_open(); };
  const auto report = [this](QString message) { set_status(std::move(message)); };
  const auto commit = [this] { commit_edit(); };
  track_automation_ =
      std::make_unique<TrackAutomationController>(session, quiet, report, commit, this);
  plugin_automation_ = std::make_unique<PluginAutomationController>(
      session, quiet, report, commit,
      [this]() -> std::optional<std::uint64_t> {
        if (routed_playback_ && !routed_stop_completion_)
          return routed_playback_->status().active_graph_revision;
        if (playback_ && playback_->playing())
          return playback_->status().active_graph_revision;
        if (playback_active())
          return 0;
        return std::nullopt;
      },
      [](const project::Snapshot& snapshot, const project::AutomationLane& lane) {
        return detail::automation_lane_projection(snapshot, lane,
                                                  project::max_automation_points_per_lane);
      },
      this);
  connect(this, &ProjectBridge::playbackChanged, plugin_automation_.get(),
          &PluginAutomationController::refresh);
  route_commands_ = std::make_unique<RouteCommands>(
      session, [this] { return route_edit_allowed(); }, report, commit, this);
  track_commands_ = std::make_unique<TrackCommands>(
      session,
      [this] {
        if (control_busy()) {
          set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                          : QStringLiteral("Wait for the audio edit to finish"));
          return false;
        }
        if (!session_) {
          set_status(QStringLiteral("Open a project first"));
          return false;
        }
        return true;
      },
      [this] { return route_edit_allowed(); }, report, commit,
      [this](int index) { emit trackCreated(index); }, this);
  history_ = std::make_unique<HistoryController>(
      session,
      [this] {
        // The same rules as Ctrl+Z.
        if (control_busy()) {
          set_status(managed_media_busy()
                         ? QStringLiteral("Finish media verification before undo")
                         : QStringLiteral("Wait for the audio edit to finish before undo"));
          return false;
        }
        return session_ != nullptr;
      },
      report, commit, this);
  connect(this, &ProjectBridge::revisionChanged, history_.get(), &HistoryController::refresh);
  connect(this, &ProjectBridge::projectChanged, history_.get(), &HistoryController::refresh);
  command_palette_ = std::make_unique<CommandPalette>(this);
  master_section_ = std::make_unique<MasterSection>(true_peak_, mono_listen_, this);
  connect(this, &ProjectBridge::metersChanged, master_section_.get(), &MasterSection::poll);
  create_macro_controllers();
}

// The piano roll's tools, note effects, grooves, Clay and freezing.
void ProjectBridge::create_note_controllers() {
  const auto session = [this] { return session_.get(); };
  const auto can_edit = [this] { return edit_allowed(); };
  const auto report = [this](QString message) { set_status(std::move(message)); };
  const auto commit = [this] { commit_edit(); };
  note_tools_ = std::make_unique<NoteToolsController>(session, can_edit, report, commit, this);
  note_tools_->set_audition(
      [this](const QString& clip_id, const QString& note_id, std::uint8_t velocity) {
        audition_edited_note(clip_id, note_id, velocity);
      });
  groove_capture_ = std::make_unique<GrooveCaptureController>(
      session,
      [this](persistence::GrooveTemplate groove) {
        return note_tools_->keep_groove(std::move(groove));
      },
      report, this);
  follow_project(groove_capture_.get(), &GrooveCaptureController::project_changed);
  clay_ = std::make_unique<ClayController>(
      session, can_edit, report, commit,
      [this](const QString& name) { return note_tools_->find_groove(name); }, [this] { undo(); },
      [this] { redo(); }, this);
  follow_project(clay_.get(), &ClayController::project_changed);
  note_effects_ = std::make_unique<NoteEffectsController>(
      session, can_edit, report, commit, [this] { submit_playback_edit(); },
      [this] { schedule_background_save(); },
      [this](NativeSynthDeviceController& controller) { share_macro_map(controller); }, this);
  freeze_commands_ = std::make_unique<FreezeController>(session, can_edit, report, commit, this);
}

bool ProjectBridge::guarded_edit(const QString& refusal) {
  if (edit_allowed())
    return true;
  set_status(refusal);
  return false;
}

// Mix Check, arranging, harmony and the tempo map.
void ProjectBridge::create_song_controllers() {
  const auto session = [this] { return session_.get(); };
  const auto commit = [this] { commit_edit(); };
  const auto guarded = [this] {
    return guarded_edit(QStringLiteral("Finish the current project operation first"));
  };
  mix_check_ = std::make_unique<MixCheckController>(
      MixCheckController::Hooks{
          .session = session,
          .blocked = [this]() -> QString {
            if (playback_active())
              return QStringLiteral("Stop playback to run Mix Check");
            if (export_busy() || realtime_export_busy())
              return QStringLiteral("Wait for the export to finish before Mix Check");
            if (control_busy() || playback_completion_ || import_busy())
              return QStringLiteral("Finish the current project operation before Mix Check");
            return {};
          },
          .can_edit = guarded,
          .commit = commit,
          .project_name = [this] { return project_name(); },
          .set_solo =
              [this](project::TrackId track, bool soloed) {
                set_track_soloed(QString::fromStdString(track.to_string()), soloed);
              },
          .play_from =
              [this](std::uint64_t frame) {
                seek_to_sample(static_cast<qulonglong>(frame));
                if (!playback_active())
                  toggle_playback();
              },
          .stop = [this] { stop_playback(); }},
      this);
  // A new revision may make the check stale; another project resets it.
  follow_project(mix_check_.get(), &MixCheckController::project_changed);
  arrange_ = std::make_unique<ArrangeController>(
      ArrangeController::Hooks{.session = session,
                               .can_edit = guarded,
                               .commit = commit,
                               .loop = [this]() -> std::optional<std::pair<int, int>> {
                                 if (!loop_transport_ || !loop_transport_->enabled())
                                   return std::nullopt;
                                 return std::pair{loop_transport_->start_bar(),
                                                  loop_transport_->end_bar()};
                               }},
      this);
  follow_project(arrange_.get(), &ArrangeController::project_changed);
  harmony_ = std::make_unique<HarmonyController>(
      HarmonyController::Hooks{.session = session, .can_edit = guarded, .commit = commit}, this);
  follow_project(harmony_.get(), &HarmonyController::project_changed);
  tempo_map_commands_ = std::make_unique<TempoMapCommands>(
      session,
      [this] {
        return guarded_edit(
            QStringLiteral("Finish the current project operation before editing tempo"));
      },
      [this](QString message) { set_status(std::move(message)); }, commit,
      [this] { publish_tempo_map(); });
}

// The project's background jobs: media repair, import, collection, export
// and the VST3 browser and editor.
void ProjectBridge::create_job_controllers() {
  managed_media_ = std::make_unique<ManagedMediaController>(
      [this] { return managed_media_repair_enabled(); }, this);
  connect(this, &ProjectBridge::playbackChanged, managed_media_.get(),
          &ManagedMediaController::notify_repair_enabled);
  imports_ = std::make_unique<ImportController>(
      ImportController::Host{.session = [this] { return session_.get(); },
                             .idle = [this] { return jobs_idle(); },
                             .control_busy = [this] { return control_busy(); },
                             .started =
                                 [this] {
                                   exports_->clear_status();
                                   emit playbackChanged();
                                 },
                             .refresh = [this] { refresh(); },
                             .submit_playback_edit = [this] { submit_playback_edit(); },
                             .track_created = [this](int index) { emit trackCreated(index); },
                             .revision = [this] { return static_cast<std::uint64_t>(revision_); },
                             .status = [this](QString message) { set_status(std::move(message)); }},
      this);
  collection_ = std::make_unique<ProjectCollectionController>(this);
  connect(&imports_->audio(), &AudioImportController::completed, this,
          [this](int created_index, bool refresh_project) {
            // The dropped clips are set up once their tempo is known.
            // So is their key, when the song has one.
            if (session_ && created_index >= 0 &&
                created_index < static_cast<int>(session_->snapshot().tracks.size())) {
              for (const auto& clip :
                   session_->snapshot().tracks[static_cast<std::size_t>(created_index)].clips) {
                clip_tempo_->dropped(clip.id);
                key_match_->dropped(clip.id);
              }
            }
            if (refresh_project)
              refresh();
            if (created_index >= 0)
              emit trackCreated(created_index);
            if (!imports_->audio().closing())
              set_status(imports_->import_status());
            emit playbackChanged();
            exports_->notify();
          });
  connect(&imports_->audio(), &AudioImportController::readyToClose, this,
          &ProjectBridge::readyToClose);
  connect(collection_.get(), &ProjectCollectionController::changed, this,
          &ProjectBridge::collectionChanged);
  connect(collection_.get(), &ProjectCollectionController::finished, this, [this](bool success) {
    if (success)
      refresh_available_projects();
    set_status(collection_->status());
  });
  connect(managed_media_.get(), &ManagedMediaController::started, this, [this] {
    emit playbackChanged();
    imports_->notify();
    exports_->notify();
  });
  connect(managed_media_.get(), &ManagedMediaController::finished, this,
          [this](bool changed, bool external) {
            if (changed) {
              waveform_->invalidate();
              refresh();
              set_playback_status(
                  external ? QStringLiteral("Playback stopped; review external relink status "
                                            "in Project Media")
                           : QStringLiteral("Playback stopped; repaired media is available on "
                                            "the next Play"));
            }
            set_status(managed_media_->status());
            emit playbackChanged();
            imports_->notify();
            exports_->notify();
          });
  connect(managed_media_.get(), &ManagedMediaController::readyToClose, this,
          &ProjectBridge::readyToClose);
  exports_ = std::make_unique<ExportController>(
      ExportController::Host{.session = [this] { return session_.get(); },
                             .idle = [this] { return jobs_idle(); },
                             .project_name = [this] { return project_name_; },
                             .project_root = [this] { return waveform_root_; },
                             .buffer_frames = [this] { return audio_output_->buffer_frames(); },
                             .live_qualification = [this] { return vst3_live_qualification_; },
                             .busy_changed = [this] { emit playbackChanged(); }},
      this);
  connect(exports_.get(), &ExportController::readyToClose, this, &ProjectBridge::readyToClose);
  connect(this, &ProjectBridge::projectChanged, exports_.get(), &ExportController::notify);
  create_vst3_controllers();
}

void ProjectBridge::create_vst3_controllers() {
  vst3_browser_ = std::make_unique<Vst3BrowserController>(
      Vst3BrowserController::Host{
          .session = [this] { return session_.get(); },
          .project_generation = [this] { return project_generation_; },
          .revision = [this] { return static_cast<std::uint64_t>(revision_); },
          .jobs_busy = [this] { return control_busy() || import_busy() || export_busy(); },
          .playback_busy = [this] { return playback_busy(); },
          .commit = [this] { commit_edit(); },
          .refresh = [this] { refresh(); },
          .playback_status = [this](QString message) { set_playback_status(std::move(message)); },
          .track_created = [this](int index) { emit trackCreated(index); },
          .status = [this](QString message) { set_status(std::move(message)); }},
      this);
  connect(vst3_browser_.get(), &Vst3BrowserController::scanStoppedForClose, this, [this] {
    if (request_close())
      emit readyToClose();
  });
  vst3_editor_ = std::make_unique<Vst3EditorController>(
      Vst3EditorController::Host{
          .session = [this] { return session_.get(); },
          .control_busy = [this] { return control_busy(); },
          .transfers_busy = [this] { return export_busy() || import_busy(); },
          .playback_busy = [this] { return playback_busy(); },
          .browser_busy = [this] { return vst3_browser_busy(); },
          .routed = [this] { return routed_playback_.get(); },
          .routed_settled =
              [this] { return !routed_stop_completion_ && !routed_transport_completion_; },
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
          .catalogue = [this]() -> const plugins::Vst3CatalogueSnapshot& {
            return vst3_browser_->catalogue();
          },
#endif
          .commit = [this] { commit_edit(); },
          .schedule_save = [this] { schedule_background_save(); },
          .status = [this](QString message) { set_status(std::move(message)); }},
      *plugin_automation_, this);
}

void ProjectBridge::create_lifecycle() {
  lifecycle_ = std::make_unique<ProjectLifecycleController>(
      projects_root_,
      ProjectLifecycleController::Host{
          .session = [this] { return session_.get(); },
          .current_root = [this] { return waveform_->root(); },
          .project_change_block_reason =
              [this] {
                if (autosave_job_.running())
                  return QStringLiteral("Wait for autosave to finish before changing projects");
                if (!playback_busy() && !export_busy() && !import_busy())
                  return QString{};
                return managed_media_busy()
                           ? QStringLiteral("Finish media verification before changing projects")
                       : import_busy() ? QStringLiteral("Finish import before changing projects")
                       : export_busy() ? QStringLiteral("Finish export before changing projects")
                                       : QStringLiteral("Stop playback before changing projects");
              },
          .save_block_reason =
              [this] {
                if (!control_busy())
                  return QString{};
                return managed_media_busy()
                           ? QStringLiteral("Finish media verification before saving")
                           : QStringLiteral("Wait for the audio edit to finish before saving");
              },
          .finish_in_flight_saves = [this] { finish_in_flight_saves(); },
          .named_revision_enabled =
              [this] {
                return session_ && !playback_busy() && !export_busy() && !import_busy() &&
                       !vst3_browser_busy() && !close_after_playback_ && !closing_after_export() &&
                       !imports_->audio().closing();
              },
          .install_project = [this](auto session,
                                    const auto& root) { set_project(std::move(session), root); },
          .status = [this](QString message) { set_status(std::move(message)); },
          .available_projects_changed = [this] { emit availableProjectsChanged(); },
          .recovery_choice_changed = [this] { emit recoveryChoiceChanged(); },
          .named_revisions_changed = [this] { emit namedRevisionsChanged(); },
          .saved =
              [this] {
                set_save_problem({});
                waveform_->retry_failures();
              },
          .restored =
              [this] {
                ++project_generation_;
                select_audio_clip({});
                waveform_->invalidate();
                meter_->invalidate_after_quiesce();
                true_peak_->invalidate_after_quiesce();
                refresh_meters();
                refresh();
                emit namedRevisionApplied();
                set_playback_status(
                    QStringLiteral("Playback stopped; restored model revision %1 will be "
                                   "prepared on the next Play")
                        .arg(revision_));
              },
          .revision = [this] { return revision_; }});
}

// MIDI recording and the computer keyboard as a musical keyboard.
void ProjectBridge::create_live_input() {
  make_recording();
  connect(this, &ProjectBridge::projectChanged, this, [this] {
    click_maps_.reset();
    publish_click_schedule();
  });
  computer_keyboard_ = std::make_unique<ComputerKeyboard>(
      [this](int key, int velocity) {
        if (selected_track_id_.isEmpty()) {
          set_status(QStringLiteral("Select an instrument track to play it from the keyboard"));
          return;
        }
        play_note(selected_track_id_, key, velocity);
        recording_->note_on(selected_track_id_, key, velocity);
      },
      [this](int key) {
        release_note(key);
        recording_->note_off(key);
      });
  // In key follows the project key.
  connect(this, &ProjectBridge::projectKeyChanged, this, [this] {
    std::vector<int> scale;
    for (const auto& pitch : project_scale())
      scale.push_back(pitch.toInt());
    computer_keyboard_->set_scale(scale, project_key());
  });
}

void ProjectBridge::log_controller_statuses() {
  connect(exports_.get(), &ExportController::changed, this,
          [this] { status_log_.note("export", exports_->export_status()); });
  connect(imports_.get(), &ImportController::changed, this,
          [this] { status_log_.note("import", imports_->import_status()); });
  connect(vst3_browser_.get(), &Vst3BrowserController::changed, this,
          [this] { status_log_.note("vst3", vst3_browser_->status()); });
  connect(audio_output_.get(), &AudioOutputController::audioDevicesChanged, this,
          [this] { status_log_.note("audio output", audio_output_->audio_device_status()); });
  connect(managed_media_.get(), &ManagedMediaController::changed, this,
          [this] { status_log_.note("media", managed_media_->status()); });
  connect(collection_.get(), &ProjectCollectionController::changed, this,
          [this] { status_log_.note("collection", collection_->status()); });
  connect(this, &ProjectBridge::playbackChanged, this,
          [this] { status_log_.note("playback", playback_status_); });
}

bool ProjectBridge::edit_gate_open() const {
  return !control_busy() && !playback_completion_ && !export_busy() && !import_busy();
}

bool ProjectBridge::edit_allowed() const { return session_ && edit_gate_open(); }

void ProjectBridge::commit_edit() {
  refresh();
  submit_playback_edit();
}

bool ProjectBridge::jobs_idle() const {
  return !playback_busy() && !import_busy() && !managed_media_busy() && !close_after_playback_ &&
         !imports_->audio().closing() && !exports_->export_busy() && !exports_->closing();
}

bool ProjectBridge::import_busy() const { return imports_->import_busy(); }
bool ProjectBridge::export_busy() const { return exports_->export_busy(); }
bool ProjectBridge::realtime_export_busy() const { return exports_->realtime_export_busy(); }
bool ProjectBridge::closing_after_export() const { return exports_->closing(); }
bool ProjectBridge::vst3_browser_busy() const { return vst3_browser_->busy(); }

QObject* ProjectBridge::computer_keyboard_object() const { return computer_keyboard_.get(); }

ProjectBridge::~ProjectBridge() {
  background_save_timer_.stop();
  autosave_timer_.stop();
  autosave_job_.wait();
  background_save_job_.wait();
  if (auto finished = background_save_job_.take();
      session_ && finished && finished->result &&
      finished->result.value().status == persistence::SaveStatus::committed &&
      background_save_ticket_.project == session_->snapshot().id)
    session_->saving().finish_background_save(background_save_ticket_.revision,
                                              finished->result.value());
  // The normal close path waits for the worker and reports failures. This is
  // a last-chance fallback for hosts that destroy the bridge directly.
  if (session_ && session_->saving().has_unsaved_changes())
    (void)session_->saving().save();
}

QObject* ProjectBridge::clip_automation() const noexcept { return clip_automation_.get(); }
QObject* ProjectBridge::arrangement_automation() const noexcept {
  return arrangement_automation_.get();
}
QObject* ProjectBridge::clip_warp() const noexcept { return clip_warp_.get(); }

QObject* ProjectBridge::clip_tempo() const noexcept { return clip_tempo_.get(); }
QObject* ProjectBridge::loop_transport() const noexcept { return loop_transport_.get(); }
QObject* ProjectBridge::track_automation() const noexcept { return track_automation_.get(); }
QObject* ProjectBridge::plugin_automation() const noexcept { return plugin_automation_.get(); }
QObject* ProjectBridge::route_commands() const noexcept { return route_commands_.get(); }
QObject* ProjectBridge::track_commands() const noexcept { return track_commands_.get(); }
QObject* ProjectBridge::history() const noexcept { return history_.get(); }
QObject* ProjectBridge::command_palette() const noexcept { return command_palette_.get(); }
QObject* ProjectBridge::master_section() const noexcept { return master_section_.get(); }
QObject* ProjectBridge::note_tools() const noexcept { return note_tools_.get(); }
QObject* ProjectBridge::note_effects() const noexcept { return note_effects_.get(); }
QObject* ProjectBridge::freeze_commands() const noexcept { return freeze_commands_.get(); }
QObject* ProjectBridge::mix_check() const noexcept { return mix_check_.get(); }
QObject* ProjectBridge::arrange() const noexcept { return arrange_.get(); }
QObject* ProjectBridge::harmony() const noexcept { return harmony_.get(); }
QObject* ProjectBridge::groove_capture() const noexcept { return groove_capture_.get(); }
QObject* ProjectBridge::clay() const noexcept { return clay_.get(); }
QObject* ProjectBridge::workspace_layout() const noexcept { return workspace_layout_.get(); }

bool ProjectBridge::ready_to_change_project() {
  if (background_save_job_.running()) {
    set_status(QStringLiteral("Wait for project save to finish before changing projects"));
    return false;
  }
  if (autosave_job_.running()) {
    set_status(QStringLiteral("Wait for autosave to finish before changing projects"));
    return false;
  }
  if (playback_busy() || export_busy() || import_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before changing projects")
               : import_busy() ? QStringLiteral("Finish import before changing projects")
               : export_busy() ? QStringLiteral("Finish export before changing projects")
                               : QStringLiteral("Stop playback before changing projects"));
    return false;
  }
  if (!vst3_editor_->commit_native_editor())
    return false;
  return save_current_project_if_dirty();
}

// Everything that belongs to the open project, stopped and let go.
void ProjectBridge::release_project() {
  background_save_timer_.stop();
  background_save_pending_ = false;
  vst3_editor_->discard_vst3_insert_editor();
  close_native_synth_editor();
  close_native_effect_editor();
  clear_device_strips();
  plugin_automation_->reset_project();
  key_review_->set_project(nullptr, {});
  playback_.reset();
  routed_playback_.reset();
  play_after_monitor_ = false;
  if (monitor_completion_) {
    monitor_completion_->stop_requested.store(true, std::memory_order_release);
    if (monitor_worker_.joinable())
      monitor_worker_.join();
    if (monitor_completion_->result && *monitor_completion_->result)
      (void)monitor_completion_->result->value()->stop_and_retire();
    monitor_completion_.reset();
  }
  stop_monitor_now();
  meter_->invalidate_after_quiesce();
  true_peak_->invalidate_after_quiesce();
  playhead_sample_ = 0;
}

void ProjectBridge::close_project() {
  if (!session_ || !ready_to_change_project())
    return;
  release_project();
  autosave_timer_.stop();
  managed_media_->set_project(nullptr);
  imports_->audio().set_project(nullptr);
  session_.reset();
  ++project_generation_;
  project_name_.clear();
  set_save_problem({});
  track_row_heights_.clear();
  emit trackRowHeightsChanged();
  // The next project publishes everything afresh.
  published_ = {};
  emit projectChanged();
  refresh_available_projects();
  set_status(QStringLiteral("Project closed"));
}

void ProjectBridge::set_project(std::unique_ptr<application::ProjectSession> session,
                                const std::filesystem::path& opened_root) {
  if (!ready_to_change_project())
    return;
  // Every consumer of the root (warp sources, caches, recents) gets one
  // absolute, normalized path: a project opened as "build/song" or "song/"
  // otherwise plays until a clip needs warping.
  std::error_code absolute_error;
  auto root = std::filesystem::absolute(opened_root, absolute_error).lexically_normal();
  if (absolute_error)
    root = opened_root.lexically_normal();
  if (!root.has_filename())
    root = root.parent_path();
  release_project();
  session_ = std::move(session);
  set_save_problem({});
  refresh_meters();
  last_autosave_revision_ = session_->snapshot().revision;
  autosave_status_.clear();
  emit autosaveStatusChanged();
  if (autosave_interval_seconds_ > 0)
    autosave_timer_.start();
  track_row_heights_.clear();
  const auto saved_heights = QSettings{}.value(track_heights_key(session_->snapshot().id)).toMap();
  for (const auto& track : session_->snapshot().tracks) {
    const auto id = QString::fromStdString(track.id.to_string());
    const int height = saved_heights.value(id).toInt();
    if (height >= 64 && height <= 480)
      track_row_heights_.insert(id, height);
  }
  emit trackRowHeightsChanged();
  managed_media_->set_project(session_.get());
  imports_->audio().set_project(session_.get());
  key_review_->set_project(session_.get(), root);
  ++project_generation_;
  shown_lead_bars_ = static_cast<int>(session_->snapshot().lead_bars);
  playhead_sample_ = bar_one_sample();
  waveform_root_ = root;
  waveform_->set_project(root);
  tempo_map_commands_->reset_page();
  playback_ = std::make_unique<application::InstrumentPlayback>(*session_, audio_output_->factory(),
                                                                audio_output_->buffer_frames());
  audio_edit_failure_.clear();
  set_playback_status({});
  const auto name = root.filename().empty() ? root.string() : root.filename().string();
  project_name_ = QString::fromStdString(name);
  exports_->reset();
  refresh();
  remember_project(QString::fromStdString(root.string()));
  set_status(QStringLiteral("Project ready · model revision %1").arg(revision_));
  vst3_browser_->scan_for_project(session_->snapshot());
  cache_ui_->register_project(root);
  refresh_named_revisions();
  managed_media_->refresh();
}

void ProjectBridge::set_track_row_height(const QString& track_id, int height) {
  if (!session_ || height < 64 || height > 480)
    return;
  const auto& tracks = session_->snapshot().tracks;
  const bool exists = std::ranges::any_of(tracks, [&track_id](const project::Track& track) {
    return QString::fromStdString(track.id.to_string()) == track_id;
  });
  if (!exists)
    return;
  if (height == 88)
    track_row_heights_.remove(track_id);
  else
    track_row_heights_.insert(track_id, height);
  QSettings{}.setValue(track_heights_key(session_->snapshot().id), track_row_heights_);
  emit trackRowHeightsChanged();
}

void ProjectBridge::set_arrangement_viewport(double first_bar, double visible_bars,
                                             int pixel_width) {
  // Only the waveform columns in view change.
  if (waveform_->set_viewport(first_bar, visible_bars, pixel_width))
    refresh_clip_waveforms();
}

void ProjectBridge::rebuild_clip_waveform(const QString& clip_id) {
  waveform_->rebuild_clip(clip_id);
}

void ProjectBridge::select_audio_clip(const QString& clip_id) {
  if (!session_) {
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  std::optional<project::SourceRecord> source;
  if (parsed) {
    for (const auto& track : session_->snapshot().tracks) {
      const auto clip = std::ranges::find_if(
          track.clips, [&](const auto& candidate) { return candidate.id == parsed.value(); });
      if (clip != track.clips.end()) {
        const auto found =
            std::ranges::find_if(session_->snapshot().sources, [&](const auto& candidate) {
              return candidate.id == clip->source;
            });
        if (found != session_->snapshot().sources.end()) {
          source = *found;
        }
        break;
      }
    }
  }
  key_review_->select_clip(source ? clip_id : QString(), source);
  clip_warp_->select_clip(source ? clip_id : QString());
}

void ProjectBridge::set_tempo_event_page(int page) { tempo_map_commands_->set_page(page); }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectBridge::add_tempo_event(int bar, int beat, double bpm, int shape_index) {
  tempo_map_commands_->add_event(bar, beat, bpm, shape_index);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectBridge::update_tempo_event(const QString& tick_text, double bpm, int shape_index) {
  tempo_map_commands_->update_event(tick_text, bpm, shape_index);
}

void ProjectBridge::set_initial_tempo_bpm(double bpm) { tempo_map_commands_->set_initial_bpm(bpm); }

void ProjectBridge::set_initial_meter(int numerator, int denominator) {
  tempo_map_commands_->set_initial_meter(numerator, denominator);
}

void ProjectBridge::remove_tempo_event(const QString& tick_text) {
  tempo_map_commands_->remove_event(tick_text);
}

void ProjectBridge::set_status(QString value) {
  status_log_.note("status", value);
  status_ = std::move(value);
  emit statusChanged();
}

void ProjectBridge::add_instrument_track() { track_commands_->add_instrument_track(); }

void ProjectBridge::remove_track(const QString& track_id) {
  track_commands_->remove_track(track_id);
}

void ProjectBridge::duplicate_track(const QString& track_id) {
  if (const auto copy = track_commands_->duplicate_track(track_id); !copy.isEmpty())
    set_selected_track_id(copy);
}

bool ProjectBridge::move_track(const QString& track_id, int to_index) {
  return track_commands_->move_track(track_id, to_index);
}

void ProjectBridge::set_track_muted(const QString& track_id, bool muted) {
  track_commands_->set_track_muted(track_id, muted);
}

void ProjectBridge::set_track_soloed(const QString& track_id, bool soloed) {
  track_commands_->set_track_soloed(track_id, soloed);
}

void ProjectBridge::set_track_level_pan(const QString& track_id, double level, double pan) {
  track_commands_->set_track_level_pan(track_id, level, pan);
}

void ProjectBridge::set_master_level(double level) { track_commands_->set_master_level(level); }

bool ProjectBridge::route_edit_allowed() {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing routes"));
    return false;
  }
  return true;
}

void ProjectBridge::add_bus_track(const QString& kind) { track_commands_->add_bus_track(kind); }

void ProjectBridge::set_main_route(const QString& track_id, const QString& destination_id) {
  route_commands_->set_main_route(track_id, destination_id);
}

void ProjectBridge::add_send_route(const QString& track_id, const QString& destination_id,
                                   double gain) {
  route_commands_->add_send_route(track_id, destination_id, gain);
}

void ProjectBridge::set_send_gain(const QString& send_id, double gain) {
  route_commands_->set_send_gain(send_id, gain);
}

void ProjectBridge::remove_send(const QString& send_id) { route_commands_->remove_send(send_id); }

void ProjectBridge::add_midi_clip(const QString& track_id, int bar_index) {
  if (control_busy()) {
    set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                    : QStringLiteral("Wait for the audio edit to finish"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || bar_index < 0 || bar_index >= detail::visible_bars) {
    set_status(QStringLiteral("Select a track and a bar from 1 to 16"));
    return;
  }
  const auto tick = session_->snapshot().meter.tick_at({bar_index + 1, 1, 0});
  if (!tick) {
    set_status(QStringLiteral("That bar is unavailable"));
    return;
  }
  const auto result = session_->midi_clips().add_midi_clip(parsed.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI clip: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip added · revision %1").arg(revision_));
}

void ProjectBridge::add_note(const QString& clip_id, int step_index, int key) {
  if (control_busy()) {
    set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                    : QStringLiteral("Wait for the audio edit to finish"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || step_index < 0 || step_index >= 16 || key < 0 || key > 127) {
    set_status(QStringLiteral("Select a MIDI clip, step and pitch"));
    return;
  }
  const auto result =
      session_->notes().add_note(parsed.value(), {step_index * (timeline::ticks_per_quarter / 4)},
                                 static_cast<std::uint8_t>(key));
  if (!result) {
    set_status(QStringLiteral("MIDI note: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI note added · revision %1").arg(revision_));
}

bool ProjectBridge::audio_edit_enabled() const {
  return session_ && !audio_edit_completion_ && !playback_completion_ && !export_busy() &&
         !import_busy() && !managed_media_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing() && playback_ &&
         !playback_->retiring() &&
         (!playback_->playing() ||
          (playback_->output_state() == audio_io::OutputState::streaming &&
           playback_->status().application != application::PlaybackApplication::pending));
}

void ProjectBridge::move_audio_clip(const QString& clip_id, double normalized_start,
                                    bool snap_to_beat) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to move"));
    return;
  }
  const auto start =
      timeline_position(session_->snapshot(), normalized_start, snap_to_beat, timeline_bars_);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  const auto before = session_->snapshot().revision;
  const auto result = session_->audio_clips().move_audio_clip(parsed.value(), start.value());
  if (!result) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(before == revision_
                 ? QStringLiteral("Audio clip stayed at its original position")
                 : QStringLiteral("Audio clip moved · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::move_audio_clip_to_track(const QString& clip_id, const QString& track_id,
                                             double normalized_start) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!parsed || !track) {
    set_status(QStringLiteral("Select an audio clip and a track to move it to"));
    return;
  }
  const auto start =
      timeline_position(session_->snapshot(), normalized_start, false, timeline_bars_);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  if (auto moved = session_->audio_clips().move_audio_clip_to_track(parsed.value(), track.value(),
                                                                    start.value());
      !moved) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(moved.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("Audio clip moved to another track · revision %1").arg(revision_));
}

void ProjectBridge::nudge_audio_clip(const QString& clip_id, int beat_direction) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || (beat_direction != -1 && beat_direction != 1)) {
    set_status(QStringLiteral("Select an audio clip and a move direction"));
    return;
  }
  const auto& snapshot = session_->snapshot();
  const project::Clip* clip = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find_if(
        track.clips, [id = parsed.value()](const auto& item) { return item.id == id; });
    if (found != track.clips.end()) {
      clip = &*found;
      break;
    }
  }
  if (!clip) {
    set_status(QStringLiteral("Audio clip no longer exists"));
    return;
  }
  foundation::Result<timeline::TickPosition> start =
      std::holds_alternative<project::MusicalInterval>(clip->placement)
          ? foundation::Result<timeline::TickPosition>::success(
                std::get<project::MusicalInterval>(clip->placement).start)
          : snapshot.tempo.sample_to_tick(std::get<project::ElapsedInterval>(clip->placement).start,
                                          snapshot.sample_rate);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  auto location = snapshot.meter.locate(start.value());
  if (!location) {
    set_status(
        QStringLiteral("Audio move: %1").arg(QString::fromStdString(location.error().message)));
    return;
  }
  auto target = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
  if (beat_direction > 0) {
    target = snapshot.meter.tick_at(
        {location.value().bar, static_cast<std::uint8_t>(location.value().beat + 1), 0});
    if (!target) {
      target = snapshot.meter.tick_at({location.value().bar + 1, 1, 0});
    }
  } else if (target && start.value() == target.value()) {
    if (start.value().value == 0) {
      set_status(QStringLiteral("Audio clip cannot move before the project start"));
      return;
    }
    location = snapshot.meter.locate({start.value().value - 1});
    if (location) {
      target = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
    }
  }
  if (!target) {
    set_status(
        QStringLiteral("Audio move: %1").arg(QString::fromStdString(target.error().message)));
    return;
  }
  const auto before = snapshot.revision;
  const auto result = session_->audio_clips().move_audio_clip(parsed.value(), target.value());
  if (!result) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(before == revision_
                 ? QStringLiteral("Audio clip stayed at its original position")
                 : QStringLiteral("Audio clip moved · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::trim_audio_clip(const QString& clip_id, bool left_edge, double normalized_edge,
                                    bool snap_to_beat) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to trim"));
    return;
  }
  const auto tick =
      timeline_position(session_->snapshot(), normalized_edge, snap_to_beat, timeline_bars_);
  if (!tick) {
    set_status(QStringLiteral("Audio trim: %1").arg(QString::fromStdString(tick.error().message)));
    return;
  }
  const auto sample =
      session_->snapshot().tempo.tick_to_sample(tick.value(), session_->snapshot().sample_rate);
  if (!sample) {
    set_status(
        QStringLiteral("Audio trim: %1").arg(QString::fromStdString(sample.error().message)));
    return;
  }
  const auto before = session_->snapshot().revision;
  const auto result = session_->audio_clips().trim_audio_clip(
      parsed.value(),
      left_edge ? application::AudioClipEdge::left : application::AudioClipEdge::right,
      sample.value());
  if (!result) {
    set_status(QStringLiteral("Audio trim rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(
      before == revision_
          ? QStringLiteral("Audio clip edge stayed at its original position")
          : QStringLiteral("Audio clip trimmed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::set_send_automation_point(const QString& send_id, int zero_based_bar,
                                              double value) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  if (!send || zero_based_bar < 0 || zero_based_bar >= detail::visible_bars ||
      !std::isfinite(value) || value < 0.0 || value > 4.0) {
    set_status(QStringLiteral("Send point needs a bar from 1 to 16 and gain from 0 to 4"));
    return;
  }
  const auto track = std::ranges::find_if(session_->snapshot().tracks, [send](const auto& item) {
    return std::ranges::any_of(item.sends,
                               [send](const auto& route) { return route.id == send.value(); });
  });
  if (track == session_->snapshot().tracks.end()) {
    set_status(QStringLiteral("Select a send to automate"));
    return;
  }
  const auto projected =
      detail::send_automation_projection(session_->snapshot(), *track, send.value());
  const auto shown_points = projected.value(QStringLiteral("points")).toList();
  const bool replacing = std::ranges::any_of(shown_points, [zero_based_bar](const auto& item) {
    return item.toMap().value(QStringLiteral("bar")).toInt() == zero_based_bar;
  });
  if (projected.value(QStringLiteral("pointCount")).toInt() >=
          static_cast<int>(detail::max_quick_automation_points) &&
      !replacing) {
    set_status(QStringLiteral("Select a send with fewer than four Quick editor points"));
    return;
  }
  const auto result = session_->parameter_automation().set_send_gain_automation_point(
      send.value(), zero_based_bar, value);
  if (!result) {
    set_status(
        QStringLiteral("Send automation: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send point accepted · revision %1 · Save to keep; check transport "
                            "status for live application")
                 .arg(revision_));
}

void ProjectBridge::set_send_automation_value(const QString& send_id, const QString& point_id,
                                              double value) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!send || !point || !std::isfinite(value) || value < 0.0 || value > 4.0) {
    set_status(QStringLiteral("Select a send point and enter gain from 0 to 4"));
    return;
  }
  const auto result = session_->parameter_automation().set_send_gain_automation_value(
      send.value(), point.value(), value);
  if (!result) {
    set_status(
        QStringLiteral("Send automation: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send gain point accepted · revision %1 · Save to keep; check "
                            "transport status for live application")
                 .arg(revision_));
}

void ProjectBridge::remove_send_automation_point(const QString& send_id, const QString& point_id) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!send || !point) {
    set_status(QStringLiteral("Select a send automation point"));
    return;
  }
  const auto result = session_->parameter_automation().remove_send_gain_automation_point(
      send.value(), point.value());
  if (!result) {
    set_status(QStringLiteral("Remove send point: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send point removed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::remove_send_automation_lane(const QString& send_id) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  if (!send) {
    set_status(QStringLiteral("Select a send automation lane"));
    return;
  }
  const auto result =
      session_->parameter_automation().remove_send_gain_automation_lane(send.value());
  if (!result) {
    set_status(
        QStringLiteral("Remove send lane: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send lane removed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::undo() {
  if (control_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before undo")
                   : QStringLiteral("Wait for the audio edit to finish before undo"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto result = session_->history().undo();
  if (result) {
    refresh();
    submit_playback_edit();
    if (!playback_busy())
      set_playback_status(QStringLiteral("Playback stopped; model revision %1 will be prepared on "
                                         "the next Play")
                              .arg(revision_));
  }
  set_status(
      result
          ? QStringLiteral("Undo applied · revision %1").arg(revision_)
          : QStringLiteral("Undo failed: %1").arg(QString::fromStdString(result.error().message)));
}

void ProjectBridge::redo() {
  if (control_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before redo")
                   : QStringLiteral("Wait for the audio edit to finish before redo"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto result = session_->history().redo();
  if (result) {
    refresh();
    submit_playback_edit();
    if (!playback_busy())
      set_playback_status(QStringLiteral("Playback stopped; model revision %1 will be prepared on "
                                         "the next Play")
                              .arg(revision_));
  }
  set_status(
      result
          ? QStringLiteral("Redo applied · revision %1").arg(revision_)
          : QStringLiteral("Redo failed: %1").arg(QString::fromStdString(result.error().message)));
}

bool ProjectBridge::playback_active() const {
  return audio_edit_completion_ || routed_completion_ || routed_playback_ ||
         routed_stop_completion_ || (playback_ && playback_->playing());
}

QVariantMap ProjectBridge::master_meter() const {
  const auto reading = meter_->latest();
  const bool valid = reading && reading->transport_generation != 0;
  return {{"valid", valid},
          {"leftPeak", valid ? reading->left_peak : 0.0F},
          {"rightPeak", valid ? reading->right_peak : 0.0F},
          {"leftRms", valid ? reading->left_rms : 0.0F},
          {"rightRms", valid ? reading->right_rms : 0.0F},
          {"clipped", valid && reading->clipped},
          {"invalidFrames", valid ? reading->invalid_frames : 0U},
          {"endSample", valid ? static_cast<qulonglong>(reading->end_sample) : 0ULL},
          {"generation", valid ? static_cast<qulonglong>(reading->transport_generation) : 0ULL}};
}

void ProjectBridge::drain_realtime_diagnostics() {
  // What the audio callback could not say in text: silenced blocks, xruns.
  // An empty ring costs one atomic load.
  if (const auto drained = foundation::log::drain_diagnostics(); !drained.status.empty())
    set_status(QString::fromStdString(drained.status));
}

void ProjectBridge::poll_meters() {
  drain_realtime_diagnostics();
  // Edits poll playback every 5 ms; meters need no more than ~20 Hz. The
  // final poll that stops the timer always refreshes, clearing stale levels.
  if (playback_timer_.isActive() && meter_clock_.isValid() && meter_clock_.elapsed() < 45)
    return;
  refresh_meters();
}

QObject* ProjectBridge::meters() const noexcept { return meter_model_.get(); }

bool ProjectBridge::track_meters_live() const noexcept {
  // The routed graph measures each track's node after level, pan, mute and
  // inserts. A paused, changing, stopping or direct-instrument transport
  // has no live node meters, so every track reads invalid.
  if (!routed_playback_ && monitor_)
    return monitor_->output_status().state == audio_io::OutputState::streaming;
  return routed_playback_ && !routed_completion_ && !routed_stop_completion_ &&
         !routed_transport_completion_ && !routed_playback_->paused() &&
         routed_playback_->output_status().state == audio_io::OutputState::streaming;
}

void ProjectBridge::refresh_meters() {
  OSCILLATE_TRACE_SPAN("ui.refresh_meters");
  meter_clock_.start();
  // The display clock runs while there is anything to show.
  if (meter_model_)
    meter_model_->wake();
  QVariantMap meters;
  if (session_) {
    const bool live = track_meters_live();
    for (const auto& track : session_->snapshot().tracks) {
      const auto reading = live ? live_run()->track_meter(track.id) : std::nullopt;
      const bool valid = reading && reading->transport_generation != 0;
      meters.insert(QString::fromStdString(track.id.to_string()),
                    QVariantMap{{"valid", valid},
                                {"leftPeak", valid ? reading->left_peak : 0.0F},
                                {"rightPeak", valid ? reading->right_peak : 0.0F},
                                {"leftRms", valid ? reading->left_rms : 0.0F},
                                {"rightRms", valid ? reading->right_rms : 0.0F},
                                {"clipped", valid && reading->clipped}});
    }
  }
  track_meters_ = std::move(meters);
  emit metersChanged();
}

QVariant ProjectBridge::cpu_load() const {
  // The whole device callback against its period, from the output's
  // DeviceTelemetry (PipeWire or JACK): the same figure as the trace's audio.load.
  std::optional<audio_io::CallbackLoad> load;
  if (routed_playback_)
    load = routed_playback_->output_status().callback_load;
  else if (playback_)
    load = playback_->output_status().callback_load;
  else if (monitor_)
    load = monitor_->output_status().callback_load;
  return load ? QVariant{std::clamp(load->average, 0.0, 1.0)} : QVariant{};
}

int ProjectBridge::audio_threads() const {
  if (!routed_playback_)
    return 0;
  const auto output = routed_playback_->output_status();
  return output.state == audio_io::OutputState::streaming
             ? 1 + static_cast<int>(output.realtime_workers)
             : 0;
}

QVariantMap ProjectBridge::master_true_peak() const {
  const auto reading = true_peak_->latest();
  const bool incomplete = true_peak_->incomplete();
  const bool complete = reading && reading->complete && !incomplete && reading->peak.frames != 0 &&
                        reading->transport_generation != 0;
  return {
      {"valid", complete},
      {"incomplete", incomplete},
      {"left", complete ? reading->peak.left_true_peak : 0.0F},
      {"right", complete ? reading->peak.right_true_peak : 0.0F},
      {"invalidFrames", complete ? static_cast<qulonglong>(reading->peak.invalid_frames) : 0ULL},
      {"droppedBlocks", static_cast<qulonglong>(true_peak_->dropped_blocks())},
      {"endSample", complete ? static_cast<qulonglong>(reading->end_sample) : 0ULL}};
}

QVariantList ProjectBridge::master_spectrum() const {
  const auto reading = spectrum_->latest();
  if (!reading)
    return {};
  constexpr int bands = 48;
  constexpr double low_hz = 20.0;
  const auto high_hz = std::min(20'000.0, static_cast<double>(reading->spectrum.sample_rate) / 2.0);
  const auto bin_scale = static_cast<double>(native_dsp::StereoSpectrum::window_frames) /
                         static_cast<double>(reading->spectrum.sample_rate);
  QVariantList values;
  values.reserve(bands);
  for (int band = 0; band < bands; ++band) {
    const auto low = low_hz * std::pow(high_hz / low_hz, static_cast<double>(band) / bands);
    const auto high = low_hz * std::pow(high_hz / low_hz, static_cast<double>(band + 1) / bands);
    const auto first = std::clamp(static_cast<std::size_t>(std::floor(low * bin_scale)),
                                  std::size_t{1}, native_dsp::StereoSpectrum::bin_count - 1);
    const auto last = std::clamp(static_cast<std::size_t>(std::ceil(high * bin_scale)), first,
                                 native_dsp::StereoSpectrum::bin_count - 1);
    double amplitude = 0.0;
    for (auto bin = first; bin <= last; ++bin) {
      amplitude = std::max(amplitude, static_cast<double>(std::max(reading->spectrum.left[bin],
                                                                   reading->spectrum.right[bin])));
    }
    const auto db = 20.0 * std::log10(std::max(amplitude, 1e-6));
    values.push_back(std::clamp((db + 72.0) / 72.0, 0.0, 1.0));
  }
  return values;
}

bool ProjectBridge::playback_enabled() const {
  if (routed_transport_completion_) {
    return false;
  }
  if (routed_completion_) {
    return !routed_completion_->stop_requested.load(std::memory_order_acquire) &&
           !close_after_playback_;
  }
  if (routed_stop_completion_) {
    return false;
  }
  if (routed_playback_) {
    return !close_after_playback_;
  }
  if (audio_edit_completion_) {
    return !audio_edit_completion_->stop_requested.load(std::memory_order_acquire) &&
           !close_after_playback_;
  }
  return session_ && !playback_completion_ && (!playback_ || !playback_->retiring()) &&
         !export_busy() && !import_busy() && !managed_media_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing();
}

bool ProjectBridge::managed_media_busy() const { return managed_media_->busy(); }

bool ProjectBridge::managed_media_repair_enabled() const {
  return session_ && !managed_media_busy() && !playback_busy() && !export_busy() &&
         !import_busy() && !vst3_browser_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing() && !managed_media_->closing();
}

void ProjectBridge::collect_project_copy() {
  if (!session_ || waveform_root_.empty() || collection_->busy() || playback_busy() ||
      export_busy() || import_busy() || managed_media_busy() || vst3_browser_busy()) {
    set_status(QStringLiteral("Finish active project work before collecting a copy"));
    return;
  }
  if (const auto failure = collection_->collect(*session_, waveform_root_, projects_root_))
    set_status(*failure);
}

void ProjectBridge::cancel_project_collection() { collection_->cancel(); }

void ProjectBridge::refresh_available_projects() { lifecycle_->refresh_available_projects(); }
void ProjectBridge::remember_project(const QString& path) { lifecycle_->remember_project(path); }
bool ProjectBridge::save_current_project_if_dirty() {
  return lifecycle_->save_current_project_if_dirty();
}
void ProjectBridge::create_default_project(const QString& name) {
  lifecycle_->create_default_project(name);
}
void ProjectBridge::open_project(const QUrl& folder) { lifecycle_->open_project(folder); }
void ProjectBridge::choose_open_project() { lifecycle_->choose_open_project(); }
void ProjectBridge::open_listed(const QString& path) { lifecycle_->open_listed(path); }
void ProjectBridge::forget_project(const QString& path) { lifecycle_->forget_project(path); }
void ProjectBridge::trash_project(const QString& path) { lifecycle_->trash_project(path); }
void ProjectBridge::rename_project(const QString& path, const QString& name) {
  lifecycle_->rename_project(path, name);
}
void ProjectBridge::choose_recovery(const QString& generation, const QString& kind) {
  lifecycle_->choose_recovery(generation, kind);
}
void ProjectBridge::cancel_recovery() { lifecycle_->cancel_recovery(); }
void ProjectBridge::save() { lifecycle_->save(); }
bool ProjectBridge::named_revision_enabled() const { return lifecycle_->named_revision_enabled(); }
void ProjectBridge::refresh_named_revisions() { lifecycle_->refresh_named_revisions(); }
void ProjectBridge::create_named_revision(const QString& name) {
  lifecycle_->create_named_revision(name);
}
void ProjectBridge::restore_named_revision(const QString& name) {
  lifecycle_->restore_named_revision(name);
}

} // namespace oscillate::quick
