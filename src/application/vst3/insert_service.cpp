#include "oscillate/application/vst3/insert_service.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace oscillate::plugins {
namespace {

[[nodiscard]] foundation::Result<project::PluginInstanceId> reject(std::string message) {
  return foundation::Result<project::PluginInstanceId>::failure(
      {foundation::ErrorCode::unavailable, std::move(message)});
}

[[nodiscard]] bool same_binary(const Vst3BinaryIdentity& left, const Vst3BinaryIdentity& right) {
  return left.canonical_bundle == right.canonical_bundle &&
         left.canonical_binary == right.canonical_binary && left.byte_size == right.byte_size &&
         left.sha256 == right.sha256;
}

// The class ID and VST3 subcategory are two distinct identity domains.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] const Vst3Class* unique_class(const Vst3Metadata& metadata, std::string_view class_id,
                                            std::string_view subcategory) {
  const Vst3Class* found = nullptr;
  for (const auto& candidate : metadata.classes) {
    if (candidate.id != class_id)
      continue;
    bool matching = false;
    std::string_view remaining = candidate.subcategories;
    while (!remaining.empty()) {
      const auto separator = remaining.find('|');
      if (remaining.substr(0, separator) == subcategory)
        matching = true;
      if (separator == std::string_view::npos)
        break;
      remaining.remove_prefix(separator + 1);
    }
    if (found != nullptr || candidate.category != "Audio Module Class" || !matching)
      return nullptr;
    found = &candidate;
  }
  return found;
}

using VerifiedClass = std::pair<Vst3Class, Vst3BinaryIdentity>;

[[nodiscard]] foundation::Result<VerifiedClass> verify_selection(const Vst3CatalogueEntry& selected,
                                                                 std::string_view class_id,
                                                                 std::string_view subcategory,
                                                                 Vst3Scanner& scanner) {
  const auto refuse = [](std::string message) {
    return foundation::Result<VerifiedClass>::failure(
        {foundation::ErrorCode::unavailable, std::move(message)});
  };
  if (!selected.scan.ok() || selected.canonical_bundle.empty() ||
      selected.scan.binary.canonical_bundle != selected.canonical_bundle)
    return refuse("Selected VST3 catalogue entry has no verified binary");
  const auto* advertised = unique_class(selected.scan.metadata, class_id, subcategory);
  if (advertised == nullptr)
    return refuse("Selected VST3 class is unavailable or ambiguous for this track kind");
  const auto fresh = scanner.scan(selected.canonical_bundle);
  if (!fresh.ok())
    return refuse("Selected VST3 binary is no longer available: " + fresh.diagnostic);
  if (!same_binary(selected.scan.binary, fresh.binary) ||
      fresh.binary.canonical_bundle != selected.canonical_bundle)
    return refuse("Selected VST3 binary changed since discovery");
  const auto* current = unique_class(fresh.metadata, class_id, subcategory);
  if (current == nullptr || current->category != advertised->category ||
      current->vendor != advertised->vendor || current->name != advertised->name ||
      current->version != advertised->version ||
      current->subcategories != advertised->subcategories)
    return refuse("Selected VST3 class identity changed since discovery");
  return foundation::Result<VerifiedClass>::success({*current, fresh.binary});
}

[[nodiscard]] foundation::Result<project::PluginInstanceId>
insert_verified_class(application::ProjectSession& session, const Vst3Class& current,
                      const Vst3BinaryIdentity& binary, project::TrackId track, std::size_t index) {
  return session.effects().insert_vst3_plugin(
      track, index,
      {.class_id = current.id,
       .vendor = current.vendor,
       .name = current.name,
       .version = current.version,
       .canonical_bundle_path = binary.canonical_bundle.string(),
       .binary_sha256 = binary.sha256});
}

} // namespace

foundation::Result<VerifiedVst3EffectSelection>
verify_vst3_effect_selection(const Vst3CatalogueEntry& selected, std::string_view class_id,
                             Vst3Scanner& scanner) {
  auto verified = verify_selection(selected, class_id, "Fx", scanner);
  if (!verified)
    return foundation::Result<VerifiedVst3EffectSelection>::failure(verified.error());
  return foundation::Result<VerifiedVst3EffectSelection>::success(VerifiedVst3EffectSelection(
      std::move(verified.value().first), std::move(verified.value().second)));
}

foundation::Result<VerifiedVst3InstrumentSelection>
verify_vst3_instrument_selection(const Vst3CatalogueEntry& selected, std::string_view class_id,
                                 Vst3Scanner& scanner) {
  auto verified = verify_selection(selected, class_id, "Instrument", scanner);
  if (!verified)
    return foundation::Result<VerifiedVst3InstrumentSelection>::failure(verified.error());
  return foundation::Result<VerifiedVst3InstrumentSelection>::success(
      VerifiedVst3InstrumentSelection(std::move(verified.value().first),
                                      std::move(verified.value().second)));
}

foundation::Result<project::PluginInstanceId>
insert_verified_vst3_effect(application::ProjectSession& session,
                            const VerifiedVst3EffectSelection& verified, project::TrackId track,
                            std::size_t index) {
  // Any track's chain takes a VST3 effect, anywhere in it (ADR-0050).
  if (std::ranges::find(session.snapshot().tracks, track, &project::Track::id) ==
      session.snapshot().tracks.end())
    return foundation::Result<project::PluginInstanceId>::failure(
        {foundation::ErrorCode::not_found, "VST3 effect track is missing"});
  const auto& current = verified.effect_class();
  const auto& binary = verified.binary();

  return insert_verified_class(session, current, binary, track, index);
}

foundation::Result<project::PluginInstanceId>
insert_verified_vst3_instrument(application::ProjectSession& session,
                                const VerifiedVst3InstrumentSelection& verified,
                                project::TrackId track) {
  const auto& snapshot = session.snapshot();
  const auto selected = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  // The instrument is the first stage of the track's chain; any other
  // tracks and chains play beside it (ADR-0050).
  if (selected == snapshot.tracks.end() || selected->kind != project::TrackKind::instrument ||
      !std::holds_alternative<std::monostate>(selected->instrument_source)) {
    return foundation::Result<project::PluginInstanceId>::failure(
        {foundation::ErrorCode::unsupported,
         "VST3 instrument requires an instrument track without a source"});
  }
  return insert_verified_class(session, verified.instrument_class(), verified.binary(), track, 0);
}

foundation::Result<project::TrackId>
create_verified_vst3_instrument_track(application::ProjectSession& session,
                                      const VerifiedVst3InstrumentSelection& verified) {
  const auto& current = verified.instrument_class();
  const auto& binary = verified.binary();
  return session.instruments().create_vst3_instrument_track(
      {.class_id = current.id,
       .vendor = current.vendor,
       .name = current.name,
       .version = current.version,
       .canonical_bundle_path = binary.canonical_bundle.string(),
       .binary_sha256 = binary.sha256});
}

foundation::Result<project::PluginInstanceId>
insert_verified_vst3_effect(application::ProjectSession& session,
                            const Vst3CatalogueEntry& selected, std::string_view class_id,
                            project::TrackId track, std::size_t index, Vst3Scanner& scanner) {
  auto verified = verify_vst3_effect_selection(selected, class_id, scanner);
  if (!verified)
    return reject(verified.error().message);
  return insert_verified_vst3_effect(session, verified.value(), track, index);
}

} // namespace oscillate::plugins
