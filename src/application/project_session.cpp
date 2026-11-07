#include "oscillate/application/project_session.hpp"

#include "session/session_state.hpp"

#include "oscillate/persistence/project_creation.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oscillate::application {
namespace {
std::atomic<std::uint64_t> next_session_generation{1};
} // namespace

ProjectSession::ProjectSession(std::filesystem::path root, persistence::ProjectStore store)
    : state_(std::make_unique<SessionState>(std::move(root), std::move(store))), saving_(*state_),
      history_(*state_), media_repair_(*state_), tracks_(*state_), routing_(*state_),
      automation_(*state_), parameter_automation_(*state_), song_(*state_, automation_),
      source_analysis_(*state_), effects_(*state_, saving_), instruments_(*state_, saving_),
      track_devices_(*state_), midi_clips_(*state_), notes_(*state_), pedals_(*state_),
      audio_clips_(*state_), warp_(*state_), clipboard_(*state_), imports_(*state_, saving_),
      exports_(*state_), freeze_(*state_, saving_),
      session_generation_(next_session_generation.fetch_add(1, std::memory_order_relaxed)) {}

ProjectSession::~ProjectSession() = default;

const project::Snapshot& ProjectSession::snapshot() const noexcept {
  return *state_->model->snapshot();
}

foundation::Result<std::unique_ptr<ProjectSession>>
ProjectSession::create(const std::filesystem::path& root) {
  return create(root, CreateOptions{});
}

foundation::Result<std::unique_ptr<ProjectSession>>
ProjectSession::create(const std::filesystem::path& root, CreateOptions options) {
  // The project is built and first saved in a hidden stage beside `root`, then
  // renamed into place, so a killed creation never leaves a half project there.
  auto started = persistence::ProjectCreation::begin(root);
  if (!started) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(started.error());
  }
  auto creation = std::move(started.value().creation);
  auto session =
      std::unique_ptr<ProjectSession>(new ProjectSession(root, std::move(started.value().writer)));
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  auto id = project::ProjectId::from_bits({0, 1});
  if (!tempo || !meter || !id) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(
        {foundation::ErrorCode::internal, "Cannot initialize project timing"});
  }
  auto model = project::ProjectModel::create(id.value(), 48'000, std::move(tempo.value()),
                                             std::move(meter.value()), *session->state_->gate);
  if (!model) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(model.error());
  }
  session->state_->model = std::make_unique<project::ProjectModel>(std::move(model.value()));
  if (options.default_returns) {
    auto seeded = session->tracks_.seed_default_returns();
    if (!seeded)
      return foundation::Result<std::unique_ptr<ProjectSession>>::failure(seeded.error());
  }
  auto saved = session->saving().save();
  if (!saved) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(saved.error());
  }
  auto published = creation.publish(session->state_->store);
  if (!published) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(published.error());
  }
  return foundation::Result<std::unique_ptr<ProjectSession>>::success(std::move(session));
}

foundation::Result<std::unique_ptr<ProjectSession>>
ProjectSession::open(const std::filesystem::path& root, persistence::FaultHook save_fault) {
  auto store = persistence::ProjectStore::open_writer(root, false, std::move(save_fault));
  if (!store) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(store.error());
  }
  auto session =
      std::unique_ptr<ProjectSession>(new ProjectSession(root, std::move(store.value())));
  auto loaded = session->state_->store.load_current();
  if (!loaded) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(loaded.error());
  }
  session->state_->saved_revision = loaded.value().snapshot.revision;
  for (const auto& source : loaded.value().snapshot.sources)
    if (source.external_link)
      session->state_->saved_external_sources.push_back(source);
  auto model =
      project::ProjectModel::open(std::move(loaded.value().snapshot), *session->state_->gate);
  if (!model) {
    return foundation::Result<std::unique_ptr<ProjectSession>>::failure(model.error());
  }
  session->state_->model = std::make_unique<project::ProjectModel>(std::move(model.value()));
  return foundation::Result<std::unique_ptr<ProjectSession>>::success(std::move(session));
}

foundation::Result<ProjectSession::RecoveryInspection>
ProjectSession::inspect_recovery(const std::filesystem::path& root) {
  auto reader = persistence::ProjectStore::open_reader(root);
  if (!reader)
    return foundation::Result<RecoveryInspection>::failure(reader.error());
  RecoveryInspection inspection;
  const auto append = [&inspection](const persistence::LoadedGeneration& loaded, bool pointed) {
    std::size_t clips = 0;
    for (const auto& track : loaded.snapshot.tracks)
      clips += track.clips.size();
    inspection.choices.push_back({loaded.generation, loaded.kind, loaded.snapshot.revision,
                                  loaded.snapshot.tracks.size(), clips, pointed});
  };
  auto current = reader.value().load_current();
  if (current) {
    inspection.has_current = true;
    append(current.value(), true);
  } else if (current.error().code != foundation::ErrorCode::not_found) {
    inspection.current_issue = current.error().message;
  }
  auto autosave = reader.value().load_autosave();
  if (autosave) {
    inspection.has_autosave = true;
    if (!current || current.value().generation != autosave.value().generation)
      append(autosave.value(), true);
  } else if (autosave.error().code != foundation::ErrorCode::not_found) {
    inspection.autosave_issue = autosave.error().message;
  }
  if (!current || (!autosave && inspection.autosave_issue)) {
    auto candidates = reader.value().recovery_candidates();
    if (!candidates)
      return foundation::Result<RecoveryInspection>::failure(candidates.error());
    for (const auto& candidate : candidates.value()) {
      const bool listed = std::ranges::any_of(inspection.choices, [&](const auto& choice) {
        return choice.generation == candidate.generation;
      });
      if (!listed && (!current || candidate.snapshot.id == current.value().snapshot.id) &&
          (!current || candidate.kind == persistence::SaveKind::autosave))
        append(candidate, false);
    }
  }
  return foundation::Result<RecoveryInspection>::success(std::move(inspection));
}

foundation::Result<std::unique_ptr<ProjectSession>>
ProjectSession::open_recovered(const std::filesystem::path& root, std::string_view generation,
                               persistence::SaveKind kind, bool expected_pointed) {
  using Result = foundation::Result<std::unique_ptr<ProjectSession>>;
  auto writer = persistence::ProjectStore::open_writer(root, false);
  if (!writer)
    return Result::failure(writer.error());
  auto session =
      std::unique_ptr<ProjectSession>(new ProjectSession(root, std::move(writer.value())));
  auto current = session->state_->store.load_current();
  if (kind == persistence::SaveKind::autosave && !current) {
    return Result::failure({foundation::ErrorCode::conflict,
                            "Repair a validated last-save generation before opening an autosave"});
  }
  auto selected = kind == persistence::SaveKind::explicit_save
                      ? session->state_->store.load_current()
                      : session->state_->store.load_autosave();
  if (expected_pointed && (!selected || selected.value().generation != generation))
    return Result::failure(
        {foundation::ErrorCode::conflict, "Recovery pointer changed after the choice was shown"});
  if (!expected_pointed && selected)
    return Result::failure({foundation::ErrorCode::conflict,
                            "Recovery pointer was repaired or changed after the choice was shown"});
  if (!expected_pointed) {
    auto candidates = session->state_->store.recovery_candidates();
    if (!candidates)
      return Result::failure(candidates.error());
    const auto candidate = std::ranges::find_if(candidates.value(), [&](const auto& item) {
      return item.generation == generation && item.kind == kind;
    });
    if (candidate == candidates.value().end())
      return Result::failure(
          {foundation::ErrorCode::not_found, "Selected recovery generation is no longer valid"});
    if (current && kind == persistence::SaveKind::autosave &&
        candidate->snapshot.revision < current.value().snapshot.revision)
      return Result::failure(
          {foundation::ErrorCode::conflict, "Autosave predates the last explicit save"});
    auto adopted = session->state_->store.adopt_generation(generation, kind);
    if (!adopted)
      return Result::failure(adopted.error());
    if (adopted.value().status != persistence::SaveStatus::committed)
      return Result::failure(
          {foundation::ErrorCode::io_failure,
           "Recovery pointer durability is uncertain; inspect project before retrying"});
    selected = kind == persistence::SaveKind::explicit_save
                   ? session->state_->store.load_current()
                   : session->state_->store.load_autosave();
  }
  if (!selected || selected.value().generation != generation)
    return Result::failure({foundation::ErrorCode::conflict,
                            "Selected recovery pointer changed before project opened"});
  if (current && current.value().snapshot.id != selected.value().snapshot.id)
    return Result::failure({foundation::ErrorCode::conflict, "Recovery project identity differs"});
  if (current && kind == persistence::SaveKind::autosave &&
      selected.value().snapshot.revision < current.value().snapshot.revision)
    return Result::failure(
        {foundation::ErrorCode::conflict, "Autosave predates the last explicit save"});
  session->state_->saved_revision = current && kind == persistence::SaveKind::autosave
                                        ? current.value().snapshot.revision
                                        : selected.value().snapshot.revision;
  session->state_->recovered_autosave = kind == persistence::SaveKind::autosave;
  const auto& saved_sources = current && kind == persistence::SaveKind::autosave
                                  ? current.value().snapshot.sources
                                  : selected.value().snapshot.sources;
  for (const auto& source : saved_sources)
    if (source.external_link)
      session->state_->saved_external_sources.push_back(source);
  auto model =
      project::ProjectModel::open(std::move(selected.value().snapshot), *session->state_->gate);
  if (!model)
    return Result::failure(model.error());
  session->state_->model = std::make_unique<project::ProjectModel>(std::move(model.value()));
  return Result::success(std::move(session));
}

foundation::Result<ControlTransportGate::PlaybackToken> ProjectSession::reserve_playback_start() {
  return state_->gate->try_start();
}

} // namespace oscillate::application
