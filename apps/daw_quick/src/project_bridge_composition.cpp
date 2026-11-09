#include "project_bridge.hpp"

#include "arrange_controller.hpp"
#include "arrangement_automation_controller.hpp"
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
#include "jack_playback_output.hpp"
#include "key_match_controller.hpp"
#include "loop_transport.hpp"
#include "master_section.hpp"
#include "meter_model.hpp"
#include "mix_check_controller.hpp"
#include "note_effects_controller.hpp"
#include "note_tools_controller.hpp"
#include "plugin_automation_controller.hpp"
#include "project_bridge_projection.hpp"
#include "recording_controller.hpp"
#include "route_commands.hpp"
#include "tempo_lane_controller.hpp"
#include "track_commands.hpp"
#include "waveform_controller.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Composition: the audio output, meters and the feature controllers the
// bridge owns, created and wired to it once at construction.
namespace oscillate::quick {
namespace {

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

} // namespace

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
  plugin_automation_ =
      std::make_unique<PluginAutomationController>(session, quiet, report, commit, this);
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
  tempo_lane_ = std::make_unique<TempoLaneController>(
      session,
      [this] {
        return guarded_edit(
            QStringLiteral("Finish the current project operation before editing tempo"));
      },
      [this](QString message) { set_status(std::move(message)); }, commit, this);
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
  // A saved copy opens, unless another project was opened meanwhile.
  connect(
      collection_.get(), &ProjectCollectionController::copySaved, this,
      [this](const QString& destination, const QString& source) {
        if (!session_ || QString::fromStdString(waveform_root_.string()) != source)
          return;
        lifecycle_->open_project(QUrl::fromLocalFile(destination));
        const auto copy = std::filesystem::path(destination.toStdString());
        if (project_name_ == QString::fromStdString(copy.filename().string()))
          set_status(collection_->status());
        else
          set_status(
              QStringLiteral("Saved a copy at %1; it did not open: %2").arg(destination, status_));
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
  create_diagnostics();
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
          .routed = [this] { return routed_run(); },
          .routed_settled =
              [this] { return !routed_stop_completion_ && !routed_transport_completion_; },
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
          .catalogue = [this]() -> const plugins::Vst3CatalogueSnapshot& {
            return vst3_browser_->catalogue();
          },
          .hash_binary = vst3_binary_hasher_,
#endif
          .commit = [this] { commit_edit(); },
          .schedule_save = [this] { schedule_background_save(); },
          .status = [this](QString message) { set_status(std::move(message)); }},
      *plugin_automation_, this);
}

void ProjectBridge::create_diagnostics() {
  diagnostics_ = std::make_unique<DiagnosticsController>(
      DiagnosticsController::Host{
          .plugins =
              [this] {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
                return diagnostics_plugins(vst3_browser_->catalogue());
#else
                return DiagnosticsPlugins{};
#endif
              },
          .status = [this](QString message) { set_status(std::move(message)); },
          .reveal = {},
          .write = {}},
      this);
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

} // namespace oscillate::quick
