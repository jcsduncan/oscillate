#include "oscillate/export/processed_clip.hpp"

#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/clay_source.hpp"
#include "oscillate/processing/warp_render_cache.hpp"

#include <unistd.h>
#include <utility>

namespace oscillate::exporting {

foundation::Result<std::optional<ProcessedExportClip>>
prepare_processed_export_clip(const project::Snapshot& snapshot,
                              const persistence::ProjectStore& store, const project::Clip& clip) {
  using Prepared = foundation::Result<std::optional<ProcessedExportClip>>;
  auto request = processing::processed_clip_request(snapshot, clip.id, warp_dsp::QualityTier::q0);
  if (!request)
    return Prepared::failure(request.error());
  if (!request.value())
    return Prepared::success(std::nullopt);
  const auto& resolved = *request.value();
  // A Clay take plays its render through the same warp (ADR-0081).
  if (resolved.clay) {
    auto rendered =
        processing::clay::open_rendered(store, resolved.clay->original, resolved.clay->take);
    if (!rendered)
      return Prepared::failure(rendered.error());
    auto reader = processing::OfflineWarpClip::open(rendered.value().fd(), resolved);
    if (!reader)
      return Prepared::failure(reader.error());
    const auto first = resolved.plan.project_start_frame;
    return Prepared::success(ProcessedExportClip{first, first + resolved.plan.warp.output_frames(),
                                                 std::move(reader).value()});
  }
  auto descriptor = store.open_managed_source(resolved.source);
  if (!descriptor)
    return Prepared::failure(descriptor.error());
  auto reader = processing::OfflineWarpClip::open(descriptor.value(), resolved);
  const int closed = ::close(descriptor.value());
  if (!reader)
    return Prepared::failure(reader.error());
  if (closed != 0)
    return Prepared::failure(
        {foundation::ErrorCode::io_failure, "Cannot close verified warp source descriptor"});
  const auto first = resolved.plan.project_start_frame;
  return Prepared::success(ProcessedExportClip{first, first + resolved.plan.warp.output_frames(),
                                               std::move(reader).value()});
}

} // namespace oscillate::exporting
