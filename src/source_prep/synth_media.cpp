#include <oscillate/source_prep/synth_media.hpp>

#include <oscillate/project/model.hpp>
#include <oscillate/source_prep/managed_audio.hpp>

#include <algorithm>
#include <memory>
#include <utility>
#include <variant>

namespace oscillate::source_prep {

foundation::Result<engine::SynthMedia>
load_synth_media(const persistence::ProjectStore& store, const project::Snapshot& snapshot,
                 const project::NativeSynthDocument& document, const std::stop_token& stop) {
  using R = foundation::Result<engine::SynthMedia>;
  engine::SynthMedia media;
  for (const auto& slot : document.media) {
    const auto* reference = std::get_if<project::ManagedAudioReference>(&slot.media);
    if (reference == nullptr)
      continue;
    // Every slot names a source of its own, even when its audio is shared.
    const auto record =
        std::ranges::find(snapshot.sources, reference->source, &project::SourceRecord::id);
    if (record == snapshot.sources.end() || record->sha256 != reference->sha256)
      return R::failure({foundation::ErrorCode::unavailable,
                         "Synth media slot '" + slot.id + "' has no matching source"});
    const auto shared =
        std::ranges::find(media, reference->sha256, &engine::SynthMediaEntry::sha256);
    if (shared != media.end()) {
      media.push_back({slot.id, reference->sha256, shared->asset});
      continue;
    }
    auto decoded = load_managed_audio(store, *record, project::max_native_synth_media_frames, stop);
    if (!decoded)
      return R::failure(decoded.error());
    auto asset = std::make_shared<engine::SynthMediaAsset>();
    asset->source_rate = decoded.value().source_rate;
    asset->frames = std::move(decoded.value().frames);
    media.push_back({slot.id, reference->sha256, std::move(asset)});
  }
  return R::success(std::move(media));
}

} // namespace oscillate::source_prep
