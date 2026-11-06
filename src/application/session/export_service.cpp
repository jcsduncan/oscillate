#include "oscillate/application/session/export_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "session_state.hpp"

#include <utility>
#include <variant>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

} // namespace

foundation::Result<exporting::PublishOutcome> MasterExportTask::run() && {
  if (const auto* wav = std::get_if<exporting::MasterRenderOptions>(&options_)) {
    return exporting::render_wav_master(*snapshot_, reader_, std::move(authority_), *wav);
  }
  if (const auto* stems = std::get_if<exporting::StemRenderOptions>(&options_)) {
    return exporting::render_wav_stems(*snapshot_, reader_, std::move(authority_), *stems);
  }
  return exporting::render_encoded_master(*snapshot_, reader_, std::move(authority_),
                                          std::get<exporting::EncodedMasterOptions>(options_));
}

foundation::Result<MasterExportTask>
ExportService::prepare_wav_master_export(exporting::MasterRenderOptions options) {
  return prepare_master_export(std::move(options));
}

foundation::Result<MasterExportTask>
ExportService::prepare_encoded_master_export(exporting::EncodedMasterOptions options) {
  return prepare_master_export(std::move(options));
}

foundation::Result<MasterExportTask>
ExportService::prepare_wav_stem_export(exporting::StemRenderOptions options) {
  return prepare_master_export(std::move(options));
}

foundation::Result<StoppedSnapshotExportTask> ExportService::prepare_stopped_snapshot_export() {
  if (state_.repair_state)
    return foundation::Result<StoppedSnapshotExportTask>::failure(repair_in_progress());
  if (!state_.pending_receipts.empty()) {
    return foundation::Result<StoppedSnapshotExportTask>::failure(
        {foundation::ErrorCode::conflict, "Save the pending managed source before export"});
  }
  auto snapshot = model().snapshot();
  auto authority = exporting::StoppedExportAuthority::acquire(*snapshot, *state_.gate);
  if (!authority)
    return foundation::Result<StoppedSnapshotExportTask>::failure(authority.error());
  auto reader = persistence::ProjectStore::open_reader(state_.root);
  if (!reader)
    return foundation::Result<StoppedSnapshotExportTask>::failure(reader.error());
  return foundation::Result<StoppedSnapshotExportTask>::success(
      {std::move(snapshot), std::move(reader.value()), state_.gate, std::move(authority.value())});
}

foundation::Result<MasterExportTask> ExportService::prepare_master_export(
    std::variant<exporting::MasterRenderOptions, exporting::EncodedMasterOptions,
                 exporting::StemRenderOptions>
        options) {
  if (state_.repair_state)
    return foundation::Result<MasterExportTask>::failure(repair_in_progress());
  if (!state_.pending_receipts.empty()) {
    return foundation::Result<MasterExportTask>::failure(
        {foundation::ErrorCode::conflict, "Save the pending managed source before export"});
  }
  auto snapshot = model().snapshot();
  auto authority = exporting::StoppedExportAuthority::acquire(*snapshot, *state_.gate);
  if (!authority) {
    return foundation::Result<MasterExportTask>::failure(authority.error());
  }
  auto reader = persistence::ProjectStore::open_reader(state_.root);
  if (!reader) {
    return foundation::Result<MasterExportTask>::failure(reader.error());
  }
  return foundation::Result<MasterExportTask>::success(
      {std::move(snapshot), std::move(reader.value()), state_.gate, std::move(authority.value()),
       std::move(options)});
}

Result<PinnedSnapshot> ExportService::pin_snapshot() const {
  using R = Result<PinnedSnapshot>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  if (!state_.pending_receipts.empty())
    return R::failure({ErrorCode::conflict, "Save the pending managed source before checking"});
  auto reader = persistence::ProjectStore::open_reader(state_.root);
  if (!reader)
    return R::failure(reader.error());
  const auto folder =
      state_.root.has_filename() ? state_.root.filename() : state_.root.parent_path().filename();
  return R::success(PinnedSnapshot{model().snapshot(), std::move(reader).value(), folder.string()});
}

} // namespace oscillate::application
