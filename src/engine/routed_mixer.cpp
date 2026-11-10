#include "oscillate/engine/routed_mixer.hpp"

#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oscillate::engine {
namespace {
using Result = foundation::Result<std::unique_ptr<RoutedMixer>>;

[[nodiscard]] Result fail(foundation::ErrorCode code, const char* message) {
  return Result::failure({code, message});
}

[[nodiscard]] bool fits_bytes(std::uint64_t count, std::uint64_t bytes_each,
                              std::uint64_t budget) noexcept {
  return bytes_each == 0 || count <= budget / bytes_each;
}

[[nodiscard]] StereoFrame finite(StereoFrame frame) noexcept {
  return {std::isfinite(frame.left) ? frame.left : 0.0F,
          std::isfinite(frame.right) ? frame.right : 0.0F};
}

// Whether any of a node's predecessors is still ringing out.
template <typename Drains> [[nodiscard]] bool draining(const Drains& drains) noexcept {
  return std::ranges::any_of(drains, [](const auto& drain) {
    return drain->remaining_frames.load(std::memory_order_relaxed) != 0;
  });
}

} // namespace

RoutedMixer::RoutedMixer(RoutingLatencyPlan plan, RoutedMixerLimits limits,
                         std::vector<std::shared_ptr<ProcessorRuntime>> processors,
                         std::vector<std::uint32_t> processor_latencies,
                         std::vector<TailDrains> tails, std::vector<bool> muted,
                         std::vector<StereoFrame> output_gains, std::vector<GainRamp> gain_ramps,
                         std::vector<std::vector<StereoFrame>> buffers,
                         std::vector<std::unique_ptr<DelayCompensationRuntime>> source_input_delays,
                         std::vector<std::vector<StereoFrame>> sidechain_buffers,
                         std::vector<SidechainProcessorRuntime*> sidechain_processors,
                         std::vector<NodeFrameHistory> source_histories,
                         std::vector<NodeFrameHistory> processor_input_histories,
                         std::vector<StereoFrame> scratch, std::vector<EdgeRuntime> edges,
                         std::vector<std::vector<std::size_t>> incoming) noexcept
    : plan_(std::move(plan)), limits_(limits), processors_(std::move(processors)),
      processor_latencies_(std::move(processor_latencies)), tails_(std::move(tails)),
      muted_(std::move(muted)), output_gains_(std::move(output_gains)),
      gain_ramps_(std::move(gain_ramps)), buffers_(std::move(buffers)),
      source_input_delays_(std::move(source_input_delays)),
      sidechain_buffers_(std::move(sidechain_buffers)),
      sidechain_processors_(std::move(sidechain_processors)),
      source_histories_(std::move(source_histories)),
      processor_input_histories_(std::move(processor_input_histories)),
      scratch_(std::move(scratch)), edges_(std::move(edges)), incoming_(std::move(incoming)) {}

// `live`: the prior mixer is still rendering. Only its configuration is
// read here; its running state (delay lines, histories, ramps) is taken over
// by adopt_live_state() on the callback at the switching block.
Result RoutedMixer::prepare_impl(std::span<const RoutedNode> nodes,
                                 std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
                                 const RoutedMixer* prior, BlockContext switch_boundary,
                                 bool live) {
  if (limits.max_frames == 0 || limits.max_frames > 8192) {
    return fail(foundation::ErrorCode::invalid_argument, "mixer block limit must be 1..8192");
  }
  if (limits.gain_ramp_frames > 8192) {
    return fail(foundation::ErrorCode::invalid_argument,
                "mixer gain ramp limit must be at most 8192 samples");
  }
  std::unordered_set<RoutingNodeId> sidechain_destinations;
  std::unordered_set<RoutingNodeId> sidechain_sources;
  try {
    for (const auto& edge : edges) {
      if (edge.kind == RoutingEdgeKind::sidechain) {
        sidechain_destinations.insert(edge.destination);
        sidechain_sources.insert(edge.source);
      }
    }
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "sidechain route preparation ran out of memory");
  }
  // A sidechain source also keeps its pre-fader block for its key edges.
  const auto buffer_count = static_cast<std::uint64_t>(nodes.size()) + 1 +
                            static_cast<std::uint64_t>(sidechain_destinations.size()) +
                            static_cast<std::uint64_t>(sidechain_sources.size());
  const auto bytes_per_buffer = static_cast<std::uint64_t>(limits.max_frames) * sizeof(StereoFrame);
  if (!fits_bytes(buffer_count, bytes_per_buffer, limits.max_buffer_bytes)) {
    return fail(foundation::ErrorCode::resource_exhausted, "routing buffers exceed byte budget");
  }
  const auto bytes_per_history =
      (static_cast<std::uint64_t>(limits.latency.max_compensation_frames) + 1) *
      sizeof(StereoFrame);
  const auto input_history_bytes =
      limits.max_processor_input_history_frames == 0
          ? 0ULL
          : (static_cast<std::uint64_t>(limits.max_processor_input_history_frames) + 1) *
                sizeof(StereoFrame);
  const auto route_history_count = edges.size() + sidechain_destinations.size();
  if (!fits_bytes(route_history_count, bytes_per_history, limits.max_history_bytes) ||
      !fits_bytes(nodes.size(), input_history_bytes,
                  limits.max_history_bytes - route_history_count * bytes_per_history)) {
    return fail(foundation::ErrorCode::resource_exhausted, "route histories exceed byte budget");
  }
  // This optional second budget includes the ring's extra slot and every
  // planned node, including the master. No callback allocation is needed.
  if (limits.max_source_history_bytes != 0 &&
      !fits_bytes(nodes.size(), bytes_per_history, limits.max_source_history_bytes)) {
    return fail(foundation::ErrorCode::resource_exhausted, "source histories exceed byte budget");
  }

  std::vector<RoutingNode> routing_nodes;
  routing_nodes.reserve(nodes.size());
  for (const auto& node : nodes) {
    routing_nodes.push_back(node.routing);
  }
  auto planned = prepare_routing_latency(routing_nodes, edges, limits.latency);
  if (!planned) {
    return Result::failure(planned.error());
  }
  auto plan = std::move(planned).value();

  if (prior != nullptr && !live &&
      (switch_boundary.transport_generation == 0 ||
       prior->generation_ != switch_boundary.transport_generation ||
       prior->next_position_ != switch_boundary.project_sample_position)) {
    return fail(foundation::ErrorCode::unavailable,
                "prior mixer is not quiescent at the requested boundary");
  }

  try {
    std::unordered_map<RoutingNodeId, std::size_t> slots;
    slots.reserve(nodes.size());
    for (std::size_t index = 0; index < plan.topological_order.size(); ++index) {
      slots.emplace(plan.topological_order[index], index);
    }
    std::unordered_map<RoutingNodeId, std::shared_ptr<ProcessorRuntime>> owners;
    std::unordered_map<RoutingNodeId, std::optional<RoutedTailReplacement>> replacements;
    std::unordered_map<RoutingNodeId, std::shared_ptr<ProcessorRuntime>> state_handoffs;
    std::unordered_map<RoutingNodeId, std::uint32_t> latencies;
    std::unordered_map<RoutingNodeId, bool> muted_by_node;
    std::unordered_map<RoutingNodeId, StereoFrame> gain_by_node;
    std::unordered_map<RoutingNodeId, std::shared_ptr<ProcessorRuntime>> output_stages;
    output_stages.reserve(nodes.size());
    owners.reserve(nodes.size());
    replacements.reserve(nodes.size());
    state_handoffs.reserve(nodes.size());
    latencies.reserve(nodes.size());
    muted_by_node.reserve(nodes.size());
    gain_by_node.reserve(nodes.size());
    std::unordered_set<ProcessorRuntime*> active_owners;
    active_owners.reserve(nodes.size());
    for (const auto& node : nodes) {
      if (node.processor && !active_owners.insert(node.processor.get()).second)
        return fail(foundation::ErrorCode::conflict,
                    "route processors require exclusive runtime ownership");
      if (node.output_stage && !active_owners.insert(node.output_stage.get()).second)
        return fail(foundation::ErrorCode::conflict,
                    "route output stages require exclusive runtime ownership");
      owners.emplace(node.routing.id, node.processor);
      output_stages.emplace(node.routing.id, node.output_stage);
      replacements.emplace(node.routing.id, node.replacement);
      state_handoffs.emplace(node.routing.id, node.state_handoff_prior);
      latencies.emplace(node.routing.id, node.routing.processor_latency_frames);
      muted_by_node.emplace(node.routing.id, node.muted);
      if (!std::isfinite(node.output_gain.left) || node.output_gain.left < 0.0F ||
          node.output_gain.left > 4.0F || !std::isfinite(node.output_gain.right) ||
          node.output_gain.right < 0.0F || node.output_gain.right > 4.0F)
        return fail(foundation::ErrorCode::invalid_argument, "route output gain is invalid");
      gain_by_node.emplace(node.routing.id, node.output_gain);
    }
    // A live switch drops a node only once it has faded to silence
    // (ready_to_continue), its drains with it.
    if (prior != nullptr && !live) {
      for (std::size_t index = 0; index < prior->plan_.nodes.size(); ++index) {
        if (!owners.contains(prior->plan_.nodes[index].id) && draining(prior->tails_[index]))
          return fail(foundation::ErrorCode::conflict,
                      "route removal would truncate an active processor tail");
      }
    }
    std::vector<std::shared_ptr<ProcessorRuntime>> processors;
    std::vector<std::uint32_t> processor_latencies;
    std::vector<RoutedMixer::TailDrains> tails;
    std::vector<bool> muted;
    std::vector<StereoFrame> output_gains;
    std::vector<RoutedMixer::GainRamp> gain_ramps;
    std::vector<std::vector<StereoFrame>> buffers;
    std::vector<std::unique_ptr<DelayCompensationRuntime>> source_input_delays;
    std::vector<std::vector<StereoFrame>> sidechain_buffers;
    std::vector<SidechainProcessorRuntime*> sidechain_processors;
    std::vector<RoutedMixer::NodeFrameHistory> source_histories;
    std::vector<RoutedMixer::NodeFrameHistory> processor_input_histories;
    processors.reserve(nodes.size());
    processor_latencies.reserve(nodes.size());
    tails.reserve(nodes.size());
    muted.reserve(nodes.size());
    output_gains.reserve(nodes.size());
    gain_ramps.reserve(nodes.size());
    buffers.reserve(nodes.size());
    source_input_delays.reserve(nodes.size());
    sidechain_buffers.reserve(nodes.size());
    sidechain_processors.reserve(nodes.size());
    source_histories.reserve(nodes.size());
    processor_input_histories.reserve(nodes.size());
    std::unordered_set<ProcessorRuntime*> draining_owners;
    draining_owners.reserve(nodes.size());
    for (const auto id : plan.topological_order) {
      const auto& next_owner = owners.at(id);
      const auto& replacement = replacements.at(id);
      const auto& handoff = state_handoffs.at(id);
      RoutedMixer::TailDrains node_tails;
      const PlannedNodeLatency* old = nullptr;
      if (prior != nullptr) {
        const auto found = std::ranges::find(prior->plan_.nodes, id, &PlannedNodeLatency::id);
        if (found != prior->plan_.nodes.end())
          old = &*found;
      }
      if (old == nullptr) {
        if (replacement || handoff)
          return fail(foundation::ErrorCode::conflict,
                      "new route cannot declare a predecessor tail");
      } else {
        const auto old_index = static_cast<std::size_t>(old - prior->plan_.nodes.data());
        const auto& old_owner = prior->processors_[old_index];
        const auto& old_tails = prior->tails_[old_index];
        // Predecessors still ringing out keep draining across any switch.
        for (const auto& drain : old_tails) {
          if (drain->remaining_frames.load(std::memory_order_relaxed) != 0)
            node_tails.push_back(drain);
        }
        if (next_owner == old_owner) {
          if (replacement || handoff)
            return fail(foundation::ErrorCode::conflict,
                        "unchanged route processor cannot request replacement");
        } else if (!old_owner) {
          if (replacement || handoff ||
              (!live && latencies.at(id) != prior->processor_latencies_[old_index]))
            return fail(foundation::ErrorCode::conflict,
                        "new route processor cannot inherit an incompatible predecessor");
        } else {
          // A quiescent predecessor: the successor owns its retained stage
          // state and must never process it through a drain as well.
          const bool quiescent_handoff =
              handoff && !replacement && handoff == old_owner && next_owner &&
              (live || latencies.at(id) == prior->processor_latencies_[old_index]);
          // The successor takes over what the predecessor held at the switch,
          // or the path takes it back dry (ADR-0049); the predecessor needs no
          // drain.
          const bool latency_taken_over =
              !quiescent_handoff && live && !replacement && !handoff &&
              (next_owner ? next_owner->continues_latency_of(old_owner.get())
                          : old_owner->releasable_latency() != 0);
          if (!quiescent_handoff && !latency_taken_over &&
              (!replacement || handoff || replacement->prior_owner != old_owner ||
               (replacement->drain_processor && replacement->drain_processor == next_owner) ||
               replacement->drain_frames == 0 ||
               replacement->drain_frames > routed_tail_frame_cap ||
               (!live && latencies.at(id) != prior->processor_latencies_[old_index]))) {
            return fail(foundation::ErrorCode::conflict,
                        "route processor replacement lacks a bounded, compatible tail handoff");
          }
          if (replacement)
            node_tails.push_back(std::make_shared<RoutedMixer::TailDrain>(
                old_owner, replacement->drain_processor ? replacement->drain_processor : old_owner,
                replacement->drain_frames, live ? 0 : switch_boundary.state_generation(),
                replacement->fade_out));
        }
      }
      if (node_tails.size() > RoutedMixer::max_tail_drains_per_node)
        return fail(foundation::ErrorCode::resource_exhausted,
                    "too many predecessors still ringing out on one route; wait for their tails");
      for (const auto& drain : node_tails) {
        // A drain runs its processor on zero input: no live owner may run it.
        if (std::ranges::any_of(nodes,
                                [&](const RoutedNode& node) {
                                  return node.processor &&
                                         node.processor->contains(drain->drain_processor.get());
                                }) ||
            active_owners.contains(drain->prior_owner.get()) ||
            !draining_owners.insert(drain->prior_owner.get()).second ||
            (drain->drain_processor != drain->prior_owner &&
             !draining_owners.insert(drain->drain_processor.get()).second))
          return fail(foundation::ErrorCode::conflict,
                      "draining route processor is owned by another live path");
      }
      processors.push_back(next_owner);
      auto* sidechain_processor = dynamic_cast<SidechainProcessorRuntime*>(next_owner.get());
      if (sidechain_destinations.contains(id) && sidechain_processor == nullptr)
        return fail(foundation::ErrorCode::unsupported,
                    "sidechain destination processor does not support a second input");
      sidechain_processors.push_back(sidechain_processor);
      processor_latencies.push_back(latencies.at(id));
      tails.push_back(std::move(node_tails));
      muted.push_back(muted_by_node.at(id));
      const auto target_gain = gain_by_node.at(id);
      RoutedMixer::GainRamp ramp;
      if (old != nullptr && !live) {
        const auto old_index = static_cast<std::size_t>(old - prior->plan_.nodes.data());
        const auto& old_gain = prior->output_gains_[old_index];
        const auto& old_ramp = prior->gain_ramps_[old_index];
        if (target_gain.left == old_gain.left && target_gain.right == old_gain.right) {
          ramp = old_ramp;
        } else if (limits.gain_ramp_frames != 0) {
          ramp.from = old_gain;
          if (old_ramp.total != 0 && old_ramp.elapsed < old_ramp.total) {
            const auto fraction =
                static_cast<float>(old_ramp.elapsed) / static_cast<float>(old_ramp.total);
            ramp.from = {old_ramp.from.left + (old_gain.left - old_ramp.from.left) * fraction,
                         old_ramp.from.right + (old_gain.right - old_ramp.from.right) * fraction};
          }
          ramp.total = limits.gain_ramp_frames;
        }
      }
      output_gains.push_back(target_gain);
      gain_ramps.push_back(ramp);
      buffers.emplace_back(limits.max_frames);
      std::unique_ptr<DelayCompensationRuntime> source_input_delay;
      if (sidechain_destinations.contains(id)) {
        const auto node_plan = std::ranges::find(plan.nodes, id, &PlannedNodeLatency::id);
        const auto own_latency = latencies.at(id);
        if (node_plan == plan.nodes.end() || node_plan->output_latency_frames < own_latency)
          return fail(foundation::ErrorCode::corrupt_data,
                      "sidechain destination has inconsistent source latency");
        const auto delay = node_plan->output_latency_frames - own_latency;
        if (delay > limits.latency.max_compensation_frames)
          return fail(foundation::ErrorCode::resource_exhausted,
                      "sidechain source input exceeds compensation budget");
        const auto config =
            DelayCompensationConfig{id, static_cast<std::uint32_t>(delay),
                                    limits.latency.max_compensation_frames, bytes_per_history};
        if (old != nullptr && !live &&
            prior
                ->source_input_delays_[static_cast<std::size_t>(old - prior->plan_.nodes.data())]) {
          auto prepared = DelayCompensationRuntime::prepare_transition_after_quiesce(
              *prior->source_input_delays_[static_cast<std::size_t>(old -
                                                                    prior->plan_.nodes.data())],
              config, switch_boundary);
          if (!prepared)
            return Result::failure(prepared.error());
          source_input_delay = std::move(prepared).value();
        } else {
          if (prior != nullptr && !live && delay != 0)
            return fail(foundation::ErrorCode::unavailable,
                        "new delayed sidechain source lacks prior input history");
          auto prepared = DelayCompensationRuntime::prepare(config);
          if (!prepared)
            return Result::failure(prepared.error());
          source_input_delay = std::move(prepared).value();
        }
      }
      source_input_delays.push_back(std::move(source_input_delay));
      if (sidechain_destinations.contains(id))
        sidechain_buffers.emplace_back(limits.max_frames);
      else
        sidechain_buffers.emplace_back();
      RoutedMixer::NodeFrameHistory history;
      if (limits.max_source_history_bytes != 0) {
        history.frames.resize(static_cast<std::size_t>(limits.latency.max_compensation_frames) + 1);
        if (old != nullptr && !live) {
          const auto old_index = static_cast<std::size_t>(old - prior->plan_.nodes.data());
          const auto& previous = prior->source_histories_[old_index];
          if (!previous.frames.empty() &&
              previous.generation == switch_boundary.state_generation() &&
              previous.next_position == switch_boundary.project_sample_position) {
            const auto count = std::min(previous.recorded, history.frames.size());
            for (std::size_t frame = 0; frame < count; ++frame) {
              history.frames[frame] =
                  previous.frames[(previous.cursor + previous.frames.size() - count + frame) %
                                  previous.frames.size()];
            }
            history.cursor = count % history.frames.size();
            history.recorded = count;
            history.generation = previous.generation;
            history.next_position = previous.next_position;
          }
        }
      }
      source_histories.push_back(std::move(history));
      RoutedMixer::NodeFrameHistory input_history;
      if (limits.max_processor_input_history_frames != 0) {
        input_history.frames.resize(
            static_cast<std::size_t>(limits.max_processor_input_history_frames) + 1);
        if (old != nullptr && !live) {
          const auto old_index = static_cast<std::size_t>(old - prior->plan_.nodes.data());
          const auto& previous = prior->processor_input_histories_[old_index];
          if (!previous.frames.empty() &&
              previous.generation == switch_boundary.state_generation() &&
              previous.next_position == switch_boundary.project_sample_position) {
            const auto count = std::min(previous.recorded, input_history.frames.size());
            for (std::size_t frame = 0; frame < count; ++frame)
              input_history.frames[frame] =
                  previous.frames[(previous.cursor + previous.frames.size() - count + frame) %
                                  previous.frames.size()];
            input_history.cursor = count % input_history.frames.size();
            input_history.recorded = count;
            input_history.generation = previous.generation;
            input_history.next_position = previous.next_position;
          }
        }
      }
      processor_input_histories.push_back(std::move(input_history));
    }
    std::vector<StereoFrame> scratch(limits.max_frames);
    std::vector<RoutedMixer::EdgeRuntime> runtime_edges;
    std::vector<std::vector<std::size_t>> incoming(nodes.size());
    runtime_edges.reserve(plan.edges.size());
    for (const auto& edge : plan.edges) {
      auto config =
          DelayCompensationConfig{edge.id, edge.compensation_frames,
                                  limits.latency.max_compensation_frames, bytes_per_history};
      foundation::Result<std::unique_ptr<DelayCompensationRuntime>> delay =
          DelayCompensationRuntime::prepare(config);
      if (!delay) {
        return Result::failure(delay.error());
      }
      if (prior != nullptr) {
        const auto old = std::ranges::find(prior->plan_.edges, edge.id, &PlannedEdgeDelay::id);
        if (old != prior->plan_.edges.end() &&
            (old->source != edge.source || old->destination != edge.destination ||
             old->kind != edge.kind)) {
          return fail(foundation::ErrorCode::conflict,
                      "route ID cannot be reused for different endpoints or input kinds");
        }
        if (live) {
          // adopt_live_state() continues or primes this line at the switch.
        } else if (edge.compensation_frames != 0 && old != prior->plan_.edges.end()) {
          const auto old_index = static_cast<std::size_t>(old - prior->plan_.edges.begin());
          delay = DelayCompensationRuntime::prepare_transition_after_quiesce(
              *prior->edges_[old_index].delay, config, switch_boundary);
        } else if (edge.compensation_frames != 0) {
          const auto& history = source_histories[slots.at(edge.source)];
          if (history.frames.empty() || history.recorded < edge.compensation_frames ||
              history.generation != switch_boundary.state_generation() ||
              history.next_position != switch_boundary.project_sample_position ||
              history.recorded > switch_boundary.project_sample_position) {
            return fail(foundation::ErrorCode::unavailable,
                        "new compensated route lacks current source history; hold old graph");
          }
          // Replay the chronological source samples ending at the held
          // boundary. The full retained ring also supports a rapid later
          // latency increase without losing pre-switch samples.
          std::size_t replayed = 0;
          const auto start = switch_boundary.project_sample_position - history.recorded;
          while (replayed < history.recorded) {
            const auto count = std::min(limits.max_frames, history.recorded - replayed);
            for (std::size_t frame = 0; frame < count; ++frame) {
              scratch[frame] = history.frames[(history.cursor + history.frames.size() -
                                               history.recorded + replayed + frame) %
                                              history.frames.size()];
            }
            delay.value()->process(std::span(scratch).first(count),
                                   {start + replayed, switch_boundary.transport_generation,
                                    switch_boundary.state_generation()});
            replayed += count;
          }
        }
      }
      if (!delay) {
        return Result::failure(delay.error());
      }
      const auto destination = slots.at(edge.destination);
      incoming[destination].push_back(runtime_edges.size());
      runtime_edges.push_back({slots.at(edge.source), destination, edge.gain, edge.kind,
                               std::move(delay).value(), edge.gain, 0, 0});
    }
    auto mixer = std::unique_ptr<RoutedMixer>(new RoutedMixer(
        std::move(plan), limits, std::move(processors), std::move(processor_latencies),
        std::move(tails), std::move(muted), std::move(output_gains), std::move(gain_ramps),
        std::move(buffers), std::move(source_input_delays), std::move(sidechain_buffers),
        std::move(sidechain_processors), std::move(source_histories),
        std::move(processor_input_histories), std::move(scratch), std::move(runtime_edges),
        std::move(incoming)));
    mixer->output_stages_.reserve(mixer->plan_.topological_order.size());
    for (const auto id : mixer->plan_.topological_order)
      mixer->output_stages_.push_back(output_stages.at(id));
    mixer->build_levels();
    if (live) {
      mixer->live_from_ = prior;
      mixer->live_nodes_.reserve(mixer->plan_.nodes.size());
      for (const auto& node : mixer->plan_.nodes) {
        const auto old = std::ranges::find(prior->plan_.nodes, node.id, &PlannedNodeLatency::id);
        mixer->live_nodes_.push_back(
            old == prior->plan_.nodes.end()
                ? RoutedMixer::no_prior
                : static_cast<std::size_t>(old - prior->plan_.nodes.begin()));
      }
      for (std::size_t index = 0; index < prior->plan_.nodes.size(); ++index) {
        if (!owners.contains(prior->plan_.nodes[index].id))
          mixer->live_removed_.push_back(index);
      }
      mixer->live_faded_.reserve(mixer->live_removed_.size());
      mixer->live_edges_.reserve(mixer->plan_.edges.size());
      for (const auto& edge : mixer->plan_.edges) {
        const auto old = std::ranges::find(prior->plan_.edges, edge.id, &PlannedEdgeDelay::id);
        mixer->live_edges_.push_back(
            old == prior->plan_.edges.end()
                ? RoutedMixer::no_prior
                : static_cast<std::size_t>(old - prior->plan_.edges.begin()));
      }
      mixer->plan_latency_handover(*prior);
    }
    if (limits.node_meters)
      mixer->node_meters_ = std::make_unique<RoutedNodeMeters>(mixer->buffers_.size());
    return Result::success(std::move(mixer));
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "routing mixer preparation ran out of memory");
  }
}

Result prepare_routed_mixer(std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
                            RoutedMixerLimits limits, const RoutedMixer* prior,
                            BlockContext switch_boundary) {
  return RoutedMixer::prepare_impl(nodes, edges, limits, prior, switch_boundary, false);
}

Result prepare_routed_mixer_live(std::span<const RoutedNode> nodes,
                                 std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
                                 const RoutedMixer& prior) {
  return RoutedMixer::prepare_impl(nodes, edges, limits, &prior, {}, true);
}

void RoutedMixer::graph_activated(std::uint64_t revision) noexcept {
  // A new graph is audible from this block on.
  foundation::trace::instant("edit.audible", revision);
  for (const auto& processor : processors_) {
    if (processor)
      processor->graph_activated(revision);
  }
}

void RoutedMixer::graph_deactivated(std::uint64_t revision) noexcept {
  for (const auto& processor : processors_) {
    if (processor)
      processor->graph_deactivated(revision);
  }
}

void RoutedMixer::control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept {
  for (const auto& processor : processors_) {
    if (processor)
      processor->control_overlay_rebound(from, to);
  }
}

bool RoutedMixer::accepts_context(BlockContext context, std::size_t frames) const noexcept {
  return frames <= 8192 && frames <= limits_.max_frames && context.transport_generation != 0 &&
         frames <= std::numeric_limits<std::uint64_t>::max() - context.project_sample_position &&
         (generation_ == 0 || context.transport_generation >= generation_) &&
         (generation_ != context.transport_generation ||
          context.project_sample_position == next_position_);
}

bool RoutedMixer::control_overlay_safe() const noexcept {
  for (std::size_t index = 0; index < processors_.size(); ++index) {
    if ((processors_[index] && !processors_[index]->control_overlay_revision_independent()) ||
        draining(tails_[index]))
      return false;
  }
  return true;
}

void RoutedMixer::ramp_node(std::size_t index, bool muted, StereoFrame gain) noexcept {
  if (muted == muted_[index] && gain.left == output_gains_[index].left &&
      gain.right == output_gains_[index].right)
    return;
  auto& ramp = gain_ramps_[index];
  const auto prior_target = muted_[index] ? StereoFrame{} : output_gains_[index];
  auto from = prior_target;
  if (ramp.elapsed < ramp.total) {
    const auto fraction = static_cast<float>(ramp.elapsed) / static_cast<float>(ramp.total);
    from = {ramp.from.left + (prior_target.left - ramp.from.left) * fraction,
            ramp.from.right + (prior_target.right - ramp.from.right) * fraction};
  }
  ramp.from = from;
  ramp.elapsed = 0;
  ramp.total = std::max<std::uint32_t>(64, limits_.gain_ramp_frames);
  muted_[index] = muted;
  output_gains_[index] = gain;
}

void RoutedMixer::apply_node_controls(std::span<const RoutedNodeControl> controls) noexcept {
  // The publisher validates count, IDs and finite ranges before publication.
  // This method runs exactly once on the callback after source preflight.
  for (std::size_t index = 0; index < controls.size(); ++index)
    ramp_node(index, controls[index].muted, controls[index].output_gain);
}

bool RoutedMixer::apply_live_node_gain(RoutingNodeId node, StereoFrame gain) noexcept {
  const auto found = std::ranges::find(plan_.nodes, node, &PlannedNodeLatency::id);
  if (found == plan_.nodes.end())
    return false;
  const auto index = static_cast<std::size_t>(found - plan_.nodes.begin());
  ramp_node(index, muted_[index], gain);
  return true;
}

std::optional<RoutedNodeMeterReading> RoutedMixer::node_meter(RoutingNodeId node) const noexcept {
  if (!node_meters_)
    return std::nullopt;
  const auto found = std::ranges::find(plan_.topological_order, node);
  if (found == plan_.topological_order.end())
    return std::nullopt;
  return node_meters_->latest(static_cast<std::size_t>(found - plan_.topological_order.begin()));
}

std::span<const StereoFrame> RoutedMixer::node_output(std::size_t index,
                                                      std::size_t frames) const noexcept {
  if (index >= buffers_.size() || frames > limits_.max_frames)
    return {};
  return std::span<const StereoFrame>(buffers_[index]).first(frames);
}

void RoutedMixer::ramp_edge(std::size_t index, float gain) noexcept {
  auto& edge = edges_[index];
  if (edge.gain == gain)
    return;
  auto from = edge.gain;
  if (edge.ramp_elapsed < edge.ramp_total) {
    const auto fraction =
        static_cast<float>(edge.ramp_elapsed) / static_cast<float>(edge.ramp_total);
    from = edge.ramp_from + (edge.gain - edge.ramp_from) * fraction;
  }
  edge.ramp_from = from;
  edge.ramp_elapsed = 0;
  edge.ramp_total = std::max<std::uint32_t>(64, limits_.gain_ramp_frames);
  edge.gain = gain;
}

void RoutedMixer::apply_edge_controls(std::span<const RoutedEdgeControl> controls) noexcept {
  // The publisher validates complete edge identity and finite physical gains.
  for (std::size_t index = 0; index < controls.size(); ++index)
    ramp_edge(index, controls[index].gain);
}

bool RoutedMixer::apply_live_edge_gain(RoutingEdgeId edge, float gain) noexcept {
  const auto found = std::ranges::find(plan_.edges, edge, &PlannedEdgeDelay::id);
  if (found == plan_.edges.end())
    return false;
  ramp_edge(static_cast<std::size_t>(found - plan_.edges.begin()), gain);
  return true;
}

bool RoutedMixer::admits_block(std::span<const std::span<const StereoFrame>> sources,
                               std::size_t frames, BlockContext context,
                               std::span<const EdgeGainSpan> gains,
                               std::span<const NodeGainSpan> node_gains) const noexcept {
  if (!accepts_context(context, frames) || sources.size() != buffers_.size())
    return false;
  for (const auto source : sources) {
    if (!source.empty() && source.size() != frames)
      return false;
  }
  if (!gains.empty()) {
    if (gains.size() != edges_.size())
      return false;
    for (std::size_t edge = 0; edge < gains.size(); ++edge) {
      const auto values = gains[edge].values;
      if (gains[edge].id != plan_.edges[edge].id || (!values.empty() && values.size() != frames) ||
          std::ranges::any_of(values, [](float value) {
            return !std::isfinite(value) || value < 0.0F || value > 4.0F;
          }))
        return false;
    }
  }
  if (!node_gains.empty()) {
    if (node_gains.size() != output_gains_.size())
      return false;
    for (std::size_t node = 0; node < node_gains.size(); ++node) {
      const auto values = node_gains[node].values;
      if (node_gains[node].id != plan_.nodes[node].id ||
          (!values.empty() && values.size() != frames) ||
          std::ranges::any_of(values, [](StereoFrame value) {
            return !std::isfinite(value.left) || value.left < 0.0F || value.left > 4.0F ||
                   !std::isfinite(value.right) || value.right < 0.0F || value.right > 4.0F;
          }))
        return false;
    }
  }
  return true;
}

bool RoutedMixer::mix_block(std::span<const std::span<const StereoFrame>> sources,
                            std::span<StereoFrame> output, BlockContext context,
                            std::span<const EdgeGainSpan> gains,
                            std::span<const NodeGainSpan> node_gains,
                            AudioWorkerPool* workers) noexcept {
  if (output.size() > 8192) {
    return false;
  }
  if (!admits_block(sources, output.size(), context, gains, node_gains)) {
    std::ranges::fill(output, StereoFrame{});
    return false;
  }
  if (output.empty()) {
    return true;
  }
  generation_ = context.transport_generation;
  next_position_ = context.project_sample_position + output.size();
  const NodeBlock block{sources, output.size(), context, gains, node_gains};
  if (workers == nullptr || workers->attached_workers() == 0) {
    for (std::size_t index = 0; index < buffers_.size(); ++index)
      mix_node(index, block, std::span(scratch_).first(2 * limits_.max_frames));
  } else {
    // Every node of a level depends only on earlier levels, so a level's
    // nodes run in parallel and the next level starts after they finish.
    struct LevelJob {
      RoutedMixer& mixer;
      const NodeBlock& block;
      std::span<const std::size_t> nodes;
    };
    for (std::size_t level = 0; level + 1 < level_offsets_.size(); ++level) {
      LevelJob job{
          *this, block,
          std::span(level_nodes_)
              .subspan(level_offsets_[level], level_offsets_[level + 1] - level_offsets_[level])};
      workers->run(
          job.nodes.size(),
          // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AudioWorkerPool::Task.
          [](void* context, std::size_t task, std::size_t lane) noexcept {
            auto& level_job = *static_cast<LevelJob*>(context);
            auto& mixer = level_job.mixer;
            mixer.mix_node(
                level_job.nodes[task], level_job.block,
                std::span(mixer.scratch_)
                    .subspan(lane * 2 * mixer.limits_.max_frames, 2 * mixer.limits_.max_frames));
          },
          &job);
    }
  }
  std::ranges::copy(std::span(buffers_.back()).first(output.size()), output.begin());
  return true;
}

void RoutedMixer::mix_node(std::size_t index, const NodeBlock& block,
                           std::span<StereoFrame> scratch) noexcept {
  OSCILLATE_TRACE_SPAN("graph.node", plan_.nodes[index].id);
  auto frames = std::span(node_buffer(index, block.slot)).first(block.frames);
  auto sidechain = std::span(sidechain_buffers_[index]);
  if (!sidechain.empty())
    std::ranges::fill(sidechain.first(block.frames), StereoFrame{});
  if (limits_.skip_muted_nodes && muted_[index] && prefader_buffers_[index].empty() &&
      gain_ramps_[index].elapsed >= gain_ramps_[index].total && tails_[index].empty() &&
      source_histories_[index].frames.empty()) {
    std::ranges::fill(frames, StereoFrame{});
    if (node_meters_)
      node_meters_->observe(index, frames, block.context);
    return;
  }
  if (block.sources[index].empty()) {
    std::ranges::fill(frames, StereoFrame{});
  } else {
    for (std::size_t frame = 0; frame < block.frames; ++frame) {
      frames[frame] = finite(block.sources[index][frame]);
    }
  }
  if (source_input_delays_[index])
    source_input_delays_[index]->process(frames, block.context);
  for (const auto edge_index : incoming_[index]) {
    auto& edge = edges_[edge_index];
    auto delayed = std::span(scratch).first(block.frames);
    // A key is taken after the source's inserts, before its fader and mute;
    // its edge gain carries whether the key sounds (routing silences a muted
    // source's key there).
    const auto& from = edge.kind == RoutingEdgeKind::sidechain
                           ? prefader_buffer(edge.source_index, block.slot)
                           : node_buffer(edge.source_index, block.slot);
    std::ranges::copy(std::span(from).first(block.frames), delayed.begin());
    edge.delay->process(delayed, block.context);
    const auto dynamic =
        block.gains.empty() ? std::span<const float>{} : block.gains[edge_index].values;
    auto destination =
        edge.kind == RoutingEdgeKind::sidechain ? sidechain.first(block.frames) : frames;
    for (std::size_t frame = 0; frame < block.frames; ++frame) {
      float gain = dynamic.empty() ? edge.gain : dynamic[frame];
      if (dynamic.empty() && edge.ramp_elapsed < edge.ramp_total) {
        const auto fraction =
            static_cast<float>(++edge.ramp_elapsed) / static_cast<float>(edge.ramp_total);
        gain = edge.ramp_from + (edge.gain - edge.ramp_from) * fraction;
      }
      destination[frame] = finite({destination[frame].left + delayed[frame].left * gain,
                                   destination[frame].right + delayed[frame].right * gain});
    }
  }
  auto& input_history = processor_input_histories_[index];
  if (!input_history.frames.empty()) {
    if (input_history.generation != block.context.state_generation()) {
      input_history.cursor = 0;
      input_history.recorded = 0;
      input_history.generation = block.context.state_generation();
    }
    for (const auto frame : frames) {
      input_history.frames[input_history.cursor] = frame;
      input_history.cursor =
          input_history.cursor + 1 == input_history.frames.size() ? 0 : input_history.cursor + 1;
      input_history.recorded = std::min(input_history.recorded + 1, input_history.frames.size());
    }
    input_history.next_position = next_position_;
  }
  // Every predecessor still ringing out drains on zero input; their outputs
  // sum into the first half of the lane, the second half takes each in turn.
  std::size_t tail_count = 0;
  auto drained = scratch.subspan(limits_.max_frames, limits_.max_frames);
  for (const auto& drain : tails_[index]) {
    auto remaining = drain->remaining_frames.load(std::memory_order_relaxed);
    if (remaining == 0)
      continue;
    if (drain->processing_generation != block.context.state_generation()) {
      drain->remaining_frames.store(0, std::memory_order_relaxed); // A seek retires it.
      continue;
    }
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block.frames));
    auto output = drained.first(count);
    std::ranges::fill(output, StereoFrame{});
    drain->drain_processor->process(output, block.context);
    drain->remaining_frames.store(remaining - count, std::memory_order_relaxed);
    if (drain->fade_out) {
      for (std::size_t frame = 0; frame < count; ++frame) {
        const auto left = remaining - frame;
        if (left < drain_fade_frames) {
          const auto gain = static_cast<float>(left) / static_cast<float>(drain_fade_frames);
          output[frame] = {output[frame].left * gain, output[frame].right * gain};
        }
      }
    }
    if (count > tail_count) {
      std::ranges::fill(scratch.subspan(tail_count, count - tail_count), StereoFrame{});
      tail_count = count;
    }
    for (std::size_t frame = 0; frame < count; ++frame)
      scratch[frame] = {scratch[frame].left + output[frame].left,
                        scratch[frame].right + output[frame].right};
  }
  if (!sidechain.empty()) {
    sidechain_processors_[index]->process_with_sidechain(frames, sidechain.first(block.frames),
                                                         block.context);
  } else if (processors_[index]) {
    processors_[index]->process(frames, block.context);
  }
  for (std::size_t frame = 0; frame < frames.size(); ++frame) {
    if (frame < tail_count)
      frames[frame] = finite(
          {frames[frame].left + scratch[frame].left, frames[frame].right + scratch[frame].right});
    else
      frames[frame] = finite(frames[frame]);
  }
  if (!prefader_buffers_[index].empty())
    std::ranges::copy(frames, prefader_buffer(index, block.slot).begin());
  auto& ramp = gain_ramps_[index];
  if (muted_[index] && ramp.elapsed >= ramp.total) {
    std::ranges::fill(frames, StereoFrame{});
  } else {
    const auto gain = muted_[index] ? StereoFrame{} : output_gains_[index];
    const auto dynamic =
        block.node_gains.empty() ? std::span<const StereoFrame>{} : block.node_gains[index].values;
    if (!dynamic.empty()) {
      // Dynamic gains replace the static output gain, not a mute: a mute or
      // unmute fade still runs on top of them, and the ramp advances.
      const bool fading_in = !muted_[index] && ramp.from.left == 0.0F && ramp.from.right == 0.0F;
      for (std::size_t frame = 0; frame < frames.size(); ++frame) {
        float envelope = muted_[index] ? 0.0F : 1.0F; // Silent once a mute fade ends.
        if (ramp.elapsed < ramp.total) {
          const auto fraction = static_cast<float>(++ramp.elapsed) / static_cast<float>(ramp.total);
          envelope = muted_[index] ? 1.0F - fraction : fading_in ? fraction : 1.0F;
        }
        frames[frame] = finite({frames[frame].left * dynamic[frame].left * envelope,
                                frames[frame].right * dynamic[frame].right * envelope});
      }
    } else if (ramp.elapsed < ramp.total) {
      for (auto& frame : frames) {
        if (ramp.elapsed < ramp.total) {
          const auto fraction = static_cast<float>(++ramp.elapsed) / static_cast<float>(ramp.total);
          const auto left = ramp.from.left + (gain.left - ramp.from.left) * fraction;
          const auto right = ramp.from.right + (gain.right - ramp.from.right) * fraction;
          frame = finite({frame.left * left, frame.right * right});
        } else {
          frame = finite({frame.left * gain.left, frame.right * gain.right});
        }
      }
    } else if (gain.left != 1.0F || gain.right != 1.0F) {
      for (auto& frame : frames)
        frame = finite({frame.left * gain.left, frame.right * gain.right});
    }
  }
  if (output_stages_[index])
    output_stages_[index]->process(frames, block.context);
  if (node_meters_)
    node_meters_->observe(index, frames, block.context);
  auto& history = source_histories_[index];
  if (!history.frames.empty()) {
    if (history.generation != block.context.state_generation()) {
      history.cursor = 0;
      history.recorded = 0;
      history.generation = block.context.state_generation();
    }
    for (const auto frame : frames) {
      history.frames[history.cursor] = frame;
      history.cursor = history.cursor + 1 == history.frames.size() ? 0 : history.cursor + 1;
      history.recorded = std::min(history.recorded + 1, history.frames.size());
    }
    history.next_position = next_position_;
  }
}

namespace {

// Continue a node history's recorded frames, oldest first, in `next`.
void continue_history(const auto& prior, auto& next) noexcept {
  if (prior.frames.empty() || next.frames.empty())
    return;
  const auto count = std::min(prior.recorded, next.frames.size());
  for (std::size_t frame = 0; frame < count; ++frame)
    next.frames[frame] =
        prior.frames[(prior.cursor + prior.frames.size() - count + frame) % prior.frames.size()];
  next.cursor = count % next.frames.size();
  next.recorded = count;
  next.generation = prior.generation;
  next.next_position = prior.next_position;
}

// The same time shift for a node's own output history.
void shift_history(auto& history, std::int64_t shift,
                   std::span<const StereoFrame> appended) noexcept {
  if (history.frames.empty())
    return;
  const auto size = history.frames.size();
  if (shift > 0) {
    const auto dropped = std::min<std::size_t>(static_cast<std::size_t>(shift), history.recorded);
    history.cursor = (history.cursor + size - dropped) % size;
    history.recorded -= dropped;
    return;
  }
  for (const auto frame : appended) {
    history.frames[history.cursor] = frame;
    history.cursor = history.cursor + 1 == size ? 0 : history.cursor + 1;
    history.recorded = std::min(history.recorded + 1, size);
  }
}

} // namespace

// A live switch keeps time exactly when every latency change is one node's,
// its processor can continue the old one (or the old one can release what it
// held, when the node loses its processor), the output latency is unchanged
// and only that node's outgoing delays change, by the negative of its own
// change (ADR-0049). Otherwise each changed delay crossfades on its own.
void RoutedMixer::plan_latency_handover(const RoutedMixer& prior) {
  live_shifts_.assign(live_nodes_.size(), 0);
  live_continues_.assign(live_nodes_.size(), false);
  bool exact = plan_.master_output_latency_frames == prior.plan_.master_output_latency_frames;
  std::size_t flush_frames = 0;
  for (std::size_t index = 0; index < live_nodes_.size(); ++index) {
    const auto old = live_nodes_[index];
    if (old == no_prior)
      continue;
    const auto next_latency = static_cast<std::int64_t>(processor_latencies_[index]);
    const auto old_latency = static_cast<std::int64_t>(prior.processor_latencies_[old]);
    if (static_cast<bool>(source_input_delays_[index]) !=
            static_cast<bool>(prior.source_input_delays_[old]) ||
        (source_input_delays_[index] && source_input_delays_[index]->delay_frames() !=
                                            prior.source_input_delays_[old]->delay_frames()))
      exact = false;
    // A changed processor always continues its predecessor as far as it
    // can, so its fresh stages never start empty (ADR-0050).
    const auto* old_owner = prior.processors_[old].get();
    live_continues_[index] = processors_[index].get() != old_owner &&
                             (processors_[index] != nullptr || old_owner != nullptr);
    if (next_latency == old_latency)
      continue;
    const bool continues =
        processors_[index]
            ? processors_[index]->continues_latency_of(old_owner) &&
                  (old_owner != nullptr ||
                   limits_.max_processor_input_history_frames >= processor_latencies_[index])
            : old_owner != nullptr && old_owner->releasable_latency() == old_latency;
    exact = exact && continues && sidechain_buffers_[index].empty();
    live_shifts_[index] = next_latency - old_latency;
    if (next_latency < old_latency)
      flush_frames = std::max(flush_frames, static_cast<std::size_t>(old_latency - next_latency));
  }
  for (std::size_t index = 0; index < live_edges_.size() && exact; ++index) {
    const auto old = live_edges_[index];
    if (old == no_prior)
      continue;
    const auto shift = live_shifts_[edges_[index].source_index];
    exact = static_cast<std::int64_t>(plan_.edges[index].compensation_frames) ==
            static_cast<std::int64_t>(prior.plan_.edges[old].compensation_frames) - shift;
  }
  // Without exact time, delays crossfade on their own and nothing shifts.
  if (!exact)
    std::ranges::fill(live_shifts_, 0);
  if (flush_frames != 0)
    live_flush_.assign(flush_frames, StereoFrame{});
}

bool RoutedMixer::ready_to_continue(const RoutedMixer& prior) const noexcept {
  return std::ranges::all_of(live_removed_, [&prior](std::size_t index) {
    const auto& ramp = prior.gain_ramps_[index];
    return prior.muted_[index] && ramp.elapsed >= ramp.total;
  });
}

void RoutedMixer::fade_out_dropped(RoutedMixer& prior) noexcept {
  for (const auto index : live_removed_) {
    if (prior.muted_[index] || live_faded_.size() == live_faded_.capacity())
      continue;
    prior.ramp_node(index, true, prior.output_gains_[index]);
    live_faded_.push_back(index);
  }
}

void RoutedMixer::restore_dropped(RoutedMixer& prior) noexcept {
  for (const auto index : live_faded_)
    prior.ramp_node(index, false, prior.output_gains_[index]);
  live_faded_.clear();
}

bool RoutedMixer::adopt_live_state(const RoutedMixer& prior, BlockContext context) noexcept {
  if (live_from_ != &prior)
    return false;
  live_from_ = nullptr;
  // A seek or new generation at the switch starts this graph afresh, as the
  // prior graph itself would have; a planned loop wrap keeps its history.
  const bool continuous =
      prior.generation_ != 0 && ((prior.generation_ == context.transport_generation &&
                                  prior.next_position_ == context.project_sample_position) ||
                                 context.planned_loop_wrap);
  for (std::size_t index = 0; index < live_nodes_.size(); ++index) {
    // Drains this switch started begin in the switching block's generation.
    for (const auto& drain : tails_[index]) {
      if (drain->processing_generation == 0)
        drain->processing_generation = continuous ? context.state_generation() : 0;
    }
    const auto old = live_nodes_[index];
    if (!continuous)
      continue;
    if (old == no_prior) {
      // A new node (an added track) fades in, as an unmute does.
      if (limits_.gain_ramp_frames != 0)
        gain_ramps_[index] = {{0.0F, 0.0F}, 0, limits_.gain_ramp_frames};
      continue;
    }
    const auto& old_gain = prior.output_gains_[old];
    const auto& old_ramp = prior.gain_ramps_[old];
    auto& ramp = gain_ramps_[index];
    const auto target = output_gains_[index];
    if (target.left == old_gain.left && target.right == old_gain.right) {
      ramp = old_ramp;
    } else if (limits_.gain_ramp_frames != 0) {
      ramp = {};
      ramp.from = old_gain;
      if (old_ramp.total != 0 && old_ramp.elapsed < old_ramp.total) {
        const auto fraction =
            static_cast<float>(old_ramp.elapsed) / static_cast<float>(old_ramp.total);
        ramp.from = {old_ramp.from.left + (old_gain.left - old_ramp.from.left) * fraction,
                     old_ramp.from.right + (old_gain.right - old_ramp.from.right) * fraction};
      }
      ramp.total = limits_.gain_ramp_frames;
    }
    continue_history(prior.source_histories_[old], source_histories_[index]);
    continue_history(prior.processor_input_histories_[old], processor_input_histories_[index]);
    if (source_input_delays_[index] && prior.source_input_delays_[old])
      source_input_delays_[index]->adopt(*prior.source_input_delays_[old]);
  }
  if (continuous) {
    for (std::size_t index = 0; index < edges_.size(); ++index) {
      auto& edge = edges_[index];
      const auto old = live_edges_[index];
      if (old != no_prior) {
        const auto& before = prior.edges_[old];
        if (plan_.edges[index].compensation_frames != 0)
          edge.delay->adopt(*before.delay);
        // Continue the prior edge's gain and any glide in progress; with
        // ramps on, glide from where it is to this graph's gain (as nodes do).
        const auto target = edge.gain;
        if (target == before.gain || limits_.gain_ramp_frames != 0) {
          edge.gain = before.gain;
          edge.ramp_from = before.ramp_from;
          edge.ramp_elapsed = before.ramp_elapsed;
          edge.ramp_total = before.ramp_total;
          ramp_edge(index, target);
        }
      } else if (plan_.edges[index].compensation_frames != 0) {
        // A new compensated route starts from its source's recent output.
        const auto& history = source_histories_[edge.source_index];
        if (!history.frames.empty() && history.recorded != 0) {
          const auto size = history.frames.size();
          const auto first = (history.cursor + size - history.recorded) % size;
          const auto older = std::min(history.recorded, size - first);
          edge.delay->prime(std::span(history.frames).subspan(first, older),
                            std::span(history.frames).first(history.recorded - older), context);
        }
      }
    }
    // A node whose processor changed continues its predecessor: its fresh
    // stages primed, or flushing what a removed stage still held. When its
    // latency changed with exact time, its delays shift by the change;
    // otherwise flushed frames are dropped and its delays crossfade.
    for (std::size_t index = 0; index < live_continues_.size(); ++index) {
      if (!live_continues_[index])
        continue;
      const auto& input = processor_input_histories_[index];
      std::span<const StereoFrame> older;
      std::span<const StereoFrame> newer;
      if (!input.frames.empty() && input.recorded != 0) {
        const auto size = input.frames.size();
        const auto first = (input.cursor + size - input.recorded) % size;
        const auto count = std::min(input.recorded, size - first);
        older = std::span(input.frames).subspan(first, count);
        newer = std::span(input.frames).first(input.recorded - count);
      }
      auto* old_owner = prior.processors_[live_nodes_[index]].get();
      auto flushed =
          std::span(live_flush_)
              .first(processors_[index] ? processors_[index]->continue_latency_of(
                                              old_owner, older, newer, context, live_flush_)
                                        : old_owner->release_latency(live_flush_));
      const auto shift = live_shifts_[index];
      if (shift == 0)
        continue;
      // The flushed frames leave the node through its level, as they would have.
      const auto gain = muted_[index] ? StereoFrame{} : output_gains_[index];
      for (auto& frame : flushed)
        frame = finite({frame.left * gain.left, frame.right * gain.right});
      shift_history(source_histories_[index], shift, flushed);
      for (auto& edge : edges_) {
        if (edge.source_index == index)
          edge.delay->shift(shift, flushed);
      }
    }
    generation_ = prior.generation_;
    next_position_ = prior.next_position_;
  }
  return true;
}

std::optional<std::size_t> RoutedMixer::prepare_pipeline() {
  const bool live_state =
      std::ranges::any_of(tails_, [](const auto& drains) { return !drains.empty(); }) ||
      std::ranges::any_of(source_histories_,
                          [](const auto& history) { return !history.frames.empty(); }) ||
      std::ranges::any_of(processor_input_histories_,
                          [](const auto& history) { return !history.frames.empty(); });
  if (live_state || buffers_.empty())
    return std::nullopt;
  const auto depth = level_offsets_.size() - 1;
  const auto nodes = buffers_.size();
  slot_buffers_.assign((depth - 1) * nodes, std::vector<StereoFrame>(limits_.max_frames));
  slot_prefader_buffers_.assign((depth - 1) * nodes, {});
  for (std::size_t slot = 1; slot < depth; ++slot)
    for (std::size_t index = 0; index < nodes; ++index)
      if (!prefader_buffers_[index].empty())
        slot_prefader_buffers_[(slot - 1) * nodes + index].assign(limits_.max_frames,
                                                                  StereoFrame{});
  step_slots_.assign(depth, {});
  for (auto& slot : step_slots_) {
    slot.sources.assign(nodes, std::vector<StereoFrame>(limits_.max_frames));
    slot.views.assign(nodes, {});
    slot.gains.assign(edges_.size(), std::vector<float>(limits_.max_frames));
    slot.gain_spans.assign(edges_.size(), {});
    slot.node_gains.assign(nodes, std::vector<StereoFrame>(limits_.max_frames));
    slot.node_gain_spans.assign(nodes, {});
  }
  // Every node of a step can run at once: one scratch lane per thread.
  scratch_.assign((AudioWorkerPool::max_workers + 1) * 2 * limits_.max_frames, StereoFrame{});
  steps_ = 0;
  return depth;
}

std::optional<std::size_t>
RoutedMixer::mix_step(std::span<const std::span<const StereoFrame>> sources, std::size_t frames,
                      BlockContext context, std::span<const EdgeGainSpan> gains,
                      std::span<const NodeGainSpan> node_gains, std::span<StereoFrame> output,
                      AudioWorkerPool* workers, RoutedStepTasks extra) noexcept {
  const auto depth = step_slots_.size();
  if (depth == 0 || frames == 0 || output.size() < limits_.max_frames ||
      !admits_block(sources, frames, context, gains, node_gains))
    return std::nullopt;
  generation_ = context.transport_generation;
  next_position_ = context.project_sample_position + frames;
  // Hold the fed block for the levels still to process it.
  auto& fed = step_slots_[steps_ % depth];
  for (std::size_t index = 0; index < sources.size(); ++index) {
    if (sources[index].empty()) {
      fed.views[index] = {};
    } else {
      std::ranges::copy(sources[index], fed.sources[index].begin());
      fed.views[index] = std::span(fed.sources[index]).first(frames);
    }
  }
  fed.has_gains = !gains.empty();
  for (std::size_t edge = 0; edge < gains.size(); ++edge) {
    std::ranges::copy(gains[edge].values, fed.gains[edge].begin());
    fed.gain_spans[edge] = {gains[edge].id,
                            std::span(fed.gains[edge]).first(gains[edge].values.size())};
  }
  fed.has_node_gains = !node_gains.empty();
  for (std::size_t node = 0; node < node_gains.size(); ++node) {
    std::ranges::copy(node_gains[node].values, fed.node_gains[node].begin());
    fed.node_gain_spans[node] = {
        node_gains[node].id, std::span(fed.node_gains[node]).first(node_gains[node].values.size())};
  }
  fed.frames = frames;
  fed.context = context;
  // Level L works on the block fed L steps ago; levels not reached yet wait.
  // A node at level L reads its sources' slot for that block, which they
  // wrote in an earlier step and will not overwrite for depth - L more.
  const auto reached = static_cast<std::size_t>(std::min<std::uint64_t>(steps_ + 1, depth));
  struct StepJob {
    RoutedMixer& mixer;
    std::uint64_t step;
    std::size_t nodes;
    RoutedStepTasks extra;
  } job{*this, steps_, level_offsets_[reached], extra};
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AudioWorkerPool::Task.
  const auto run = [](void* context, std::size_t task, std::size_t lane) noexcept {
    auto& step_job = *static_cast<StepJob*>(context);
    if (task >= step_job.nodes) {
      step_job.extra.task(step_job.extra.context, task - step_job.nodes, lane);
      return;
    }
    auto& mixer = step_job.mixer;
    const auto index = mixer.level_nodes_[task];
    const auto block = step_job.step - mixer.node_level_[index];
    const auto slot = static_cast<std::size_t>(block % mixer.step_slots_.size());
    const auto& held = mixer.step_slots_[slot];
    const NodeBlock node_block{held.views,
                               held.frames,
                               held.context,
                               held.has_gains ? std::span<const EdgeGainSpan>(held.gain_spans)
                                              : std::span<const EdgeGainSpan>{},
                               held.has_node_gains
                                   ? std::span<const NodeGainSpan>(held.node_gain_spans)
                                   : std::span<const NodeGainSpan>{},
                               slot};
    mixer.mix_node(index, node_block,
                   std::span(mixer.scratch_)
                       .subspan(lane * 2 * mixer.limits_.max_frames, 2 * mixer.limits_.max_frames));
  };
  const auto tasks = job.nodes + extra.count;
  if (workers == nullptr || workers->attached_workers() == 0) {
    for (std::size_t task = 0; task < tasks; ++task)
      run(&job, task, 0);
  } else {
    workers->run(tasks, run, &job);
  }
  ++steps_;
  if (steps_ < depth)
    return std::size_t{0};
  // The master (last in topological order, the deepest level) has finished
  // the block fed depth - 1 steps ago.
  const auto slot = static_cast<std::size_t>((steps_ - depth) % depth);
  const auto emitted = step_slots_[slot].frames;
  std::ranges::copy(std::span(node_buffer(buffers_.size() - 1, slot)).first(emitted),
                    output.begin());
  return emitted;
}

void RoutedMixer::build_levels() {
  prefader_buffers_.assign(buffers_.size(), {});
  for (const auto& edge : edges_) {
    if (edge.kind == RoutingEdgeKind::sidechain && prefader_buffers_[edge.source_index].empty())
      prefader_buffers_[edge.source_index].assign(limits_.max_frames, StereoFrame{});
  }
  // Sources precede destinations in topological order, so one pass assigns
  // each node one more than its deepest source.
  std::vector<std::size_t> level(buffers_.size(), 0);
  std::size_t depth = 0;
  for (std::size_t index = 0; index < buffers_.size(); ++index) {
    for (const auto edge : incoming_[index])
      level[index] = std::max(level[index], level[edges_[edge].source_index] + 1);
    depth = std::max(depth, level[index] + 1);
  }
  level_offsets_.assign(depth + 1, 0);
  for (const auto value : level)
    ++level_offsets_[value + 1];
  std::size_t widest = 1;
  for (std::size_t value = 0; value < depth; ++value) {
    widest = std::max(widest, level_offsets_[value + 1]);
    level_offsets_[value + 1] += level_offsets_[value];
  }
  node_level_ = level;
  level_nodes_.assign(buffers_.size(), 0);
  auto cursor = level_offsets_;
  for (std::size_t index = 0; index < buffers_.size(); ++index)
    level_nodes_[cursor[level[index]]++] = index;
  // One scratch lane per thread that can share a level.
  const auto lanes = std::min(widest, AudioWorkerPool::max_workers + 1);
  // Each lane: one block for route delays and summed drains, one for a drain.
  scratch_.assign(lanes * 2 * limits_.max_frames, StereoFrame{});
}

} // namespace oscillate::engine
