#include "oscillate/application/role_listening.hpp"

#include "oscillate/analysis/mix_role.hpp"
#include "oscillate/media/audio_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <unistd.h>

namespace oscillate::application {

foundation::Result<std::vector<project::Operation>>
listen_for_roles(const persistence::ProjectStore& store, const project::Snapshot& snapshot,
                 const std::function<bool()>& cancelled, const std::stop_token& stop) {
  using R = foundation::Result<std::vector<project::Operation>>;
  std::vector<project::Operation> operations;
  for (const auto& track : snapshot.tracks) {
    if (track.kind != project::TrackKind::audio || track.frozen || track.clips.empty())
      continue;
    // The source the track plays most of.
    std::map<project::SourceId, std::uint64_t> played;
    for (const auto& clip : track.clips)
      played[clip.source] += static_cast<std::uint64_t>(
          std::max<std::int64_t>(0, clip.source_range.end.value - clip.source_range.start.value));
    const auto most = std::ranges::max_element(
        played, [](const auto& a, const auto& b) { return a.second < b.second; });
    const auto source =
        std::ranges::find(snapshot.sources, most->first, &project::SourceRecord::id);
    if (source == snapshot.sources.end())
      continue;
    auto descriptor = store.open_verified_source(*source, stop);
    if (!descriptor)
      return R::failure(descriptor.error());
    auto reader = media::AudioReader::open_fd(descriptor.value(), *source);
    (void)::close(descriptor.value());
    if (!reader)
      return R::failure(reader.error());
    auto heard = analysis::hear_role(
        reader.value(), [&] { return stop.stop_requested() || (cancelled && cancelled()); });
    if (!heard)
      return R::failure(heard.error());
    if (track.heard_role != heard.value().role)
      operations.emplace_back(project::SetTrackHeardRole{track.id, heard.value().role});
  }
  return R::success(std::move(operations));
}

} // namespace oscillate::application
