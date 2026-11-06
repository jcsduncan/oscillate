#include "oscillate/application/session/save_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

} // namespace

foundation::Result<void> SaveService::save() {
  if (state_.repair_state)
    return foundation::Result<void>::failure(repair_in_progress());
  const auto* proof =
      state_.pending_external_proof &&
              state_.pending_external_proof->revision() == snapshot().revision &&
              std::ranges::any_of(snapshot().sources,
                                  [&](const auto& source) {
                                    return source == state_.pending_external_proof->record();
                                  })
          ? &*state_.pending_external_proof
          : nullptr;
  // An external path change must never cause full media hashing on this
  // control/UI thread. Undo and redo can change a location too, so require an
  // exact worker proof for any location not already in CURRENT.
  for (const auto& source : snapshot().sources) {
    if (!source.external_link)
      continue;
    const bool saved = std::ranges::any_of(state_.saved_external_sources,
                                           [&](const auto& item) { return item == source; });
    if (!saved && (!proof || proof->record() != source)) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::conflict, "External link needs worker verification before save"});
    }
  }
  auto outcome = state_.store.save(
      snapshot(), persistence::SaveKind::explicit_save,
      std::span<const persistence::SourceReceipt>(state_.pending_receipts), proof);
  if (!outcome) {
    return foundation::Result<void>::failure(outcome.error());
  }
  if (outcome.value().status != persistence::SaveStatus::committed) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::io_failure, "Project save durability is uncertain"});
  }
  state_.save_maintenance_warning = outcome.value().maintenance_warning;
  state_.saved_revision = snapshot().revision;
  state_.recovered_autosave = false;
  state_.pending_receipts.clear();
  state_.saved_external_sources.clear();
  for (const auto& source : snapshot().sources)
    if (source.external_link)
      state_.saved_external_sources.push_back(source);
  state_.pending_external_proof.reset();
  return foundation::Result<void>::success();
}

foundation::Result<SaveService::BackgroundSaveTask> SaveService::begin_background_save() const {
  using Result = foundation::Result<BackgroundSaveTask>;
  if (!state_.model || !has_unsaved_changes())
    return Result::failure({foundation::ErrorCode::not_found, "No unsaved project revision"});
  if (state_.repair_state || !state_.pending_receipts.empty() || state_.pending_external_proof)
    return Result::failure(
        {foundation::ErrorCode::conflict, "Finish media publication before background save"});
  for (const auto& source : snapshot().sources) {
    if (source.external_link && std::ranges::find(state_.saved_external_sources, source) ==
                                    state_.saved_external_sources.end())
      return Result::failure(
          {foundation::ErrorCode::conflict, "External link needs worker verification before save"});
  }
  auto writer = state_.store.fork_explicit_save_writer();
  if (!writer)
    return Result::failure(writer.error());
  return Result::success(BackgroundSaveTask(model().snapshot(), std::move(writer.value())));
}

void SaveService::finish_background_save(std::uint64_t revision,
                                         const persistence::SaveOutcome& outcome) {
  if (outcome.status != persistence::SaveStatus::committed || !state_.model ||
      revision > snapshot().revision || revision < state_.saved_revision)
    return;
  state_.saved_revision = revision;
  state_.recovered_autosave = false;
  state_.save_maintenance_warning = outcome.maintenance_warning;
}

foundation::Result<SaveService::AutosaveTask> SaveService::begin_autosave() const {
  using Result = foundation::Result<AutosaveTask>;
  if (!state_.model || !has_unsaved_changes())
    return Result::failure({foundation::ErrorCode::not_found, "No unsaved project revision"});
  if (state_.repair_state || !state_.pending_receipts.empty())
    return Result::failure(
        {foundation::ErrorCode::conflict, "Finish media publication before autosaving"});
  auto writer = state_.store.fork_autosave_writer();
  if (!writer)
    return Result::failure(writer.error());
  return Result::success(AutosaveTask(model().snapshot(), std::move(writer.value())));
}

foundation::Result<persistence::SaveOutcome>
SaveService::create_named_revision(std::string_view name) {
  using Result = foundation::Result<persistence::SaveOutcome>;
  auto stopped = state_.gate->acquire_stopped();
  if (!stopped)
    return Result::failure(stopped.error());
  auto saved = save();
  if (!saved)
    return Result::failure(saved.error());
  return state_.store.name_current_revision(name);
}

foundation::Result<persistence::NamedRevisionListing> SaveService::list_named_revisions() const {
  if (state_.repair_state)
    return foundation::Result<persistence::NamedRevisionListing>::failure(repair_in_progress());
  return state_.store.list_named_revisions();
}

foundation::Result<void> SaveService::restore_named_revision(std::string_view name) {
  using Result = foundation::Result<void>;
  if (state_.repair_state)
    return Result::failure(repair_in_progress());
  auto stopped = state_.gate->acquire_stopped();
  if (!stopped)
    return Result::failure(stopped.error());
  if (!state_.pending_receipts.empty())
    return Result::failure(
        {foundation::ErrorCode::conflict, "Save pending managed media before restore"});
  auto current = state_.store.load_current();
  if (!current)
    return Result::failure(current.error());
  if (current.value().snapshot.revision != snapshot().revision)
    return Result::failure({foundation::ErrorCode::conflict,
                            "Save current project changes before restoring a named revision"});
  auto named = state_.store.load_named_revision(name);
  if (!named)
    return Result::failure(named.error());
  if (!named.value().source_diagnostics.empty()) {
    const auto& first = named.value().source_diagnostics.front();
    const char* reason =
        first.status == persistence::LoadedGeneration::SourceDiagnostic::Status::missing ? "missing"
        : first.status == persistence::LoadedGeneration::SourceDiagnostic::Status::damaged
            ? "damaged"
            : "unresolved";
    return Result::failure({foundation::ErrorCode::unavailable,
                            "Named revision has " +
                                std::to_string(named.value().source_diagnostics.size()) +
                                " unavailable media source(s); first: " + first.id.to_string() +
                                " (" + reason + "); repair or relink before restore"});
  }
  if (named.value().snapshot.sample_rate != snapshot().sample_rate)
    return Result::failure(
        {foundation::ErrorCode::unsupported,
         "Named revision uses a different sample rate; device renegotiation is not ready"});
  for (const auto& source : named.value().snapshot.sources) {
    if (source.external_link &&
        !std::ranges::any_of(state_.saved_external_sources, [&](const auto& current_source) {
          return current_source == source;
        })) {
      return Result::failure(
          {foundation::ErrorCode::conflict,
           "Named revision changes an external link; verify it on a worker before restore"});
    }
  }
  auto applied = model().restore_snapshot(std::move(named.value().snapshot));
  if (!applied)
    return Result::failure(applied.error());
  auto saved = save();
  if (!saved)
    return Result::failure(
        {saved.error().code, "Named revision restored in memory; save not confirmed: " +
                                 saved.error().message + "; choose Save or Undo"});
  return Result::success();
}

const std::optional<foundation::Error>& SaveService::save_maintenance_warning() const noexcept {
  return state_.save_maintenance_warning;
}

bool SaveService::has_unsaved_changes() const noexcept {
  return state_.model &&
         (state_.recovered_autosave || model().snapshot()->revision != state_.saved_revision);
}

std::uint64_t SaveService::persisted_revision() const noexcept { return state_.saved_revision; }

} // namespace oscillate::application
