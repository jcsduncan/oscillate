#include "project_bridge.hpp"

#include "arrange_controller.hpp"
#include "arrangement_automation_controller.hpp"
#include "browser_preview_controller.hpp"
#include "clay_controller.hpp"
#include "clip_automation_controller.hpp"
#include "clip_tempo_controller.hpp"
#include "clip_warp_controller.hpp"
#include "command_palette.hpp"
#include "computer_keyboard.hpp"
#include "diagnostics_controller.hpp"
#include "freeze_controller.hpp"
#include "groove_capture_controller.hpp"
#include "harmony_controller.hpp"
#include "history_controller.hpp"
#include "key_match_controller.hpp"
#include "loop_transport.hpp"
#include "macro_controller.hpp"
#include "master_section.hpp"
#include "meter_model.hpp"
#include "mix_check_controller.hpp"
#include "native_synth_audition.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "note_effects_controller.hpp"
#include "note_tools_controller.hpp"
#include "plugin_automation_controller.hpp"
#include "rack_library_controller.hpp"
#include "recording_controller.hpp"
#include "route_commands.hpp"
#include "tempo_lane_controller.hpp"
#include "track_automation_controller.hpp"
#include "track_commands.hpp"
#include "ui_contracts.hpp"
#include "vst3_qualification_policy.hpp"
#include "waveform_controller.hpp"
#include "workspace_layout_controller.hpp"

#include <QDir>
#include <QSettings>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

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
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  vst3_binary_hasher_ = std::move(dependencies.vst3_binary_hasher);
#endif
  plugin_editor_commit_ = dependencies.plugin_editor_commit
                              ? std::move(dependencies.plugin_editor_commit)
                              : [this] { return vst3_editor_->commit_native_editor(); };
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
QObject* ProjectBridge::tempo_lane() const noexcept { return tempo_lane_.get(); }
QObject* ProjectBridge::loop_transport() const noexcept { return loop_transport_.get(); }
QObject* ProjectBridge::track_automation() const noexcept { return track_automation_.get(); }
QObject* ProjectBridge::plugin_automation() const noexcept { return plugin_automation_.get(); }
QObject* ProjectBridge::route_commands() const noexcept { return route_commands_.get(); }
QObject* ProjectBridge::track_commands() const noexcept { return track_commands_.get(); }
QObject* ProjectBridge::history() const noexcept { return history_.get(); }
QObject* ProjectBridge::command_palette() const noexcept { return command_palette_.get(); }
QObject* ProjectBridge::diagnostics() const noexcept { return diagnostics_.get(); }
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

} // namespace oscillate::quick
