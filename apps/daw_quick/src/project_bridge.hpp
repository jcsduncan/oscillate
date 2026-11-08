#pragma once

#include "app_logging.hpp"
#include "audio_output_controller.hpp"
#include "background_job.hpp"
#include "cache_ui_controller.hpp"
#include "export_controller.hpp"
#include "import_controller.hpp"
#include "key_review_controller.hpp"
#include "managed_media_controller.hpp"
#include "metered_output.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/pipewire_sinks.hpp"
#include "performance_trace.hpp"
#include "project_collection_controller.hpp"
#include "project_lifecycle_controller.hpp"
#include "project_summary_view.hpp"
#include "routed_playback.hpp"
#include "transport_controller.hpp"
#include "vst3_browser_controller.hpp"
#include "vst3_editor_controller.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::synth {
struct SynthEngine;
}

namespace oscillate::quick {

class ArrangementAutomationController;
class ClipTempoController;
class BrowserPreviewController;
class KeyMatchController;
class ClipAutomationController;
class ClipWarpController;
class LoopTransport;
struct WarpPlaybackObservation;
class PluginAutomationController;
class TrackAutomationController;
class RouteCommands;
class ComputerKeyboard;
class TrackCommands;
class NoteToolsController;
class NoteEffectsController;
class FreezeController;
class MixCheckController;
class ArrangeController;
class HarmonyController;
class RecordingController;
class GrooveCaptureController;
class ClayController;
class MacroController;
class RackLibraryController;
class TempoMapCommands;
class WorkspaceLayoutController;
class NativeSynthDeviceController;
class NativeSynthAudition;
class SessionNativeSynthAdapter;
class NativeSynthAutomationController;
class RelayPanelModel;
class AnvilPanelModel;
class KnockPanelModel;
class WaveformController;
class MeterModel;
class KeyedListModel;
class HistoryController;
class CommandPalette;
class DiagnosticsController;
class MasterSection;

// Runs a job on a worker and returns its thread (empty when the job ran
// inline, before the call returned).
using BridgeWorkerRunner = std::function<std::jthread(std::function<void()>)>;
using BridgeLiveEditHandoffHook =
    std::function<void(application::ProjectSession&, application::PreparedInstrumentPlayback&)>;
// ProjectBridge collaborators a host may replace; defaults are the production ones.
struct BridgeDependencies {
  // Binaries known to report a fixed latency, in place of the shipped
  // qualification list (verified_vst3_live_qualification).
  std::optional<std::vector<std::string>> fixed_latency_vst3_binaries{};
  // Runs the live audio-edit handoff; default: a new worker thread.
  BridgeWorkerRunner audio_edit_runner{};
  // Sees each prepared live audio edit on the worker, before the handoff
  // replaces the playing graph.
  BridgeLiveEditHandoffHook before_live_edit_handoff{};
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Hashes a plugin binary for the VST3 editor, on its worker; default:
  // plugins::hash_vst3_binary.
  Vst3BinaryHasher vst3_binary_hasher{};
#endif
};

// A UI-thread, revisioned projection. QML receives values, never model pointers.
class ProjectBridge final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool projectOpen READ project_open NOTIFY projectChanged)
  Q_PROPERTY(QString projectName READ project_name NOTIFY projectChanged)
  Q_PROPERTY(int autosaveIntervalSeconds READ autosave_interval_seconds WRITE
                 set_autosave_interval_seconds NOTIFY autosaveIntervalChanged)
  Q_PROPERTY(QString autosaveStatus READ autosave_status NOTIFY autosaveStatusChanged)
  // Why the last save of the open project failed, until one succeeds; empty
  // when the project on disk is current.
  Q_PROPERTY(QString saveProblem READ save_problem NOTIFY saveProblemChanged)
  Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)
  // Each projection signals only when its value changed. `tracks` is the
  // whole list for lookups; delegates repeat `trackModel` (and each row's
  // `clips` model), which signal only the rows that changed.
  Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
  Q_PROPERTY(QObject* trackModel READ track_model CONSTANT)
  // Arrangement markers: {id, name, ticks, bar} in position order.
  Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
  // The song's key as the top bar reads it ("A min"), empty until chosen;
  // the suggestion comes from the key detected in the project's audio.
  Q_PROPERTY(QString projectKey READ project_key NOTIFY projectKeyChanged)
  Q_PROPERTY(QString suggestedProjectKey READ suggested_project_key NOTIFY projectKeyChanged)
  // The project key's pitch classes (0 = C), tonic first; empty without a
  // key. The piano roll highlights them and can fold to them.
  Q_PROPERTY(QVariantList projectScale READ project_scale NOTIFY projectKeyChanged)
  // The edit selection the clipboard and shortcuts act on. The arrangement
  // sets clip ids and the selected track; the piano roll sets its clip and
  // notes (an empty note list means the clips are the selection).
  Q_PROPERTY(QStringList selectedClipIds READ selected_clip_ids WRITE set_selected_clip_ids NOTIFY
                 selectionChanged)
  Q_PROPERTY(QString selectedTrackId READ selected_track_id WRITE set_selected_track_id NOTIFY
                 selectionChanged)
  // The computer keyboard as a musical keyboard, playing the selected track.
  Q_PROPERTY(QObject* computerKeyboard READ computer_keyboard_object CONSTANT)
  Q_PROPERTY(QString selectedNoteClipId READ selected_note_clip_id WRITE set_selected_note_clip_id
                 NOTIFY selectionChanged)
  Q_PROPERTY(QStringList selectedNoteIds READ selected_note_ids WRITE set_selected_note_ids NOTIFY
                 selectionChanged)
  Q_PROPERTY(QObject* clipAutomation READ clip_automation CONSTANT)
  Q_PROPERTY(QObject* arrangementAutomation READ arrangement_automation CONSTANT)
  Q_PROPERTY(QObject* clipTempo READ clip_tempo CONSTANT)
  // Browser previews and smart drops (ADR-0059; project_bridge_browser.cpp).
  Q_PROPERTY(QObject* browserPreview READ browser_preview CONSTANT)
  Q_PROPERTY(QObject* keyMatch READ key_match CONSTANT)
  Q_PROPERTY(QObject* clipWarp READ clip_warp CONSTANT)
  Q_PROPERTY(QObject* loopTransport READ loop_transport CONSTANT)
  Q_PROPERTY(QObject* performanceTrace READ performance_trace CONSTANT)
  Q_PROPERTY(QObject* trackAutomation READ track_automation CONSTANT)
  Q_PROPERTY(QObject* pluginAutomation READ plugin_automation CONSTANT)
  Q_PROPERTY(QObject* routeCommands READ route_commands CONSTANT)
  Q_PROPERTY(QObject* trackCommands READ track_commands CONSTANT)
  // The undo history panel, the Ctrl+K palette's ranking and the master
  // section's loudness, true peak and mono check.
  Q_PROPERTY(QObject* history READ history CONSTANT)
  Q_PROPERTY(QObject* commandPalette READ command_palette CONSTANT)
  // "Save diagnostics" (foundation/diagnostics_bundle).
  Q_PROPERTY(QObject* diagnostics READ diagnostics CONSTANT)
  Q_PROPERTY(QObject* masterSection READ master_section CONSTANT)
  Q_PROPERTY(QObject* noteTools READ note_tools CONSTANT)
  // MIDI effects before a track's built-in synth (ADR-0059).
  Q_PROPERTY(QObject* noteEffects READ note_effects CONSTANT)
  // Freeze, bounce, flatten and consolidate (ADR-0059).
  Q_PROPERTY(QObject* freezeCommands READ freeze_commands CONSTANT)
  // Mix Check (ADR-0074).
  Q_PROPERTY(QObject* mixCheck READ mix_check CONSTANT)
  // Arrange from a loop (ADR-0080).
  Q_PROPERTY(QObject* arrange READ arrange CONSTANT)
  // Harmony Lab and Chord Comp; chords are notes in MIDI clips (ADR-0084).
  Q_PROPERTY(QObject* harmony READ harmony CONSTANT)
  // MIDI recording, the click and count-in (ADR-0086).
  Q_PROPERTY(QObject* recording READ recording CONSTANT)
  // Groove from a record (ADR-0079).
  Q_PROPERTY(QObject* grooveCapture READ groove_capture CONSTANT)
  // Clay, the audio clip editor's Notes tab (ADR-0081).
  Q_PROPERTY(QObject* clay READ clay CONSTANT)
  // The device chain's macro strip and the user's racks (ADR-0059).
  Q_PROPERTY(QObject* macros READ macros CONSTANT)
  Q_PROPERTY(QObject* racks READ racks CONSTANT)
  Q_PROPERTY(QObject* workspaceLayout READ workspace_layout CONSTANT)
  Q_PROPERTY(QVariantMap trackRowHeights READ track_row_heights NOTIFY trackRowHeightsChanged)
  Q_PROPERTY(int timelineBars READ timeline_bars NOTIFY timelineChanged)
  // Bars before bar 1 (ADR-0053): the ruler numbers zero-based bar i as
  // i + 1 - leadBars, so they count up to 0 before bar 1.
  Q_PROPERTY(int leadBars READ lead_bars NOTIFY timelineChanged)
  Q_PROPERTY(QVariantList insertChains READ insert_chains NOTIFY insertChainsChanged)
  // Depends on playback, import, export and the VST3 browser; each of their
  // change signals re-notifies it.
  Q_PROPERTY(bool insertChainEditEnabled READ insert_chain_edit_enabled NOTIFY
                 insertChainEditEnabledChanged)
  // The project's first tempo and meter: {projectBpm, projectMeter}.
  Q_PROPERTY(QVariantMap projectTiming READ project_timing NOTIFY projectTimingChanged)
  Q_PROPERTY(QVariantMap tempoMap READ tempo_map NOTIFY tempoMapChanged)
  Q_PROPERTY(QVariantList routeTargets READ route_targets NOTIFY routingChanged)
  Q_PROPERTY(int routeTargetCount READ route_target_count NOTIFY routingChanged)
  Q_PROPERTY(double masterLevel READ master_level NOTIFY masterLevelChanged)
  Q_PROPERTY(QVariantList availableProjects READ available_projects NOTIFY availableProjectsChanged)
  Q_PROPERTY(QVariantMap recoveryChoice READ recovery_choice NOTIFY recoveryChoiceChanged)
  Q_PROPERTY(QVariantList namedRevisions READ named_revisions NOTIFY namedRevisionsChanged)
  Q_PROPERTY(
      QVariantList namedRevisionIssues READ named_revision_issues NOTIFY namedRevisionsChanged)
  Q_PROPERTY(bool namedRevisionEnabled READ named_revision_enabled NOTIFY playbackChanged)
  // Saved-source diagnostics and media repair (ManagedMediaController).
  Q_PROPERTY(ManagedMediaController* managedMedia READ managed_media CONSTANT)
  Q_PROPERTY(QString collectionStatus READ collection_status NOTIFY collectionChanged)
  Q_PROPERTY(bool collectionBusy READ collection_busy NOTIFY collectionChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(QString playbackStatus READ playback_status NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackStatusIsDefault READ playback_status_is_default NOTIFY playbackChanged)
  Q_PROPERTY(QVariantMap playbackState READ playback_state NOTIFY playbackChanged)
  // Live levels: meters.channel(trackId) (an empty ID for the master) is
  // that channel's MeterChannel, post fader, with display ballistics on its
  // own ~60 Hz clock; each channel signals alone. Only the routed path
  // measures tracks, so the direct instrument path reads invalid.
  // cpuLoad is the 0..1 smoothed render load, undefined in QML while no
  // output is streaming; it refreshes on metersChanged at the playback poll
  // cadence.
  Q_PROPERTY(QObject* meters READ meters CONSTANT)
  Q_PROPERTY(QVariant cpuLoad READ cpu_load NOTIFY metersChanged)
  // Real-time threads rendering live playback: the device callback plus its
  // audio workers (ADR-0048). Zero while nothing streams.
  Q_PROPERTY(int audioThreads READ audio_threads NOTIFY metersChanged)
  Q_PROPERTY(QVariantMap masterTruePeak READ master_true_peak NOTIFY playbackChanged)
  Q_PROPERTY(QVariantList masterSpectrum READ master_spectrum NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackActive READ playback_active NOTIFY playbackChanged)
  Q_PROPERTY(bool routedGraphCurrent READ routed_graph_current NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackRevisionInactive READ playback_revision_inactive NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackPaused READ playback_paused NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackPauseSupported READ playback_pause_supported NOTIFY playbackChanged)
  Q_PROPERTY(qulonglong playheadSample READ playhead_sample NOTIFY playbackChanged)
  Q_PROPERTY(double playheadBar READ playhead_bar NOTIFY playbackChanged)
  Q_PROPERTY(double playheadSeconds READ playhead_seconds NOTIFY playbackChanged)
  Q_PROPERTY(bool playbackEnabled READ playback_enabled NOTIFY playbackChanged)
  // Audio and MIDI import (ImportController).
  Q_PROPERTY(ImportController* imports READ imports CONSTANT)
  Q_PROPERTY(bool audioEditEnabled READ audio_edit_enabled NOTIFY playbackChanged)
  // The output backend, device and buffer request (AudioOutputController).
  Q_PROPERTY(AudioOutputController* audioOutput READ audio_output CONSTANT)
  // The export dialog and its jobs (ExportController).
  Q_PROPERTY(ExportController* exports READ exports CONSTANT)
  // The VST3 browser and parameter editor (Vst3BrowserController,
  // Vst3EditorController).
  Q_PROPERTY(Vst3BrowserController* vst3Browser READ vst3_browser CONSTANT)
  Q_PROPERTY(Vst3EditorController* vst3Editor READ vst3_editor CONSTANT)
  Q_PROPERTY(CacheUiController* cacheControls READ cache_controls CONSTANT)
  Q_PROPERTY(KeyReviewController* keyReview READ key_review CONSTANT)
  Q_PROPERTY(QObject* nativeSynthController READ native_synth_controller NOTIFY
                 nativeSynthControllerChanged)
  Q_PROPERTY(QObject* nativeSynthAutomation READ native_synth_automation NOTIFY
                 nativeSynthControllerChanged)
  // The open synth panel's own model, for engines whose panel needs more
  // than the controller (Anvil's waveform preview, ADR-0060; Knock's pad
  // labels and ringing modes, ADR-0068); null otherwise.
  Q_PROPERTY(QObject* nativeSynthPanelModel READ native_synth_panel_model NOTIFY
                 nativeSynthControllerChanged)
  // The track the built-in synth editor is open for; empty while closed.
  Q_PROPERTY(
      QString nativeSynthTrackId READ native_synth_track_id NOTIFY nativeSynthControllerChanged)
  Q_PROPERTY(QVariantList nativeSynthEngines READ native_synth_engines CONSTANT)
  // Built-in effects (ADR-0043): the catalogue, and the open slot's panel.
  // The browser (project_bridge_browser.cpp): one card per preset of every
  // built-in instrument and effect, plus the bundled starter sounds.
  Q_PROPERTY(QVariantList browserCatalogue READ browser_catalogue CONSTANT)
  Q_PROPERTY(QStringList browserFavourites READ browser_favourites NOTIFY browserCollectionsChanged)
  Q_PROPERTY(QStringList browserRecents READ browser_recents NOTIFY browserCollectionsChanged)
  Q_PROPERTY(QObject* nativeEffectController READ native_effect_controller NOTIFY
                 nativeEffectControllerChanged)
  Q_PROPERTY(QObject* nativeEffectAutomation READ native_effect_automation NOTIFY
                 nativeEffectControllerChanged)
  // The open effect panel's own model (Relay's echo train); null otherwise.
  Q_PROPERTY(QObject* nativeEffectPanelModel READ native_effect_panel_model NOTIFY
                 nativeEffectControllerChanged)
  // Names a panel uses for its inputs; it follows sidechain edits.
  Q_PROPERTY(
      QVariantMap nativeEffectContext READ native_effect_context NOTIFY nativeEffectContextChanged)

public:
  using AudioDeviceDiscovery =
      std::function<foundation::Result<std::vector<audio_io::PipeWireSink>>()>;
  using WorkerRunner = BridgeWorkerRunner;
  using LiveEditHandoffHook = BridgeLiveEditHandoffHook;
  using Dependencies = BridgeDependencies;
  explicit ProjectBridge(const std::filesystem::path& home_directory = {},
                         QObject* parent = nullptr,
                         application::PlaybackOutputFactory output_factory = {},
                         AudioDeviceDiscovery audio_device_discovery = {},
                         application::PlaybackOutputFactory jack_output_factory = {},
                         std::function<bool()> jack_server_probe = {},
                         Dependencies dependencies = {});
  ~ProjectBridge() override;

  [[nodiscard]] bool project_open() const { return session_ != nullptr; }
  [[nodiscard]] QString project_name() const { return project_name_; }
  [[nodiscard]] int autosave_interval_seconds() const noexcept {
    return autosave_interval_seconds_;
  }
  [[nodiscard]] QString autosave_status() const { return autosave_status_; }
  [[nodiscard]] QString save_problem() const { return save_problem_; }
  void set_autosave_interval_seconds(int seconds);
  [[nodiscard]] CacheUiController* cache_controls() const { return cache_ui_.get(); }
  [[nodiscard]] KeyReviewController* key_review() const { return key_review_.get(); }
  [[nodiscard]] qulonglong revision() const { return revision_; }
  [[nodiscard]] QString project_format() const { return summary_.format(); }
  [[nodiscard]] QVariantList tracks() const { return tracks_; }
  [[nodiscard]] QObject* clip_automation() const noexcept;
  [[nodiscard]] QObject* arrangement_automation() const noexcept;
  [[nodiscard]] QObject* clip_tempo() const noexcept;
  [[nodiscard]] QObject* browser_preview() const noexcept;
  [[nodiscard]] QObject* key_match() const noexcept;
  [[nodiscard]] QObject* clip_warp() const noexcept;
  [[nodiscard]] QObject* loop_transport() const noexcept;
  [[nodiscard]] PerformanceTrace* performance_trace() const noexcept {
    return performance_trace_.get();
  }
  [[nodiscard]] QObject* track_automation() const noexcept;
  [[nodiscard]] QObject* plugin_automation() const noexcept;
  [[nodiscard]] QObject* route_commands() const noexcept;
  [[nodiscard]] QObject* track_commands() const noexcept;
  [[nodiscard]] QObject* history() const noexcept;
  [[nodiscard]] QObject* command_palette() const noexcept;
  [[nodiscard]] QObject* diagnostics() const noexcept;
  [[nodiscard]] QObject* master_section() const noexcept;
  [[nodiscard]] QObject* note_tools() const noexcept;
  [[nodiscard]] QObject* note_effects() const noexcept;
  [[nodiscard]] QObject* freeze_commands() const noexcept;
  [[nodiscard]] QObject* mix_check() const noexcept;
  [[nodiscard]] QObject* arrange() const noexcept;
  [[nodiscard]] QObject* harmony() const noexcept;
  [[nodiscard]] QObject* recording() const noexcept;
  [[nodiscard]] QObject* groove_capture() const noexcept;
  [[nodiscard]] QObject* clay() const noexcept;
  [[nodiscard]] QObject* macros() const noexcept;
  [[nodiscard]] QObject* racks() const noexcept;
  [[nodiscard]] QObject* workspace_layout() const noexcept;
  [[nodiscard]] QVariantMap track_row_heights() const { return track_row_heights_; }
  [[nodiscard]] int timeline_bars() const { return timeline_bars_; }
  [[nodiscard]] int lead_bars() const {
    return session_ ? static_cast<int>(session_->snapshot().lead_bars) : 0;
  }
  [[nodiscard]] QVariantList insert_chains() const { return insert_chains_; }
  [[nodiscard]] bool insert_chain_edit_enabled() const;
  [[nodiscard]] QVariantMap project_timing() const { return project_timing_; }
  [[nodiscard]] QVariantMap tempo_map() const { return tempo_map_; }
  [[nodiscard]] QVariantList route_targets() const { return route_targets_; }
  [[nodiscard]] int route_target_count() const { return route_target_count_; }
  [[nodiscard]] bool routing_active() const { return routing_active_; }
  [[nodiscard]] double master_level() const noexcept { return summary_.master_level(); }
  [[nodiscard]] QVariantList available_projects() const { return lifecycle_->available_projects(); }
  [[nodiscard]] QVariantMap recovery_choice() const { return lifecycle_->recovery_choice(); }
  [[nodiscard]] QVariantList named_revisions() const { return lifecycle_->named_revisions(); }
  [[nodiscard]] QVariantList named_revision_issues() const {
    return lifecycle_->named_revision_issues();
  }
  [[nodiscard]] bool named_revision_enabled() const;
  [[nodiscard]] ManagedMediaController* managed_media() const noexcept {
    return managed_media_.get();
  }
  [[nodiscard]] QString collection_status() const { return collection_->status(); }
  [[nodiscard]] bool collection_busy() const { return collection_->busy(); }
  [[nodiscard]] QString status() const { return status_; }
  [[nodiscard]] QString playback_status() const {
    return playback_status_.isEmpty() ? QStringLiteral("Playback stopped") : playback_status_;
  }
  [[nodiscard]] bool playback_status_is_default() const { return playback_status_.isEmpty(); }
  [[nodiscard]] QVariantMap playback_state() const;
  [[nodiscard]] QObject* meters() const noexcept;
  // The newest engine windows, unsmoothed, as the meters read them: the
  // master's, and each track's by ID ({valid, leftPeak, rightPeak, leftRms,
  // rightRms, clipped}).
  [[nodiscard]] QVariantMap master_meter() const;
  [[nodiscard]] QVariantMap track_meters() const { return track_meters_; }
  [[nodiscard]] QVariant cpu_load() const;
  [[nodiscard]] int audio_threads() const;
  [[nodiscard]] QVariantMap master_true_peak() const;
  [[nodiscard]] QVariantList master_spectrum() const;
  [[nodiscard]] bool playback_active() const;
  [[nodiscard]] bool routed_graph_current() const;
  [[nodiscard]] bool playback_revision_inactive() const;
  [[nodiscard]] bool playback_paused() const;
  [[nodiscard]] bool playback_pause_supported() const;
  [[nodiscard]] qulonglong playhead_sample() const;
  [[nodiscard]] double playhead_bar() const;
  [[nodiscard]] double playhead_seconds() const;
  [[nodiscard]] bool playback_enabled() const;
  [[nodiscard]] ImportController* imports() const noexcept { return imports_.get(); }
  [[nodiscard]] bool audio_edit_enabled() const;
  [[nodiscard]] AudioOutputController* audio_output() const noexcept { return audio_output_.get(); }
  [[nodiscard]] ExportController* exports() const noexcept { return exports_.get(); }
  [[nodiscard]] Vst3BrowserController* vst3_browser() const noexcept { return vst3_browser_.get(); }
  [[nodiscard]] Vst3EditorController* vst3_editor() const noexcept { return vst3_editor_.get(); }
  [[nodiscard]] QObject* native_synth_controller() const noexcept;
  [[nodiscard]] QObject* native_synth_automation() const noexcept;
  [[nodiscard]] QObject* native_synth_panel_model() const noexcept;
  [[nodiscard]] QString native_synth_track_id() const;
  [[nodiscard]] QVariantList native_synth_engines() const;
  // A drum-pad engine's keys as piano-roll lanes [{key, name}]: Pulse's pads by name, Knock's by
  // the sound track `track_id` gives each pad (or "Pad 1".. without one); empty for other engines.
  Q_INVOKABLE QVariantList native_synth_note_lanes(const QString& engine_id,
                                                   const QString& track_id = QString()) const;
  [[nodiscard]] QVariantList native_effect_engines() const;
  [[nodiscard]] QObject* native_effect_controller() const noexcept;
  [[nodiscard]] QObject* native_effect_automation() const noexcept;
  [[nodiscard]] QObject* native_effect_panel_model() const noexcept;
  [[nodiscard]] QVariantMap native_effect_context() const;
  // `path_id` is a track id or "master".
  Q_INVOKABLE void add_effect_engine(const QString& path_id, int index, const QString& engine_id);
  [[nodiscard]] QVariantList browser_catalogue() const;
  [[nodiscard]] QVariantList build_browser_catalogue() const;
  [[nodiscard]] QStringList browser_favourites() const;
  [[nodiscard]] QStringList browser_recents() const;
  Q_INVOKABLE void toggle_browser_favourite(const QString& key);
  Q_INVOKABLE void note_browser_use(const QString& key);
  // A new built-in instrument track playing `engine` at a factory preset
  // (-1: the engine defaults), selected once created.
  Q_INVOKABLE void create_native_instrument_track(const QString& engine_id, int preset_index);
  Q_INVOKABLE void replace_native_instrument(const QString& track_id, const QString& engine_id,
                                             int preset_index);
  Q_INVOKABLE void add_effect_preset(const QString& path_id, int index, const QString& engine_id,
                                     int preset_index);
  // A new Crate track playing a bundled starter sound as its one chop.
  Q_INVOKABLE void create_starter_sound_track(int sound_index);
  // A dry preview of an instrument preset through the audition device.
  Q_INVOKABLE void preview_browser_preset(const QString& engine_id, int preset_index);
  Q_INVOKABLE void stop_browser_preview();
  Q_INVOKABLE void open_native_effect_editor(const QString& path_id, const QString& slot_id);
  Q_INVOKABLE void close_native_effect_editor();
  // Whether `path_id`'s insert chain takes edits now: always when stopped,
  // and while playing when it holds only built-in effects.
  Q_INVOKABLE bool chain_edit_enabled(const QString& path_id) const;
  // The sidechain key of one effect slot on a track, for its panel's strip:
  // {available, keyed, sourceId, sendId, gain, keyName, sources: [{id, name}]}.
  // QML re-reads it when tracks change.
  Q_INVOKABLE QVariantMap sidechain_state(const QString& track_id, const QString& slot_id) const;
  // While playing: hear the open built-in effect's key in place of its
  // output, and read the key's held peak (0..1, linear; 0 when not keyed).
  Q_INVOKABLE void set_effect_key_listen(const QString& track_id, const QString& slot_id,
                                         bool listen);
  Q_INVOKABLE double effect_key_level(const QString& track_id, const QString& slot_id);
  // Horizontal device chain strips: one live controller per visible strip,
  // keyed by path ("master" or a track id) and slot ("__source__" names the
  // track's built-in instrument). Null for slots without a built-in engine.
  // Strips release theirs when they go away; a project change clears all.
  Q_INVOKABLE QObject* device_strip_controller(const QString& path_id, const QString& slot_id);
  Q_INVOKABLE void release_device_strip(const QString& path_id, const QString& slot_id);
  // {kind, accent, parameters: [ids]} for an engine's strip.
  Q_INVOKABLE QVariantMap device_strip_info(const QString& engine_id) const;

  void set_project(std::unique_ptr<application::ProjectSession> session,
                   const std::filesystem::path& opened_root);
  Q_INVOKABLE void create_default_project(const QString& project_name);
  Q_INVOKABLE void open_project(const QUrl& folder);
  Q_INVOKABLE void choose_open_project();
  Q_INVOKABLE void open_listed(const QString& path);
  Q_INVOKABLE void forget_project(const QString& path);
  // Saves, stops playback services and returns to the project screen.
  Q_INVOKABLE void close_project();
  Q_INVOKABLE void trash_project(const QString& path);
  Q_INVOKABLE void rename_project(const QString& path, const QString& name);
  Q_INVOKABLE void choose_recovery(const QString& generation, const QString& kind);
  Q_INVOKABLE void cancel_recovery();
  Q_INVOKABLE void add_instrument_track();
  Q_INVOKABLE void select_native_instrument(const QString& track_id, const QString& engine_id);
  Q_INVOKABLE void open_native_synth_editor(const QString& track_id);
  Q_INVOKABLE void close_native_synth_editor();
  Q_INVOKABLE void audition_native_synth_note(int key);
  // Knock: the open panel's working pattern, dry through the track's sound,
  // while the song is stopped (its opening, as many notes as a preview plays).
  Q_INVOKABLE void audition_knock_pattern();
  // Live playing from the piano roll's keys or the computer keyboard: the
  // track's built-in synth sounds `key` until release_note(key). Notes
  // play on the dry preview device, transport stopped or running.
  Q_INVOKABLE void play_note(const QString& track_id, int key, int velocity);
  Q_INVOKABLE void release_note(int key);
  // The track that owns a MIDI clip, or empty.
  Q_INVOKABLE QString clip_track_id(const QString& clip_id) const;
  Q_INVOKABLE void preview_note_velocity(const QString& clip_id, const QString& note_id,
                                         int velocity);
  Q_INVOKABLE void remove_track(const QString& track_id);
  Q_INVOKABLE void duplicate_track(const QString& track_id);
  Q_INVOKABLE bool move_track(const QString& track_id, int to_index);
  Q_INVOKABLE void set_track_muted(const QString& track_id, bool muted);
  Q_INVOKABLE void set_track_soloed(const QString& track_id, bool soloed);
  Q_INVOKABLE void set_track_level_pan(const QString& track_id, double level, double pan);
  Q_INVOKABLE void set_master_level(double level);
  // A fader, pan or send knob mid-drag: heard at once while playing, with no
  // saved edit; the gesture's release saves it (one edit, one undo step).
  Q_INVOKABLE void preview_track_level_pan(const QString& track_id, double level, double pan);
  Q_INVOKABLE void preview_master_level(double level);
  Q_INVOKABLE void preview_send_gain(const QString& send_id, double gain);
  Q_INVOKABLE void add_bus_track(const QString& kind);
  Q_INVOKABLE void set_main_route(const QString& track_id, const QString& destination_id);
  Q_INVOKABLE void add_send_route(const QString& track_id, const QString& destination_id,
                                  double gain);
  Q_INVOKABLE void set_send_gain(const QString& send_id, double gain);
  Q_INVOKABLE void remove_send(const QString& send_id);
  Q_INVOKABLE void select_audio_clip(const QString& clip_id);
  Q_INVOKABLE void set_arrangement_viewport(double first_bar, double visible_bars, int pixel_width);
  [[nodiscard]] QObject* track_model() const noexcept;
  // The waveform columns of the clip's part in view (its clip model's
  // `waveform` role).
  [[nodiscard]] QVariantList clip_waveform(const QString& clip_id) const;
  Q_INVOKABLE void rebuild_clip_waveform(const QString& clip_id);
  Q_INVOKABLE void set_tempo_event_page(int page);
  Q_INVOKABLE void add_tempo_event(int bar, int beat, double bpm, int shape_index);
  Q_INVOKABLE void update_tempo_event(const QString& tick, double bpm, int shape_index);
  Q_INVOKABLE void set_initial_tempo_bpm(double bpm);
  Q_INVOKABLE void set_initial_meter(int numerator, int denominator);
  Q_INVOKABLE void remove_tempo_event(const QString& tick);
  // Where the playhead is inside a MIDI clip while playing, in the clip's own
  // ticks (a looping clip wraps at its pattern length), or -1 when stopped or
  // outside the clip. QML re-reads it on playbackChanged.
  Q_INVOKABLE double clip_playhead_ticks(const QString& clip_id) const;
  Q_INVOKABLE void add_midi_clip(const QString& track_id, int bar_index);
  Q_INVOKABLE void duplicate_midi_clip_after(const QString& clip_id);
  Q_INVOKABLE void delete_midi_clip(const QString& clip_id);
  Q_INVOKABLE void toggle_midi_clip_timebase(const QString& clip_id);
  Q_INVOKABLE void move_midi_clip(const QString& clip_id, double normalized_start);
  Q_INVOKABLE void resize_midi_clip(const QString& clip_id, double normalized_end);
  Q_INVOKABLE void set_midi_clip_loop(const QString& clip_id, bool loop);
  Q_INVOKABLE void add_note(const QString& clip_id, int step_index, int key);
  Q_INVOKABLE void toggle_note_grid(const QString& clip_id, int step, int key, int steps_per_bar);
  Q_INVOKABLE void move_note_grid(const QString& clip_id, const QString& note_id, int to_step,
                                  int to_key, int steps_per_bar);
  Q_INVOKABLE void remove_note_id(const QString& clip_id, const QString& note_id);
  // A note selection, each as one undo step: delete, or move by grid steps
  // and semitones together.
  Q_INVOKABLE void remove_notes(const QString& clip_id, const QStringList& note_ids);
  Q_INVOKABLE void move_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                   int step_delta, int key_delta, int steps_per_bar);
  // A Ctrl-drag: copies of the notes land at the drop, the originals stay.
  // Returns the copies' ids, so the roll can select them.
  Q_INVOKABLE QStringList copy_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                          int step_delta, int key_delta, int steps_per_bar);
  Q_INVOKABLE void resize_note_grid(const QString& clip_id, const QString& note_id, int end_step,
                                    int steps_per_bar);
  Q_INVOKABLE void quantize_midi_clip(const QString& clip_id, int steps_per_bar);
  Q_INVOKABLE void move_audio_clip(const QString& clip_id, double normalized_start,
                                   bool snap_to_beat);
  // Onto another audio track; the start is already snapped by the caller.
  Q_INVOKABLE void move_audio_clip_to_track(const QString& clip_id, const QString& track_id,
                                            double normalized_start);
  Q_INVOKABLE void nudge_audio_clip(const QString& clip_id, int beat_direction);
  Q_INVOKABLE void nudge_midi_clip(const QString& clip_id, int beat_direction);
  Q_INVOKABLE void move_midi_clip_to_track(const QString& clip_id, const QString& track_id,
                                           double normalized_start);
  Q_INVOKABLE void trim_audio_clip(const QString& clip_id, bool left_edge, double normalized_edge,
                                   bool snap_to_beat);
  Q_INVOKABLE void set_audio_clip_fade(const QString& clip_id, bool fade_in, double fraction,
                                       int curve_index);
  Q_INVOKABLE void duplicate_audio_clip_after(const QString& clip_id);
  // Clipboard and arrangement shortcuts (Ctrl+C/X/V/D, Ctrl+M, [ and ]).
  // They act on the selection properties above; notes win when selected.
  Q_INVOKABLE void copy_selection();
  Q_INVOKABLE void cut_selection();
  Q_INVOKABLE void paste_at_playhead();
  Q_INVOKABLE void duplicate_selection();
  // The selected clips together: moved by the dragged clip's move from
  // `from` to `to` (normalised timeline positions) and `track_delta` rows,
  // deleted, or (MIDI clips on one track) merged into one. One undo step each.
  Q_INVOKABLE void move_selected_clips(double from, double to, int track_delta);
  // The selected clips one beat later (+1) or earlier (-1), each on its own
  // track, as one undo step; refused as a whole when any clip would start
  // before bar 1.
  Q_INVOKABLE void nudge_selected_clips(int beat_direction);
  Q_INVOKABLE void delete_selected_clips();
  Q_INVOKABLE void merge_selected_clips();
  Q_INVOKABLE void add_marker_at_playhead();
  // Tonic 0–11 from C; mode "major" or "minor".
  Q_INVOKABLE void set_project_key(int tonic, const QString& mode);
  Q_INVOKABLE void use_suggested_project_key();
  Q_INVOKABLE void clear_project_key();
  // +1 jumps to the next marker, -1 to the previous one.
  Q_INVOKABLE void jump_to_marker(int direction);
  Q_INVOKABLE void rename_marker(const QString& marker_id, const QString& name);
  Q_INVOKABLE void remove_marker(const QString& marker_id);
  // "#rrggbb", or empty to return to the palette.
  Q_INVOKABLE void set_track_color(const QString& track_id, const QString& color);
  Q_INVOKABLE void set_clip_color(const QString& clip_id, const QString& color);
  // An audio clip's shown name; empty shows the track's.
  Q_INVOKABLE void set_audio_clip_name(const QString& clip_id, const QString& name);
  Q_INVOKABLE void set_track_name(const QString& track_id, const QString& name);
  // `gain` is linear 0..4; adds the send when missing.
  Q_INVOKABLE void set_send_level(const QString& track_id, const QString& return_id, double gain);
  [[nodiscard]] QVariantList markers() const;
  [[nodiscard]] QString project_key() const;
  [[nodiscard]] QString suggested_project_key() const;
  [[nodiscard]] QVariantList project_scale() const;
  [[nodiscard]] QStringList selected_clip_ids() const { return selected_clip_ids_; }
  void set_selected_clip_ids(const QStringList& ids);
  [[nodiscard]] QString selected_track_id() const { return selected_track_id_; }
  // Install as an application event filter (main.cpp).
  [[nodiscard]] ComputerKeyboard* computer_keyboard() const noexcept {
    return computer_keyboard_.get();
  }
  [[nodiscard]] QObject* computer_keyboard_object() const;
  void set_selected_track_id(const QString& id);
  [[nodiscard]] QString selected_note_clip_id() const { return selected_note_clip_id_; }
  void set_selected_note_clip_id(const QString& id);
  [[nodiscard]] QStringList selected_note_ids() const { return selected_note_ids_; }
  void set_selected_note_ids(const QStringList& ids);
  Q_INVOKABLE void split_audio_clip_at_playhead(const QString& clip_id);
  Q_INVOKABLE void split_midi_clip_at_playhead(const QString& clip_id);
  Q_INVOKABLE void slip_audio_clip(const QString& clip_id, double source_frame_delta);
  Q_INVOKABLE void toggle_audio_clip_timebase(const QString& clip_id);
  Q_INVOKABLE void delete_audio_clip(const QString& clip_id);
  // Convert to sampler: Crate on a new instrument track below, chopped at
  // the clip's hits, with a MIDI clip playing them; one undo step.
  Q_INVOKABLE void convert_audio_clip_to_sampler(const QString& clip_id);
  // Puts Crate on the track (unless it already plays it) with a bundled
  // starter sound as its one chop.
  Q_INVOKABLE void bind_starter_sound(const QString& track_id, int sound_index);
  Q_INVOKABLE void set_send_automation_point(const QString& send_id, int zero_based_bar,
                                             double value);
  Q_INVOKABLE void set_send_automation_value(const QString& send_id, const QString& point_id,
                                             double value);
  Q_INVOKABLE void remove_send_automation_point(const QString& send_id, const QString& point_id);
  Q_INVOKABLE void remove_send_automation_lane(const QString& send_id);
  Q_INVOKABLE void move_effect_insert(const QString& track_id, const QString& slot_id, int index);
  Q_INVOKABLE void set_effect_insert_enabled(const QString& track_id, const QString& slot_id,
                                             bool enabled);
  Q_INVOKABLE void remove_effect_insert(const QString& track_id, const QString& slot_id);
  Q_INVOKABLE void save();
  // Whether a background save or autosave is writing the project now. A
  // save() requested meanwhile runs after it, saving the latest state.
  [[nodiscard]] bool save_in_flight() const noexcept {
    return background_save_job_.running() || autosave_job_.running();
  }
  Q_INVOKABLE void save_recovery_snapshot() { start_autosave(); }
  Q_INVOKABLE void refresh_named_revisions();
  Q_INVOKABLE void create_named_revision(const QString& name);
  Q_INVOKABLE void restore_named_revision(const QString& name);
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void toggle_playback();
  Q_INVOKABLE void toggle_pause();
  Q_INVOKABLE void stop_playback();
  Q_INVOKABLE void seek_to_bar(double zero_based_bar);
  Q_INVOKABLE double elapsed_seconds_at_bar(double zero_based_bar) const;
  Q_INVOKABLE void seek_to_sample(qulonglong sample);
  Q_INVOKABLE void set_track_row_height(const QString& track_id, int height);
  Q_INVOKABLE void collect_project_copy();
  Q_INVOKABLE void cancel_project_collection();
  // A window close waits for worker cancellation, playback retirement or export publication.
  Q_INVOKABLE bool request_close();

signals:
  // The project opened, closed, or was renamed or reformatted.
  void projectChanged();
  void revisionChanged();
  void tracksChanged();
  void markersChanged();
  void projectKeyChanged();
  void timelineChanged();
  void insertChainsChanged();
  void routingChanged();
  void masterLevelChanged();
  void selectionChanged();
  void trackCreated(int index);
  void trackRowHeightsChanged();
  void namedRevisionsChanged();
  void namedRevisionApplied();
  void collectionChanged();
  void projectTimingChanged();
  void tempoMapChanged();
  void availableProjectsChanged();
  void recoveryChoiceChanged();
  void statusChanged();
  void playbackChanged();
  void metersChanged();
  void insertChainEditEnabledChanged();
  void nativeSynthControllerChanged();
  void nativeEffectControllerChanged();
  void nativeEffectContextChanged();
  void browserCollectionsChanged();
  void readyToClose();
  void autosaveIntervalChanged();
  void autosaveStatusChanged();
  void saveProblemChanged();

private:
  // Construction, by feature.
  void start_timers();
  void create_audio_output(application::PlaybackOutputFactory output_factory,
                           application::PlaybackOutputFactory jack_output_factory,
                           std::function<bool()> jack_server_probe, AudioDeviceDiscovery discovery);
  void create_meter_model();
  void create_clip_controllers();
  void create_mix_controllers();
  void create_note_controllers();
  void create_song_controllers();
  void create_job_controllers();
  void create_vst3_controllers();
  void create_diagnostics();
  void create_lifecycle();
  void create_live_input();
  // A controller that recomputes on every revision and project change.
  template <typename Controller>
  void follow_project(Controller* controller, void (Controller::*changed)()) {
    connect(this, &ProjectBridge::revisionChanged, controller, changed);
    connect(this, &ProjectBridge::projectChanged, controller, changed);
  }
  // What warp editing needs to know about the run playing now.
  [[nodiscard]] WarpPlaybackObservation warp_playback() const;
  // Shared gates. `jobs_idle` is true when no playback, import, export,
  // media repair or close holds the project, so another job may start.
  [[nodiscard]] bool jobs_idle() const;
  [[nodiscard]] bool import_busy() const;
  [[nodiscard]] bool export_busy() const;
  [[nodiscard]] bool realtime_export_busy() const;
  [[nodiscard]] bool closing_after_export() const;
  [[nodiscard]] bool managed_media_busy() const;
  [[nodiscard]] bool managed_media_repair_enabled() const;
  [[nodiscard]] bool vst3_browser_busy() const;
  // The edit gate the feature controllers share: no audio edit, playback
  // start, export or import holds the project (edit_allowed: and one is open).
  [[nodiscard]] bool edit_gate_open() const;
  [[nodiscard]] bool edit_allowed() const;
  // edit_allowed, reporting `refusal` when it is not.
  bool guarded_edit(const QString& refusal);
  // After a committed edit: re-project and update playback.
  void commit_edit();
  [[nodiscard]] bool route_edit_allowed();
  void refresh();
  void publish_project_timing();
  void publish_tempo_map();
  void set_status(QString value);
  void finish_insert_chain_edit(const foundation::Result<void>& result, const QString& action);
  [[nodiscard]] bool live_routed_insert_edit_enabled() const;
  [[nodiscard]] bool live_chain_structure_edit_enabled(const QString& track_id) const;
  void remember_project(const QString& path);
  void refresh_available_projects();
  // Refuses (with a status) while a save, export, import or playback is
  // busy; otherwise saves unsaved changes.
  [[nodiscard]] bool ready_to_change_project();
  void release_project();
  [[nodiscard]] bool save_current_project_if_dirty();
  void poll_playback();
  // Throttled playback-timer slot, then the unconditional snapshot it takes.
  void poll_meters();
  void refresh_meters();
  // Logs the audio threads' queued diagnostics and shows their summary.
  void drain_realtime_diagnostics();
  // Logs each distinct status line the job controllers show.
  void log_controller_statuses();
  StatusLog status_log_;
  [[nodiscard]] bool track_meters_live() const noexcept;
  struct PlaybackViewOptions {
    bool include_graph = false;
    bool include_direct_graph = false;
    bool include_fault = false;
    bool include_output = false;
  };
  [[nodiscard]] TransportController::PlaybackView playback_view(PlaybackViewOptions options) const;
  [[nodiscard]] TransportController::PlaybackView playback_view() const {
    return playback_view(PlaybackViewOptions{});
  }
  void start_autosave();
  void poll_autosave();
  void schedule_background_save();
  void start_background_save();
  void poll_background_save();
  // Waits for a background save or autosave that is writing the project and
  // installs its result, so a save requested now runs after it.
  void finish_in_flight_saves();
  void begin_live_audio_edit();
  void submit_audio_clip_edit();
  void begin_async_playback_stop();
  void begin_routed_playback();
  // Live monitoring (project_bridge_monitor.cpp): notes played while the
  // transport is stopped run through the project's real graph with its
  // clips muted, so track effects, sends, the master chain, analysers and
  // meters all hear them. The run that is sounding now, transport or monitor
  // (null when neither runs).
  [[nodiscard]] RoutedPlayback* live_run() const noexcept;
  // The routed transport run, or null while none runs or its stop worker
  // owns it: that worker resets the run's control, source and output, so
  // every UI-thread access goes through here. The stop completion is set and
  // cleared on the UI thread, around the worker's whole life.
  [[nodiscard]] RoutedPlayback* routed_run() const noexcept;
  void start_monitor();
  void poll_monitor();
  // Stops at once (the callback is joined). With a preparation in flight it
  // asks it to stop and returns false; poll_monitor finishes the job.
  bool stop_monitor_now();
  [[nodiscard]] bool transport_running() const noexcept;
  void send_live_note(const QString& track_id, int key, int velocity, bool on);
  // The dry preview device: live notes when no graph can play them.
  void play_note_dry(const QString& track_id, int key, int velocity);
  void begin_async_routed_stop(bool graceful = false);
  enum class RoutedTransportOperation : std::uint8_t { pause, resume, seek };
  void begin_async_routed_transport(RoutedTransportOperation operation, std::uint64_t sample = 0);
  void poll_routed_playback();
  void poll_live_audio_edit();
  void submit_playback_edit();
  void audition_edited_note(const QString& clip_id, const QString& note_id, std::uint8_t velocity);
  [[nodiscard]] QString track_display_name(project::TrackId track) const;
  void refresh_native_effect_context();
  void audition_native_synth(project::TrackId track, const native_dsp::synth::SynthEngine& engine,
                             std::uint8_t key, std::uint8_t velocity,
                             bool replace_previous = false);
  // The preview worker, made on first use; false when it cannot start.
  bool ensure_native_synth_audition();
  // project_bridge_browser.cpp: builds browser_preview_ and key_match_.
  void create_browser_previews();
  // The preview device's output factory and target, or false (and a status
  // message) when the device is unavailable.
  bool preview_output(application::PlaybackOutputFactory& factory,
                      std::optional<std::uint64_t>& target_serial);
  void audition_values(const native_dsp::synth::SynthEngine& engine, std::vector<double> values,
                       std::uint8_t key, std::uint8_t velocity, bool replace_previous);
  void set_playback_status(QString value);
  [[nodiscard]] bool playback_busy() const;
  // The first sample of bar 1, after any bars before it.
  [[nodiscard]] std::uint64_t bar_one_sample() const;
  // Where Stop goes: the loop's start while a loop is on, otherwise bar 1.
  [[nodiscard]] std::uint64_t stop_sample() const;
  // MIDI recording (project_bridge_recording.cpp, ADR-0086).
  void make_recording();
  // Lists the song's beats for the click when the tempo or meter changed.
  void publish_click_schedule();
  [[nodiscard]] std::optional<std::uint64_t> heard_sample() const;
  [[nodiscard]] bool song_playing() const;
  [[nodiscard]] bool control_busy() const;

  struct PlaybackCompletion {
    std::atomic<bool> done{false};
    std::optional<foundation::Result<application::PreparedInstrumentPlayback>> result;
  };
  struct RoutedCompletion {
    std::atomic<bool> done{false};
    std::atomic<bool> stop_requested{false};
    std::optional<foundation::Result<std::unique_ptr<RoutedPlayback>>> result;
  };
  struct RoutedStopCompletion {
    std::atomic<bool> done{false};
    RoutedPlayback::StopDrainOutcome drain = RoutedPlayback::StopDrainOutcome::not_requested;
  };
  struct RoutedTransportCompletion {
    std::atomic<bool> done{false};
    std::atomic<bool> stop_requested{false};
    RoutedTransportOperation operation = RoutedTransportOperation::pause;
    std::uint64_t sample = 0;
    std::optional<foundation::Result<void>> result;
  };
  struct AudioEditCompletion {
    std::atomic<bool> done{false};
    std::atomic<bool> stop_requested{false};
    bool stopping_only = false;
    std::optional<foundation::Result<void>> result;
  };

  std::unique_ptr<application::ProjectSession> session_;
  std::unique_ptr<SessionNativeSynthAdapter> native_synth_adapter_;
  std::unique_ptr<NativeSynthDeviceController> native_synth_controller_;
  std::unique_ptr<NativeSynthAutomationController> native_synth_automation_;
  // The open synth panel's own model (see nativeSynthPanelModel), and the
  // same object as Knock's when the panel is Knock's (its patterns audition).
  std::unique_ptr<QObject> native_synth_panel_model_;
  KnockPanelModel* native_synth_knock_ = nullptr;
  std::unique_ptr<NativeSynthAudition> native_synth_audition_;
  std::unique_ptr<ComputerKeyboard> computer_keyboard_;
  // The device-only click every main playback run carries (ADR-0086).
  std::shared_ptr<audio_io::Metronome> metronome_;
  // The tempo and meter revisions its beats were last listed from.
  std::optional<std::pair<std::uint64_t, std::uint64_t>> click_maps_;
  std::unique_ptr<RecordingController> recording_;
  bool native_synth_editor_open_ = false;
  std::unique_ptr<SessionNativeSynthAdapter> native_effect_adapter_;
  std::unique_ptr<NativeSynthDeviceController> native_effect_controller_;
  std::unique_ptr<NativeSynthAutomationController> native_effect_automation_;
  std::unique_ptr<QObject> native_effect_panel_model_;
  QVariantMap native_effect_context_;
  // The effect whose key the panel is listening to while playing.
  std::optional<std::pair<project::TrackId, project::InsertSlotId>> effect_key_listen_;
  // Device chain strips (see device_strip_controller). The controller is
  // declared after its adapter so it is destroyed first.
  struct DeviceStripEntry {
    std::string engine;
    int users = 0;
    std::unique_ptr<SessionNativeSynthAdapter> adapter;
    std::unique_ptr<NativeSynthDeviceController> controller;
  };
  std::map<QString, DeviceStripEntry> device_strips_;
  void clear_device_strips();
  QVariantMap track_row_heights_;
  std::unique_ptr<application::InstrumentPlayback> playback_;
  std::unique_ptr<RoutedPlayback> routed_playback_;
  // The monitor run, its preparation, the notes waiting for it, and which
  // track each held live key plays.
  struct PendingLiveNote {
    project::TrackId track;
    std::uint8_t key;
    std::uint8_t velocity;
    bool on;
  };
  std::unique_ptr<RoutedPlayback> monitor_;
  std::shared_ptr<RoutedCompletion> monitor_completion_;
  std::jthread monitor_worker_;
  std::vector<PendingLiveNote> monitor_pending_;
  QTimer monitor_timer_;
  QElapsedTimer monitor_idle_;
  bool play_after_monitor_ = false;
  std::map<int, project::TrackId> live_note_tracks_;
  std::shared_ptr<RoutedCompletion> routed_completion_;
  std::jthread routed_worker_;
  std::shared_ptr<RoutedStopCompletion> routed_stop_completion_;
  std::jthread routed_stop_worker_;
  std::shared_ptr<RoutedTransportCompletion> routed_transport_completion_;
  std::jthread routed_transport_worker_;
  std::optional<std::pair<RoutedTransportOperation, std::uint64_t>> deferred_routed_transport_;
  TransportController transport_controller_;
  std::uint64_t playhead_sample_ = 0;
  QString routed_stop_reason_;
  QString routed_transport_failure_;
  bool routed_limiter_active_ = false;
  std::shared_ptr<native_dsp::StereoMeter> meter_ = std::make_shared<native_dsp::StereoMeter>();
  std::shared_ptr<application::SpectrumMonitor> spectrum_ =
      std::make_shared<application::SpectrumMonitor>();
  std::shared_ptr<LiveTruePeakMonitor> true_peak_ = std::make_shared<LiveTruePeakMonitor>();
  std::shared_ptr<std::atomic<bool>> mono_listen_ = std::make_shared<std::atomic<bool>>(false);
  QVariantMap track_meters_;
  QElapsedTimer meter_clock_;
  std::unique_ptr<MeterModel> meter_model_;
  QStringList selected_clip_ids_;
  QString selected_track_id_;
  QString selected_note_clip_id_;
  QStringList selected_note_ids_;
  std::shared_ptr<PlaybackCompletion> playback_completion_;
  std::jthread playback_worker_;
  std::shared_ptr<AudioEditCompletion> audio_edit_completion_;
  std::jthread audio_edit_worker_;
  WorkerRunner audio_edit_runner_;
  LiveEditHandoffHook before_live_edit_handoff_;
  QTimer playback_timer_;
  QTimer autosave_timer_;
  QTimer autosave_poll_timer_;
  QTimer background_save_timer_;
  QTimer background_save_poll_timer_;
  // A save on a worker, and what it saves: the project and its revision.
  using SaveJob = BackgroundJob<foundation::Result<persistence::SaveOutcome>>;
  struct SaveTicket {
    std::optional<project::ProjectId> project;
    std::uint64_t revision = 0;
  };
  SaveJob autosave_job_;
  SaveTicket autosave_ticket_;
  SaveJob background_save_job_;
  SaveTicket background_save_ticket_;
  bool background_save_pending_ = false;
  bool close_after_background_save_ = false;
  std::uint64_t last_autosave_revision_ = 0;
  int autosave_interval_seconds_ = 120;
  QString autosave_status_;
  QString save_problem_;
  void set_save_problem(QString problem);
  bool close_after_autosave_ = false;
  std::unique_ptr<KeyReviewController> key_review_;
  std::unique_ptr<ClipAutomationController> clip_automation_;
  std::unique_ptr<ArrangementAutomationController> arrangement_automation_;
  std::unique_ptr<ClipWarpController> clip_warp_;
  std::unique_ptr<ClipTempoController> clip_tempo_;
  std::unique_ptr<KeyMatchController> key_match_;
  std::unique_ptr<BrowserPreviewController> browser_preview_;
  std::unique_ptr<LoopTransport> loop_transport_;
  std::unique_ptr<PerformanceTrace> performance_trace_ = std::make_unique<PerformanceTrace>();
  std::unique_ptr<TrackAutomationController> track_automation_;
  std::unique_ptr<PluginAutomationController> plugin_automation_;
  std::unique_ptr<RouteCommands> route_commands_;
  std::unique_ptr<TrackCommands> track_commands_;
  std::unique_ptr<HistoryController> history_;
  std::unique_ptr<CommandPalette> command_palette_;
  std::unique_ptr<DiagnosticsController> diagnostics_;
  std::unique_ptr<MasterSection> master_section_;
  std::unique_ptr<NoteToolsController> note_tools_;
  std::unique_ptr<NoteEffectsController> note_effects_;
  std::unique_ptr<FreezeController> freeze_commands_;
  std::unique_ptr<MixCheckController> mix_check_;
  std::unique_ptr<ArrangeController> arrange_;
  std::unique_ptr<HarmonyController> harmony_;
  std::unique_ptr<GrooveCaptureController> groove_capture_;
  std::unique_ptr<ClayController> clay_;
  // project_bridge_macros.cpp: the macro strip, the racks, and the open
  // panels following a turning macro.
  std::unique_ptr<MacroController> macros_;
  std::unique_ptr<RackLibraryController> racks_;
  void create_macro_controllers();
  void show_macro_values(project::TrackId track, std::optional<project::InsertSlotId> slot,
                         const std::vector<double>* values);
  // Every device controller shares the macro strip's MAP mode.
  void share_macro_map(NativeSynthDeviceController& controller);
  std::unique_ptr<TempoMapCommands> tempo_map_commands_;
  std::unique_ptr<WorkspaceLayoutController> workspace_layout_;
  std::unique_ptr<ManagedMediaController> managed_media_;
  std::unique_ptr<ImportController> imports_;
  std::unique_ptr<AudioOutputController> audio_output_;
  std::unique_ptr<ExportController> exports_;
  std::unique_ptr<Vst3BrowserController> vst3_browser_;
  std::unique_ptr<Vst3EditorController> vst3_editor_;
  std::unique_ptr<ProjectCollectionController> collection_;
  std::filesystem::path waveform_root_;
  std::unique_ptr<WaveformController> waveform_;
  QVariantMap project_timing_;
  // The lead bars the loop range was last placed for.
  int shown_lead_bars_ = 0;
  QVariantMap tempo_map_;
  // Empty means the ordinary stopped message. Other messages remain opaque
  // presentation text; consumers use playbackState for decisions.
  QString playback_status_;
  std::optional<application::RevisionStatus> observed_routed_graph_;
  QString source_underrun_detail_;
  QString audio_edit_failure_;
  bool close_after_playback_ = false;
  bool stop_worker_start_failed_ = false;
  QString project_name_;
  std::unique_ptr<ProjectLifecycleController> lifecycle_;
  std::unique_ptr<CacheUiController> cache_ui_;
  qulonglong revision_ = 0;
  qulonglong next_device_gesture_ = 1;
  ProjectSummaryView summary_;
  QVariantList tracks_;
  // One row per track, each with its clips' model: delegates rebind only
  // for the rows that changed. A clip's `waveform` role holds the columns
  // of its part in view (a zoom or scroll recomputes only those), and its
  // `tempo` role its ClipTempoController entry.
  KeyedListModel* track_model_ = nullptr;
  QHash<QString, KeyedListModel*> clip_models_;
  void refresh_clip_waveforms();
  void publish_track_models(const QVariantList& tracks);
  void publish_clip_tempo();
  // The values QML last received, so each change signal fires only when its
  // projection changed.
  struct Published {
    bool open = false;
    QString name;
    QString format;
    qulonglong revision = 0;
    QVariantList markers;
    QString key;
    QString suggested_key;
    int timeline_bars = 0;
    int lead_bars = 0;
    QVariantList insert_chains;
    QVariantList route_targets;
    int route_target_count = 0;
    bool routing_active = false;
    double master_level = 1.0;
  };
  Published published_;
  int timeline_bars_ = 16;
  QVariantList insert_chains_;
  QVariantList route_targets_;
  int route_target_count_ = 0;
  bool routing_active_ = false;
  QString status_;
  std::filesystem::path projects_root_;
  std::uint64_t project_generation_ = 0;
  // Exact binary/class/control/rate/block qualifications for live manual edits.
  Vst3LiveQualification vst3_live_qualification_;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Vst3BinaryHasher vst3_binary_hasher_;
#endif
  mutable std::optional<QVariantList> browser_catalogue_cache_;
};

} // namespace oscillate::quick
