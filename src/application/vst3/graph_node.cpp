#include "oscillate/application/vst3/graph_node.hpp"

#include "oscillate/application/vst3/project_binding.hpp"
#include "oscillate/application/vst3/tempo_plan.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::plugins {
namespace {

// A prepared project automation lane as a VST3 node's parameter curve.
class PreparedCurve final : public Vst3ParameterCurve {
public:
  explicit PreparedCurve(automation_project::PreparedTargetAutomation lane)
      : lane_(std::move(lane)) {}
  // The callback contract (preparation.hpp) rules out allocation, so the
  // evaluation cannot throw.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] bool evaluate_block(std::int64_t first,
                                    std::span<double> output) const noexcept override {
    return lane_.evaluate_block({first}, output);
  }

private:
  automation_project::PreparedTargetAutomation lane_;
};

class KeyBuilder {
public:
  void add(std::uint64_t value) noexcept {
    for (unsigned index = 0; index < 8; ++index) {
      value_ ^= value & 0xffU;
      value_ *= 1099511628211ULL;
      value >>= 8U;
    }
  }
  void add(std::string_view value) noexcept {
    add(value.size());
    for (const auto character : value) {
      value_ ^= static_cast<unsigned char>(character);
      value_ *= 1099511628211ULL;
    }
  }
  void add(std::span<const std::uint8_t> value) noexcept {
    add(value.size());
    for (const auto byte : value) {
      value_ ^= byte;
      value_ *= 1099511628211ULL;
    }
  }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_ == 0 ? 1 : value_; }

private:
  std::uint64_t value_{14695981039346656037ULL};
};

struct ProcessingFormat {
  double sample_rate;
  std::uint32_t max_block_frames;
  double tempo_bpm;
  std::uint64_t tempo_revision;
  Vst3ProcessingMode mode;
  bool routed_sidechain;
};

[[nodiscard]] std::uint64_t compatibility_key(const project::Vst3PluginInstance& saved,
                                              ProcessingFormat format,
                                              std::uint64_t automation_key) noexcept {
  KeyBuilder key;
  key.add(saved.class_id);
  key.add(saved.vendor);
  key.add(saved.name);
  key.add(saved.version);
  key.add(saved.binary_sha256);
  key.add(saved.component_state);
  key.add(static_cast<std::uint64_t>(saved.controller_state.has_value()));
  if (saved.controller_state)
    key.add(*saved.controller_state);
  for (const auto& parameter : saved.parameters) {
    key.add(parameter.id);
    key.add(std::bit_cast<std::uint64_t>(parameter.normalized_value));
  }
  key.add(std::bit_cast<std::uint64_t>(format.sample_rate));
  key.add(format.max_block_frames);
  key.add(std::bit_cast<std::uint64_t>(format.tempo_bpm));
  key.add(format.tempo_revision);
  key.add(static_cast<std::uint64_t>(format.mode));
  key.add(static_cast<std::uint64_t>(format.routed_sidechain));
  key.add(automation_key);
  return key.value();
}

} // namespace

PreparedVst3GraphNode
prepare_vst3_graph_node(const project::Vst3PluginInstance& saved, const ScanResult& scanned,
                        std::uint64_t engine_instance_id, const timeline::TempoMap& tempo,
                        double sample_rate, std::uint32_t max_block_frames, Vst3ProcessingMode mode,
                        std::vector<automation_project::PreparedTargetAutomation> automation,
                        std::uint64_t automation_key, bool fixed_latency_verified,
                        std::span<const std::uint32_t> qualified_manual_ids, bool routed_sidechain,
                        std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks) {
  PreparedVst3GraphNode result;
  if (!std::isfinite(sample_rate) || sample_rate < 8'000 || sample_rate > 384'000 ||
      sample_rate != std::floor(sample_rate)) {
    result.binding_status = Vst3BindingStatus::timing_unsupported;
    result.diagnostic = "VST3 effect requires a finite integral sample rate";
    return result;
  }
  auto tempo_inputs = detail::make_vst3_tempo(tempo, static_cast<std::uint32_t>(sample_rate));
  if (!tempo_inputs) {
    result.binding_status = Vst3BindingStatus::timing_unsupported;
    result.diagnostic = tempo_inputs.error().message;
    return result;
  }
  if (engine_instance_id == 0) {
    result.binding_status = Vst3BindingStatus::identity_mismatch;
    result.diagnostic = "Graph processor ID must be nonzero";
    return result;
  }
  // Size the host before loading so admission and preallocation agree.
  const auto event_budget = vst3_automation_event_budget(automation.size(), max_block_frames);
  if (!automation.empty() && (!event_budget || automation_key == 0)) {
    result.binding_status = Vst3BindingStatus::automation_unavailable;
    result.diagnostic = "VST3 automation exceeds the bounded event budget or lacks a key";
    return result;
  }
  // A zero block size has no budget; host creation reports that failure.
  const auto event_capacity = event_budget.value_or(1U);
  auto binding = bind_vst3_project_effect(saved, scanned, sample_rate, max_block_frames, mode,
                                          event_capacity, routed_sidechain);
  result.binding_status = binding.status;
  result.unresolved_parameter_ids = std::move(binding.unresolved_parameter_ids);
  result.diagnostic = std::move(binding.diagnostic);
  if (!binding.ready())
    return result;
  const auto latency_check = binding.effect->host().check_prepared_latency();
  if (latency_check.status != Vst3LatencyCheckStatus::stable) {
    result.binding_status = Vst3BindingStatus::timing_unsupported;
    result.diagnostic =
        "VST3 processor latency changed, requested a restart or could not be verified";
    return result;
  }
  result.routed_sidechain = binding.effect->host().routed_sidechain_enabled();
  for (const auto& lane : automation) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
    const auto parameter = target ? std::ranges::find(binding.effect->host().parameters(),
                                                      target->parameter_id, &Vst3ParameterInfo::id)
                                  : binding.effect->host().parameters().end();
    if (!target || target->plugin != saved.id ||
        parameter == binding.effect->host().parameters().end() || parameter->step_count != 0 ||
        binding.effect->host().parameter_precision(target->parameter_id) !=
            Vst3ParameterPrecision::sample_offsets_forwarded) {
      result.binding_status = Vst3BindingStatus::automation_unavailable;
      result.diagnostic = "Saved VST3 automation targets an unavailable or repeated parameter";
      return result;
    }
  }
  std::ranges::sort(automation, {}, [](const auto& lane) {
    return std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id;
  });
  for (std::size_t index = 1; index < automation.size(); ++index) {
    if (std::get<project::Vst3ParameterTarget>(automation[index - 1].target()).parameter_id ==
        std::get<project::Vst3ParameterTarget>(automation[index].target()).parameter_id) {
      result.binding_status = Vst3BindingStatus::automation_unavailable;
      result.diagnostic = "Saved VST3 automation repeats a parameter";
      return result;
    }
  }
  result.latency_samples = binding.effect->host().latency_samples();
  result.finite_tail_samples = binding.effect->host().finite_tail_samples();
  result.runtime_status =
      std::make_shared<std::atomic<Vst3GraphRuntimeStatus>>(Vst3GraphRuntimeStatus::ready);
  const auto key = compatibility_key(saved,
                                     {sample_rate, max_block_frames, tempo.events().front().bpm,
                                      tempo.revision(), mode, routed_sidechain},
                                     automation_key);
  std::vector<std::uint32_t> eligible;
  for (const auto& parameter : binding.effect->host().parameters()) {
    if (parameter.step_count == 0 && !parameter.read_only &&
        binding.effect->host().parameter_precision(parameter.id) ==
            Vst3ParameterPrecision::sample_offsets_forwarded)
      eligible.push_back(parameter.id);
  }
  std::ranges::sort(eligible);
  if (!qualified_manual_ids.empty()) {
    // Only controls actually exposed as continuous, writable and sample-offset
    // capable by this loaded instance can pass the caller's evidence record.
    std::erase_if(eligible, [qualified_manual_ids](std::uint32_t id) {
      return std::ranges::find(qualified_manual_ids, id) == qualified_manual_ids.end();
    });
  }
  if (qualified_blocks) {
    for (auto& block : *qualified_blocks) {
      std::ranges::sort(block.parameter_ids);
      auto unique = std::ranges::unique(block.parameter_ids);
      block.parameter_ids.erase(unique.begin(), unique.end());
      std::erase_if(block.parameter_ids, [&eligible](std::uint32_t id) {
        return !std::ranges::binary_search(eligible, id);
      });
    }
    std::erase_if(eligible, [&qualified_blocks](std::uint32_t id) {
      return !std::ranges::any_of(*qualified_blocks, [id](const auto& block) {
        return block.block_frames != 0 && std::ranges::binary_search(block.parameter_ids, id);
      });
    });
  }
  std::vector<std::uint32_t> automated;
  automated.reserve(automation.size());
  for (const auto& lane : automation)
    automated.push_back(std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id);
  result.manual_events = std::make_shared<Vst3ManualEventSlot>(
      Vst3InstanceKey{.instance_id = engine_instance_id, .compatibility_key = key},
      std::move(eligible), std::move(automated), fixed_latency_verified,
      std::move(qualified_blocks));
  std::vector<Vst3AutomationLane> lanes;
  lanes.reserve(automation.size());
  for (auto& lane : automation) {
    const auto id = std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id;
    lanes.push_back({id, std::make_shared<const PreparedCurve>(std::move(lane))});
  }
  result.request = make_vst3_graph_request(
      engine_instance_id, key,
      {std::move(binding.effect), result.runtime_status, max_block_frames,
       std::move(tempo_inputs).value(), static_cast<std::uint32_t>(sample_rate), std::move(lanes),
       event_capacity, result.manual_events, routed_sidechain});
  return result;
}

} // namespace oscillate::plugins
