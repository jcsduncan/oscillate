#pragma once

#include "oscillate/application/control_transport_gate.hpp"
#include "oscillate/application/session/audio_clip_service.hpp"
#include "oscillate/application/session/automation_service.hpp"
#include "oscillate/application/session/clipboard_service.hpp"
#include "oscillate/application/session/effect_service.hpp"
#include "oscillate/application/session/export_service.hpp"
#include "oscillate/application/session/freeze_service.hpp"
#include "oscillate/application/session/history_service.hpp"
#include "oscillate/application/session/import_service.hpp"
#include "oscillate/application/session/instrument_service.hpp"
#include "oscillate/application/session/media_repair_service.hpp"
#include "oscillate/application/session/midi_clip_service.hpp"
#include "oscillate/application/session/note_service.hpp"
#include "oscillate/application/session/parameter_automation_service.hpp"
#include "oscillate/application/session/pedal_service.hpp"
#include "oscillate/application/session/routing_service.hpp"
#include "oscillate/application/session/save_service.hpp"
#include "oscillate/application/session/song_service.hpp"
#include "oscillate/application/session/source_analysis_service.hpp"
#include "oscillate/application/session/track_device_service.hpp"
#include "oscillate/application/session/track_service.hpp"
#include "oscillate/application/session/warp_service.hpp"
#include "oscillate/persistence/store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::application {

class InstrumentPlayback;

// Control/UI thread only. Owns a real project model and durable project
// store, and composes the services that edit them (session/*_service.hpp).
// Callers edit the project through a service's accessor.
class ProjectSession {
public:
  struct RecoveryChoice {
    std::string generation;
    persistence::SaveKind kind;
    std::uint64_t revision;
    std::size_t tracks;
    std::size_t clips;
    bool pointed = false;
  };
  struct RecoveryInspection {
    std::vector<RecoveryChoice> choices;
    std::optional<std::string> current_issue;
    std::optional<std::string> autosave_issue;
    bool has_current = false;
    bool has_autosave = false;
  };

  // What a new project starts with. The app's New Project asks for the
  // default return tracks; tests and tools start empty.
  struct CreateOptions {
    // "A · Reverb" (Expanse, Vocal Plate, fully wet) and "B · Delay" (Relay,
    // Dotted Eighth, fully wet) return tracks.
    bool default_returns = false;
    // Tests inject storage faults into the creation and its first save.
    persistence::FaultHook fault = {};
  };

  ProjectSession(const ProjectSession&) = delete;
  ProjectSession& operator=(const ProjectSession&) = delete;
  ProjectSession(ProjectSession&&) = delete;
  ProjectSession& operator=(ProjectSession&&) = delete;
  ~ProjectSession();
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectSession>>
  create(const std::filesystem::path& root);
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectSession>>
  create(const std::filesystem::path& root, CreateOptions options);
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectSession>>
  open(const std::filesystem::path& root, persistence::FaultHook save_fault = {});
  [[nodiscard]] static foundation::Result<RecoveryInspection>
  inspect_recovery(const std::filesystem::path& root);
  // Revalidates the selected generation before opening it. An autosave remains
  // an unsaved working state; only a later explicit Save advances CURRENT.
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectSession>>
  open_recovered(const std::filesystem::path& root, std::string_view generation,
                 persistence::SaveKind kind, bool expected_pointed);

  // Set when create() renamed the new project folder into place but flushing
  // its entry in the parent failed: the project exists, its durability is
  // uncertain.
  [[nodiscard]] const std::optional<foundation::Error>& creation_warning() const noexcept {
    return creation_warning_;
  }

  [[nodiscard]] const project::Snapshot& snapshot() const noexcept;
  // Unique among the sessions this process opens, never reused. A job
  // started in one session captures it and drops its result when the session
  // it finishes in has another one, even a copy of the same project.
  [[nodiscard]] std::uint64_t session_generation() const noexcept { return session_generation_; }
  // Reserve playback before starting the device. The caller must stop and
  // join its callback before finishing the returned token.
  [[nodiscard]] foundation::Result<ControlTransportGate::PlaybackToken> reserve_playback_start();

  // The services this session composes.
  [[nodiscard]] SaveService& saving() noexcept { return saving_; }
  [[nodiscard]] const SaveService& saving() const noexcept { return saving_; }
  [[nodiscard]] HistoryService& history() noexcept { return history_; }
  [[nodiscard]] const HistoryService& history() const noexcept { return history_; }
  [[nodiscard]] MediaRepairService& media_repair() noexcept { return media_repair_; }
  [[nodiscard]] const MediaRepairService& media_repair() const noexcept { return media_repair_; }
  [[nodiscard]] TrackService& tracks() noexcept { return tracks_; }
  [[nodiscard]] const TrackService& tracks() const noexcept { return tracks_; }
  [[nodiscard]] RoutingService& routing() noexcept { return routing_; }
  [[nodiscard]] const RoutingService& routing() const noexcept { return routing_; }
  [[nodiscard]] SongService& song() noexcept { return song_; }
  [[nodiscard]] const SongService& song() const noexcept { return song_; }
  [[nodiscard]] SourceAnalysisService& source_analysis() noexcept { return source_analysis_; }
  [[nodiscard]] const SourceAnalysisService& source_analysis() const noexcept {
    return source_analysis_;
  }
  [[nodiscard]] AutomationService& automation() noexcept { return automation_; }
  [[nodiscard]] const AutomationService& automation() const noexcept { return automation_; }
  [[nodiscard]] ParameterAutomationService& parameter_automation() noexcept {
    return parameter_automation_;
  }
  [[nodiscard]] const ParameterAutomationService& parameter_automation() const noexcept {
    return parameter_automation_;
  }
  [[nodiscard]] EffectService& effects() noexcept { return effects_; }
  [[nodiscard]] const EffectService& effects() const noexcept { return effects_; }
  [[nodiscard]] InstrumentService& instruments() noexcept { return instruments_; }
  [[nodiscard]] const InstrumentService& instruments() const noexcept { return instruments_; }
  [[nodiscard]] TrackDeviceService& track_devices() noexcept { return track_devices_; }
  [[nodiscard]] const TrackDeviceService& track_devices() const noexcept { return track_devices_; }
  [[nodiscard]] MidiClipService& midi_clips() noexcept { return midi_clips_; }
  [[nodiscard]] const MidiClipService& midi_clips() const noexcept { return midi_clips_; }
  [[nodiscard]] NoteService& notes() noexcept { return notes_; }
  [[nodiscard]] const NoteService& notes() const noexcept { return notes_; }
  [[nodiscard]] PedalService& pedals() noexcept { return pedals_; }
  [[nodiscard]] const PedalService& pedals() const noexcept { return pedals_; }
  [[nodiscard]] AudioClipService& audio_clips() noexcept { return audio_clips_; }
  [[nodiscard]] const AudioClipService& audio_clips() const noexcept { return audio_clips_; }
  [[nodiscard]] WarpService& warp() noexcept { return warp_; }
  [[nodiscard]] const WarpService& warp() const noexcept { return warp_; }
  [[nodiscard]] ClipboardService& clipboard() noexcept { return clipboard_; }
  [[nodiscard]] const ClipboardService& clipboard() const noexcept { return clipboard_; }
  [[nodiscard]] ImportService& imports() noexcept { return imports_; }
  [[nodiscard]] const ImportService& imports() const noexcept { return imports_; }
  [[nodiscard]] ExportService& exports() noexcept { return exports_; }
  [[nodiscard]] const ExportService& exports() const noexcept { return exports_; }
  [[nodiscard]] FreezeService& freeze() noexcept { return freeze_; }
  [[nodiscard]] const FreezeService& freeze() const noexcept { return freeze_; }

private:
  friend class InstrumentPlayback;
  ProjectSession(std::filesystem::path root, persistence::ProjectStore store);

  // Declared first: every service refers to it, and the repair service's
  // worker must stop before it goes.
  std::unique_ptr<SessionState> state_;
  SaveService saving_;
  HistoryService history_;
  MediaRepairService media_repair_;
  TrackService tracks_;
  RoutingService routing_;
  AutomationService automation_;
  ParameterAutomationService parameter_automation_;
  SongService song_;
  SourceAnalysisService source_analysis_;
  EffectService effects_;
  InstrumentService instruments_;
  TrackDeviceService track_devices_;
  MidiClipService midi_clips_;
  NoteService notes_;
  PedalService pedals_;
  AudioClipService audio_clips_;
  WarpService warp_;
  ClipboardService clipboard_;
  ImportService imports_;
  ExportService exports_;
  FreezeService freeze_;
  std::uint64_t session_generation_;
  std::optional<foundation::Error> creation_warning_;
};

} // namespace oscillate::application
