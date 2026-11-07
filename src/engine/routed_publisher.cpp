#include "oscillate/engine/routed_publisher.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace oscillate::engine {
namespace {
constexpr std::size_t callback_frame_cap = 8192;

[[nodiscard]] foundation::Result<void> fail(foundation::ErrorCode code, const char* message) {
  return foundation::Result<void>::failure({code, message});
}

[[nodiscard]] bool same_context(BlockContext first, BlockContext second) noexcept {
  return first.project_sample_position == second.project_sample_position &&
         first.transport_generation == second.transport_generation;
}
} // namespace

RoutedMixerPublisher::~RoutedMixerPublisher() = default;

void RoutedMixerPublisher::silence(std::span<StereoFrame> output) noexcept {
  if (output.size() <= callback_frame_cap)
    std::ranges::fill(output, StereoFrame{});
}

bool RoutedMixerPublisher::valid_inputs(const Plan* plan,
                                        std::span<const std::span<const StereoFrame>> sources,
                                        std::size_t frames) noexcept {
  if (frames > plan->mixer->max_frames() ||
      sources.size() != plan->mixer->plan().topological_order.size())
    return false;
  return std::ranges::all_of(sources, [frames](std::span<const StereoFrame> source) {
    return source.empty() || source.size() == frames;
  });
}

foundation::Result<void> RoutedMixerPublisher::prepare_and_publish(
    std::uint64_t revision, std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
    RoutedMixerLimits limits, std::shared_ptr<RoutedGainSource> gains) {
  return publish(revision, nodes, edges, limits, std::move(gains), false);
}

foundation::Result<void> RoutedMixerPublisher::prepare_and_publish_live(
    std::uint64_t revision, std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
    RoutedMixerLimits limits, std::shared_ptr<RoutedGainSource> gains) {
  return publish(revision, nodes, edges, limits, std::move(gains), true);
}

foundation::Result<void>
RoutedMixerPublisher::publish(std::uint64_t revision, std::span<const RoutedNode> nodes,
                              std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
                              std::shared_ptr<RoutedGainSource> gains, bool live) {
  if (revision == 0 || has_pending() || retired_.load(std::memory_order_acquire) != nullptr)
    return fail(foundation::ErrorCode::conflict,
                "routed publication needs a new revision and empty handoff slots");
  auto* prior = active_.load(std::memory_order_acquire);
  if (live &&
      (prior == nullptr || revision <= prior->revision ||
       hold_requested_.load(std::memory_order_acquire) || held_.load(std::memory_order_acquire)))
    return fail(foundation::ErrorCode::conflict,
                "a live switch needs an active, unheld graph and a newer revision");
  if (!live && prior != nullptr &&
      (revision <= prior->revision || !held_.load(std::memory_order_acquire) ||
       !hold_requested_.load(std::memory_order_acquire)))
    return fail(foundation::ErrorCode::conflict,
                "active routed mixer must be held for a newer revision");

  try {
    auto prepared = live ? prepare_routed_mixer_live(nodes, edges, limits, *prior->mixer)
                         : prepare_routed_mixer(nodes, edges, limits,
                                                prior == nullptr ? nullptr : prior->mixer.get(),
                                                prior == nullptr ? BlockContext{} : boundary_);
    if (!prepared)
      return foundation::Result<void>::failure(prepared.error());
    if (prior != nullptr &&
        prior->mixer->plan().topological_order != prepared.value()->plan().topological_order)
      return fail(foundation::ErrorCode::conflict,
                  "routed source order changed; named-source publication is required");

    auto plan = std::make_unique<Plan>(
        Plan{.revision = revision,
             .mixer = std::shared_ptr<RoutedMixer>(std::move(prepared).value()),
             .gains = std::move(gains),
             .live_switch = live,
             .source_revision = revision});
    plan->controls.reserve(plan->mixer->plan().nodes.size());
    for (const auto& node : plan->mixer->plan().nodes) {
      const auto found = std::ranges::find_if(
          nodes, [id = node.id](const RoutedNode& item) { return item.routing.id == id; });
      plan->controls.push_back({node.id, found->muted, found->output_gain});
    }
    plan->edge_controls.reserve(plan->mixer->plan().edges.size());
    for (const auto& edge : plan->mixer->plan().edges)
      plan->edge_controls.push_back({edge.id, edge.gain});
    auto* raw = plan.get();
    owned_.push_back(std::move(plan));
    pending_.store(raw, std::memory_order_release);
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "routed publication ran out of control-side memory");
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> RoutedMixerPublisher::prepare_source_overlay(std::uint64_t revision) {
  auto* prior = active_.load(std::memory_order_acquire);
  if (!prior || revision <= prior->revision || has_pending() ||
      retired_.load(std::memory_order_acquire) != nullptr ||
      hold_requested_.load(std::memory_order_acquire) || held_.load(std::memory_order_acquire))
    return fail(foundation::ErrorCode::conflict,
                "Source overlay needs a settled graph and a new revision");
  try {
    auto candidate = std::make_unique<Plan>(Plan{.revision = revision,
                                                 .mixer = prior->mixer,
                                                 .gains = prior->gains,
                                                 .source_overlay = true,
                                                 .source_revision = revision});
    candidate->controls = prior->controls;
    candidate->edge_controls = prior->edge_controls;
    auto* raw = candidate.get();
    owned_.push_back(std::move(candidate));
    pending_.store(raw, std::memory_order_release);
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "Source overlay publication ran out of control-side memory");
  }
  return foundation::Result<void>::success();
}

foundation::Result<void>
RoutedMixerPublisher::prepare_control_overlay(std::uint64_t revision,
                                              std::span<const RoutedNodeControl> controls) {
  auto* prior = active_.load(std::memory_order_acquire);
  if (!prior || revision <= prior->revision || has_pending() ||
      retired_.load(std::memory_order_acquire) != nullptr ||
      hold_requested_.load(std::memory_order_acquire) || held_.load(std::memory_order_acquire))
    return fail(foundation::ErrorCode::conflict,
                "Control overlay needs a settled graph without a dry insert and a new revision");
  if (!prior->mixer->control_overlay_safe())
    return fail(foundation::ErrorCode::unsupported,
                "Control overlay has a revision-sensitive processor or an active tail");
  const auto& nodes = prior->mixer->plan().nodes;
  if (controls.size() != nodes.size() || controls.size() != prior->controls.size())
    return fail(foundation::ErrorCode::invalid_argument,
                "Control overlay must cover every existing route node");
  for (std::size_t index = 0; index < controls.size(); ++index) {
    const auto& control = controls[index];
    if (control.id != nodes[index].id || !std::isfinite(control.output_gain.left) ||
        !std::isfinite(control.output_gain.right) || control.output_gain.left < 0.0F ||
        control.output_gain.left > 4.0F || control.output_gain.right < 0.0F ||
        control.output_gain.right > 4.0F)
      return fail(foundation::ErrorCode::invalid_argument,
                  "Control overlay has a mismatched node or invalid gain");
    const auto& old = prior->controls[index];
    if (control.muted != old.muted || control.output_gain.left != old.output_gain.left ||
        control.output_gain.right != old.output_gain.right) {
      if (prior->gains && prior->gains->binds_node_gain(control.id))
        return fail(foundation::ErrorCode::unsupported,
                    "Control overlay conflicts with channel-level automation");
    }
  }
  try {
    auto candidate = std::make_unique<Plan>(Plan{.revision = revision,
                                                 .mixer = prior->mixer,
                                                 .gains = prior->gains,
                                                 .control_overlay = true,
                                                 .source_revision = prior->source_revision});
    candidate->controls.assign(controls.begin(), controls.end());
    candidate->edge_controls = prior->edge_controls;
    auto* raw = candidate.get();
    owned_.push_back(std::move(candidate));
    pending_.store(raw, std::memory_order_release);
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "Control overlay preparation ran out of memory");
  }
  return foundation::Result<void>::success();
}

foundation::Result<void>
RoutedMixerPublisher::prepare_edge_overlay(std::uint64_t revision,
                                           std::span<const RoutedEdgeControl> controls) {
  auto* prior = active_.load(std::memory_order_acquire);
  if (!prior || revision <= prior->revision || has_pending() ||
      retired_.load(std::memory_order_acquire) != nullptr ||
      hold_requested_.load(std::memory_order_acquire) || held_.load(std::memory_order_acquire))
    return fail(foundation::ErrorCode::conflict,
                "Edge overlay needs a settled graph without a dry insert and a new revision");
  if (!prior->mixer->control_overlay_safe())
    return fail(foundation::ErrorCode::unsupported,
                "Edge overlay has a revision-sensitive processor or an active tail");
  const auto& edges = prior->mixer->plan().edges;
  if (controls.size() != edges.size() || controls.size() != prior->edge_controls.size())
    return fail(foundation::ErrorCode::invalid_argument,
                "Edge overlay must cover every existing route edge");
  for (std::size_t index = 0; index < controls.size(); ++index) {
    const auto& control = controls[index];
    if (control.id != edges[index].id || !std::isfinite(control.gain) || control.gain < 0.0F ||
        control.gain > 4.0F)
      return fail(foundation::ErrorCode::invalid_argument,
                  "Edge overlay has a mismatched edge or invalid gain");
    if (control.gain != prior->edge_controls[index].gain && prior->gains &&
        prior->gains->binds_edge_gain(control.id))
      return fail(foundation::ErrorCode::unsupported,
                  "Edge overlay conflicts with send-gain automation");
  }
  try {
    auto candidate = std::make_unique<Plan>(Plan{.revision = revision,
                                                 .mixer = prior->mixer,
                                                 .gains = prior->gains,
                                                 .control_overlay = true,
                                                 .source_revision = prior->source_revision});
    candidate->controls = prior->controls;
    candidate->edge_controls.assign(controls.begin(), controls.end());
    auto* raw = candidate.get();
    owned_.push_back(std::move(candidate));
    pending_.store(raw, std::memory_order_release);
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "Edge overlay preparation ran out of memory");
  }
  return foundation::Result<void>::success();
}

bool RoutedMixerPublisher::push_live(LiveControl control) noexcept {
  const auto head = live_head_.load(std::memory_order_relaxed);
  if (head - live_tail_.load(std::memory_order_acquire) >= live_capacity)
    return false;
  live_[head % live_capacity] = control;
  live_head_.store(head + 1, std::memory_order_release);
  return true;
}

bool RoutedMixerPublisher::push_live_node_gain(RoutingNodeId node, StereoFrame gain) noexcept {
  if (!std::isfinite(gain.left) || !std::isfinite(gain.right) || gain.left < 0.0F ||
      gain.left > 4.0F || gain.right < 0.0F || gain.right > 4.0F)
    return false;
  return push_live({.edge = false, .id = node, .gain = gain});
}

bool RoutedMixerPublisher::push_live_edge_gain(RoutingEdgeId edge, float gain) noexcept {
  if (!std::isfinite(gain) || gain < 0.0F || gain > 4.0F)
    return false;
  return push_live({.edge = true, .id = edge, .gain = {gain, gain}});
}

void RoutedMixerPublisher::drain_live(const Plan& active) noexcept {
  const auto head = live_head_.load(std::memory_order_acquire);
  auto tail = live_tail_.load(std::memory_order_relaxed);
  for (; tail != head; ++tail) {
    const auto& control = live_[tail % live_capacity];
    if (control.edge) {
      if (!active.gains || !active.gains->binds_edge_gain(control.id))
        (void)active.mixer->apply_live_edge_gain(control.id, control.gain.left);
    } else if (!active.gains || !active.gains->binds_node_gain(control.id)) {
      (void)active.mixer->apply_live_node_gain(control.id, control.gain);
    }
  }
  live_tail_.store(tail, std::memory_order_release);
}

bool RoutedMixerPublisher::request_hold() noexcept {
  if (active_.load(std::memory_order_acquire) == nullptr || has_pending() ||
      retired_.load(std::memory_order_acquire) != nullptr ||
      held_.load(std::memory_order_acquire) || hold_requested_.load(std::memory_order_acquire))
    return false;
  hold_requested_.store(true, std::memory_order_release);
  return true;
}

std::optional<BlockContext> RoutedMixerPublisher::held_boundary() const noexcept {
  if (!held_.load(std::memory_order_acquire))
    return std::nullopt;
  return boundary_;
}

bool RoutedMixerPublisher::cancel_hold() noexcept {
  if (!hold_requested_.load(std::memory_order_acquire) || has_pending())
    return false;
  hold_requested_.store(false, std::memory_order_release);
  return true;
}

std::size_t RoutedMixerPublisher::collect_retired() {
  auto* retired = retired_.exchange(nullptr, std::memory_order_acq_rel);
  if (retired == nullptr)
    return 0;
  destroy_owned(retired);
  return 1;
}

bool RoutedMixerPublisher::withdraw_pending() noexcept {
  if (pending_.load(std::memory_order_acquire) == nullptr ||
      withdraw_requested_.load(std::memory_order_acquire))
    return false;
  withdraw_requested_.store(true, std::memory_order_release);
  return true;
}

// Callback: the candidate moves to the retired slot, which is empty while one
// is pending (each publication needs it empty, and only a callback switch,
// which consumes the candidate, fills it). Once the request is answered the
// callback never reads the candidate again, so the control side may free it.
void RoutedMixerPublisher::answer_withdrawal() noexcept {
  if (!withdraw_requested_.load(std::memory_order_acquire))
    return;
  if (auto* candidate = pending_.exchange(nullptr, std::memory_order_acq_rel))
    retired_.store(candidate, std::memory_order_release);
  withdraw_requested_.store(false, std::memory_order_release);
}

void RoutedMixerPublisher::destroy_owned(const Plan* plan) {
  const auto found =
      std::ranges::find_if(owned_, [plan](const auto& item) { return item.get() == plan; });
  if (found != owned_.end())
    owned_.erase(found);
}

std::uint64_t RoutedMixerPublisher::active_revision() const noexcept {
  const auto* active = active_.load(std::memory_order_acquire);
  return active == nullptr ? 0 : active->revision;
}

bool RoutedMixerPublisher::has_pending() const noexcept {
  return pending_.load(std::memory_order_acquire) != nullptr ||
         withdraw_requested_.load(std::memory_order_acquire);
}

std::optional<RoutedNodeMeterReading>
RoutedMixerPublisher::active_node_meter(RoutingNodeId node) const noexcept {
  const auto* active = active_.load(std::memory_order_acquire);
  return active == nullptr ? std::nullopt : active->mixer->node_meter(node);
}

RoutedRenderStatus
RoutedMixerPublisher::render_block(std::span<const std::span<const StereoFrame>> sources,
                                   std::span<StereoFrame> output, BlockContext context) noexcept {
  return render_block(sources, output, context, {});
}

RoutedRenderStatus RoutedMixerPublisher::render_block(
    std::span<const std::span<const StereoFrame>> sources, std::span<StereoFrame> output,
    BlockContext context, RoutedSourceFill source_fill, AudioWorkerPool* workers) noexcept {
  answer_withdrawal();
  if (output.size() > callback_frame_cap)
    return RoutedRenderStatus::invalid_block;
  if (output.empty() || context.transport_generation == 0 ||
      output.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    silence(output);
    return RoutedRenderStatus::invalid_block;
  }
  auto* active = active_.load(std::memory_order_relaxed);
  bool filled = false;
  bool live_passed = false;
  RoutedGainSpans gains;
  auto selected_sources = sources;
  const auto fill_sources = [&](Plan* plan) noexcept {
    gains = {};
    if (plan->gains && !plan->gains->fill_block(context, output.size(), gains)) {
      silence(output);
      return false;
    }
    if (source_fill.fill_selected != nullptr) {
      if (!source_fill.fill_selected(source_fill.context,
                                     plan->control_overlay ? plan->source_revision : plan->revision,
                                     context, output.size(), selected_sources) ||
          !valid_inputs(plan, selected_sources, output.size())) {
        silence(output);
        return false;
      }
    } else if (source_fill.fill != nullptr &&
               !source_fill.fill(source_fill.context,
                                 plan->control_overlay ? plan->source_revision : plan->revision,
                                 context, output.size())) {
      silence(output);
      return false;
    }
    filled = true;
    return true;
  };
  if (active == nullptr) {
    auto* candidate = pending_.load(std::memory_order_acquire);
    if (candidate != nullptr && ((source_fill.fill_selected == nullptr &&
                                  !valid_inputs(candidate, sources, output.size())) ||
                                 !candidate->mixer->accepts_context(context, output.size()))) {
      silence(output);
      return RoutedRenderStatus::invalid_block;
    }
    if (candidate != nullptr && !fill_sources(candidate))
      return RoutedRenderStatus::invalid_block;
    auto* initial = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (initial != nullptr) {
      active_.store(initial, std::memory_order_release);
      initial->mixer->graph_activated(initial->revision);
      active = initial;
    }
  } else if (held_.load(std::memory_order_relaxed) ||
             hold_requested_.load(std::memory_order_acquire)) {
    if (!held_.load(std::memory_order_relaxed)) {
      boundary_ = context;
      held_.store(true, std::memory_order_release);
    }
    if (!same_context(context, boundary_)) {
      silence(output);
      return RoutedRenderStatus::invalid_block;
    }
    auto* candidate = pending_.load(std::memory_order_acquire);
    if (candidate != nullptr && ((source_fill.fill_selected == nullptr &&
                                  !valid_inputs(candidate, sources, output.size())) ||
                                 !candidate->mixer->accepts_context(context, output.size()))) {
      silence(output);
      return RoutedRenderStatus::invalid_block;
    }
    if (candidate != nullptr && !fill_sources(candidate))
      return RoutedRenderStatus::invalid_block;
    auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (next != nullptr) {
      active->mixer->graph_deactivated(active->revision);
      active_.store(next, std::memory_order_release);
      next->mixer->graph_activated(next->revision);
      retired_.store(active, std::memory_order_release);
      active = next;
      hold_requested_.store(false, std::memory_order_release);
      held_.store(false, std::memory_order_release);
    } else if (hold_requested_.load(std::memory_order_acquire)) {
      silence(output);
      return RoutedRenderStatus::holding;
    } else {
      held_.store(false, std::memory_order_release);
    }
  } else if (auto* candidate = pending_.load(std::memory_order_acquire);
             candidate && candidate->live_switch) {
    // The new graph renders this block: no hold, no silent block. When its
    // sources cannot fill yet, the active graph renders and the switch waits.
    if (!candidate->mixer->continues(*active->mixer)) {
      // Prepared against another graph: discard it; the control side sees
      // the active revision unchanged.
      pending_.store(nullptr, std::memory_order_release);
      retired_.store(candidate, std::memory_order_release);
    } else if (candidate->mixer->accepts_context(context, output.size()) &&
               fill_sources(candidate) &&
               candidate->mixer->adopt_live_state(*active->mixer, context)) {
      pending_.store(nullptr, std::memory_order_release);
      active->mixer->graph_deactivated(active->revision);
      active_.store(candidate, std::memory_order_release);
      candidate->mixer->graph_activated(candidate->revision);
      retired_.store(active, std::memory_order_release);
      active = candidate;
    } else {
      live_passed = true;
    }
  } else if (auto* candidate = pending_.load(std::memory_order_acquire);
             candidate && (candidate->source_overlay || candidate->control_overlay) &&
             candidate->mixer == active->mixer) {
    if ((source_fill.fill_selected == nullptr &&
         !valid_inputs(candidate, sources, output.size())) ||
        !candidate->mixer->accepts_context(context, output.size())) {
      silence(output);
      return RoutedRenderStatus::invalid_block;
    }
    // A control overlay changes no source bank: it fills from the audible one.
    if (!fill_sources(candidate->control_overlay ? active : candidate)) {
      if (!candidate->source_overlay || !fill_sources(active))
        return RoutedRenderStatus::invalid_block;
    } else {
      auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
      if (next != candidate) {
        silence(output);
        return RoutedRenderStatus::invalid_block;
      }
      // An overlay retains all mixer and processor histories.
      if (candidate->control_overlay) {
        next->mixer->control_overlay_rebound(active->revision, next->revision);
        next->mixer->apply_node_controls(next->controls);
        next->mixer->apply_edge_controls(next->edge_controls);
      }
      active_.store(next, std::memory_order_release);
      retired_.store(active, std::memory_order_release);
      active = next;
    }
  }
  if (active == nullptr) {
    silence(output);
    return RoutedRenderStatus::no_graph;
  }
  if (!filled &&
      ((source_fill.fill_selected == nullptr && !valid_inputs(active, sources, output.size())) ||
       !active->mixer->accepts_context(context, output.size()))) {
    silence(output);
    return RoutedRenderStatus::invalid_block;
  }
  if (!filled && !fill_sources(active))
    return RoutedRenderStatus::invalid_block;
  drain_live(*active);
  if (!active->mixer->mix_block(selected_sources, output, context, gains.edges, gains.nodes,
                                workers))
    return RoutedRenderStatus::invalid_block;
  if (live_passed)
    live_passed_blocks_.store(live_passed_blocks_.load(std::memory_order_relaxed) + 1,
                              std::memory_order_release);
  return RoutedRenderStatus::rendered;
}

} // namespace oscillate::engine
