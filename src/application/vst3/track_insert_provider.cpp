#include "oscillate/application/vst3/track_insert_provider.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace oscillate::plugins {
namespace {
constexpr auto first_effect_instance_id = std::uint64_t{1} << 63;

[[nodiscard]] bool matches_fixed_tail_qualification(const Vst3FixedTailQualification& qualified,
                                                    const project::Vst3PluginInstance& saved,
                                                    const ScanResult& scanned,
                                                    std::uint32_t sample_rate,
                                                    std::uint32_t max_frames,
                                                    std::optional<std::uint32_t> declared_tail) {
  if (!qualified.tested_64_frames || !qualified.tested_128_frames ||
      (max_frames != 64 && max_frames != 128) || qualified.sample_rate != sample_rate ||
      !declared_tail || *declared_tail != qualified.declared_tail_frames ||
      qualified.binary_sha256.empty() || qualified.binary_byte_size == 0 ||
      qualified.binary_sha256 != scanned.binary.sha256 ||
      qualified.binary_sha256 != saved.binary_sha256 ||
      qualified.binary_byte_size != scanned.binary.byte_size ||
      qualified.class_id != saved.class_id || qualified.vendor != saved.vendor ||
      qualified.version != saved.version || qualified.component_state != saved.component_state ||
      qualified.controller_state != saved.controller_state ||
      qualified.parameters != saved.parameters)
    return false;
  return std::ranges::any_of(scanned.metadata.classes, [&](const Vst3Class& klass) {
    return klass.id == qualified.class_id && klass.vendor == qualified.vendor &&
           klass.version == qualified.version;
  });
}
} // namespace

Vst3TrackInsertProvider::Vst3TrackInsertProvider(Vst3ProcessingMode mode,
                                                 bool fixed_latency_verified)
    : mode_(mode), fixed_latency_verified_(fixed_latency_verified),
      next_instance_id_(first_effect_instance_id) {}

void Vst3TrackInsertProvider::set_scan_receipt(project::PluginInstanceId id, ScanResult scanned) {
  receipts_.insert_or_assign(id, std::move(scanned));
}

void Vst3TrackInsertProvider::set_manual_qualification(project::PluginInstanceId id,
                                                       bool fixed_latency_verified,
                                                       std::vector<std::uint32_t> parameter_ids) {
  std::ranges::sort(parameter_ids);
  auto unique = std::ranges::unique(parameter_ids);
  parameter_ids.erase(unique.begin(), unique.end());
  manual_qualifications_.insert_or_assign(
      id, ManualQualification{fixed_latency_verified, std::move(parameter_ids), std::nullopt});
}

void Vst3TrackInsertProvider::set_size_scoped_manual_qualification(
    project::PluginInstanceId id, bool fixed_latency_verified,
    std::vector<Vst3QualifiedManualBlock> qualified_blocks) {
  std::vector<std::uint32_t> ids;
  for (const auto& block : qualified_blocks)
    if (block.block_frames != 0)
      ids.insert(ids.end(), block.parameter_ids.begin(), block.parameter_ids.end());
  std::ranges::sort(ids);
  auto unique = std::ranges::unique(ids);
  ids.erase(unique.begin(), unique.end());
  manual_qualifications_.insert_or_assign(
      id, ManualQualification{fixed_latency_verified, std::move(ids), std::move(qualified_blocks)});
}

void Vst3TrackInsertProvider::set_fixed_tail_qualification(
    project::PluginInstanceId id, Vst3FixedTailQualification qualification) {
  fixed_tail_qualifications_.insert_or_assign(id, std::move(qualification));
}

std::optional<std::uint64_t>
Vst3TrackInsertProvider::assigned_instance_id(project::PluginInstanceId id) const noexcept {
  const auto found = instance_ids_.find(id);
  if (found == instance_ids_.end())
    return std::nullopt;
  return found->second;
}

foundation::Result<application::PreparedTrackInsert>
Vst3TrackInsertProvider::prepare(const project::Snapshot& candidate,
                                 const project::Vst3PluginInstance& saved,
                                 std::uint32_t max_frames) {
  using Result = foundation::Result<application::PreparedTrackInsert>;
  const auto found = receipts_.find(saved.id);
  if (found == receipts_.end()) {
    return Result::failure(
        {foundation::ErrorCode::unavailable,
         "No current VST3 scan receipt for saved insert " + saved.id.to_string()});
  }
  if (next_instance_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "VST3 graph instance ID space exhausted"});
  }
  const auto engine_instance_id = next_instance_id_;
  auto prepared_automation =
      automation_project::prepare_project_automation(candidate, candidate.revision);
  if (!prepared_automation)
    return Result::failure(prepared_automation.error());
  std::vector<automation_project::PreparedTargetAutomation> automation;
  std::uint64_t automation_key = 0;
  for (auto& lane : prepared_automation.value()) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
    if (!target || target->plugin != saved.id)
      continue;
    if (automation.empty())
      automation_key = 14695981039346656037ULL;
    auto part = lane.semantics_key();
    for (unsigned byte = 0; byte < 8; ++byte) {
      automation_key ^= part & 0xffU;
      automation_key *= 1099511628211ULL;
      part >>= 8U;
    }
    automation.push_back(std::move(lane));
  }
  if (!automation.empty() && automation_key == 0)
    automation_key = 1;
  try {
    const auto qualified = manual_qualifications_.find(saved.id);
    const std::vector<std::uint32_t> no_ids;
    const auto& ids =
        qualified == manual_qualifications_.end() ? no_ids : qualified->second.parameter_ids;
    const auto qualified_blocks = qualified == manual_qualifications_.end()
                                      ? std::optional<std::vector<Vst3QualifiedManualBlock>>{}
                                      : qualified->second.qualified_blocks;
    // The constructor flag exists only for an isolated synthetic fixture. In
    // a multi-effect project, or once any per-instance record is supplied,
    // it cannot silently qualify another saved effect.
    std::size_t enabled_effects = 0;
    if (fixed_latency_verified_ && manual_qualifications_.empty()) {
      for (const auto& track : candidate.tracks) {
        enabled_effects +=
            static_cast<std::size_t>(std::ranges::count_if(track.effects, [](const auto& slot) {
              return slot.enabled &&
                     std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
            }));
      }
    }
    const bool singleton_fixture =
        fixed_latency_verified_ && manual_qualifications_.empty() && enabled_effects == 1;
    const bool fixed_latency_verified = qualified == manual_qualifications_.end()
                                            ? singleton_fixture
                                            : qualified->second.fixed_latency_verified;
    const bool manual_control_qualified = fixed_latency_verified || !ids.empty();
    const bool has_automation = !automation.empty();
    const bool routed_sidechain = std::ranges::any_of(candidate.tracks, [&](const auto& track) {
      const auto effect = std::ranges::find_if(track.effects, [&](const auto& slot) {
        const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor);
        return slot.enabled && plugin && plugin->id == saved.id;
      });
      if (effect == track.effects.end())
        return false;
      return std::ranges::any_of(candidate.tracks, [&](const auto& source) {
        return std::ranges::any_of(source.sends, [&](const auto& send) {
          return send.purpose == project::SendPurpose::sidechain && send.destination == track.id &&
                 send.target_insert == effect->id;
        });
      });
    });
    auto node = prepare_vst3_graph_node(
        saved, found->second, engine_instance_id, candidate.tempo,
        static_cast<double>(candidate.sample_rate), max_frames, mode_, std::move(automation),
        automation_key, manual_control_qualified, ids, routed_sidechain, qualified_blocks);
    if (!node.ready()) {
      return Result::failure({foundation::ErrorCode::unavailable,
                              "VST3 insert " + saved.id.to_string() + ": " + node.diagnostic});
    }
    if (node.request.instance_id != engine_instance_id) {
      return Result::failure(
          {foundation::ErrorCode::conflict, "Verified VST3 graph ID changed during preparation"});
    }
    auto runtime = node.request.create_runtime();
    if (!runtime) {
      return Result::failure({foundation::ErrorCode::unavailable,
                              "Verified VST3 insert factory was already consumed"});
    }
    auto status = std::move(node.runtime_status);
    application::PreparedTrackInsert prepared{};
    prepared.processor = std::shared_ptr<engine::ProcessorRuntime>(std::move(runtime));
    prepared.latency_frames = node.latency_samples;
    prepared.routed_sidechain = node.routed_sidechain;
    prepared.process_failed = [status]() noexcept {
      return status->load(std::memory_order_acquire) == Vst3GraphRuntimeStatus::process_failed;
    };
    prepared.manual_events = std::move(node.manual_events);
    prepared.finite_tail_frames = node.finite_tail_samples;
    const auto fixed_tail = fixed_tail_qualifications_.find(saved.id);
    // A fixed-latency receipt can coexist with exact-state tail evidence only
    // when its size-scoped record admits no manual parameter. The unscoped
    // whole-binary qualification with empty IDs admits every continuous control,
    // so it cannot prove that the prepared tail remains valid after a drag.
    const bool no_live_manual_controls =
        ids.empty() &&
        (!fixed_latency_verified ||
         (qualified != manual_qualifications_.end() && qualified_blocks &&
          std::ranges::all_of(*qualified_blocks,
                              [](const auto& block) { return block.parameter_ids.empty(); }) &&
          std::ranges::any_of(*qualified_blocks,
                              [](const auto& block) { return block.block_frames == 64; }) &&
          std::ranges::any_of(*qualified_blocks,
                              [](const auto& block) { return block.block_frames == 128; })));
    prepared.fixed_tail_qualified =
        fixed_tail != fixed_tail_qualifications_.end() && !node.routed_sidechain &&
        !has_automation && no_live_manual_controls &&
        matches_fixed_tail_qualification(fixed_tail->second, saved, found->second,
                                         candidate.sample_rate, max_frames,
                                         node.finite_tail_samples);
    prepared.fixed_latency_qualified = manual_control_qualified;
    prepared.active_manual_graph = [slot = prepared.manual_events]() noexcept {
      return slot->active_graph();
    };
    // Commit the identity only after binding, factory use and the returned
    // runtime/probe are all ready. A failed attempt reserves nothing.
    ++next_instance_id_;
    instance_ids_.insert_or_assign(saved.id, engine_instance_id);
    return Result::success(std::move(prepared));
  } catch (const std::bad_alloc&) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "VST3 insert preparation ran out of memory"});
  }
}

} // namespace oscillate::plugins
