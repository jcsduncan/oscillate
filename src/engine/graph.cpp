#include <oscillate/engine/graph.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace oscillate::engine {
namespace {
constexpr std::size_t max_supported_frames = 8192;
constexpr std::size_t max_supported_nodes = 1024;

[[nodiscard]] foundation::Result<std::unique_ptr<RenderPlan>> fail(foundation::ErrorCode code,
                                                                   std::string message) {
  return foundation::Result<std::unique_ptr<RenderPlan>>::failure(
      {.code = code, .message = std::move(message)});
}
} // namespace

RenderPlan::RenderPlan(const Identity& identity, std::vector<Node> nodes,
                       std::vector<Transition> transitions) noexcept
    : revision_(identity.revision), base_revision_(identity.base_revision),
      max_frames_(identity.max_frames), nodes_(std::move(nodes)),
      transitions_(std::move(transitions)),
      requires_fallback_(std::ranges::any_of(
          transitions_, [](const Transition& item) { return item.audible_continuity_limited; })) {}

void RenderPlan::render(std::span<StereoFrame> stereo, BlockContext context) const noexcept {
  std::ranges::fill(stereo, StereoFrame{0.0F, 0.0F});
  for (const auto& node : nodes_) {
    node.runtime->process(stereo, context);
  }
}

foundation::Result<std::unique_ptr<RenderPlan>> prepare_graph(std::uint64_t revision,
                                                              std::size_t max_frames,
                                                              std::span<const NodeRequest> requests,
                                                              const RenderPlan* prior) {
  if (revision == 0 || (prior != nullptr && revision <= prior->revision_)) {
    return fail(foundation::ErrorCode::invalid_argument,
                "graph revision must advance beyond the prior revision");
  }
  if (max_frames == 0 || max_frames > max_supported_frames) {
    return fail(foundation::ErrorCode::invalid_argument,
                "graph maximum block size is outside 1..8192");
  }
  if (requests.size() > max_supported_nodes) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "graph exceeds the 1024-node contract limit");
  }

  std::vector<RenderPlan::Node> nodes;
  std::vector<Transition> transitions;
  nodes.reserve(requests.size());
  transitions.reserve(requests.size() + (prior == nullptr ? 0 : prior->nodes_.size()));
  for (const auto& request : requests) {
    if (request.instance_id == 0 || request.compatibility_key == 0) {
      return fail(foundation::ErrorCode::invalid_argument,
                  "processor ID and compatibility key must be nonzero");
    }
    if (std::ranges::any_of(
            nodes, [&](const auto& node) { return node.instance_id == request.instance_id; })) {
      return fail(foundation::ErrorCode::conflict, "processor instance ID occurs more than once");
    }
    const auto* previous = prior == nullptr ? nullptr : [&]() -> const RenderPlan::Node* {
      const auto found = std::ranges::find_if(
          prior->nodes_, [&](const auto& node) { return node.instance_id == request.instance_id; });
      return found == prior->nodes_.end() ? nullptr : &*found;
    }();

    std::shared_ptr<ProcessorRuntime> runtime;
    TransitionKind kind = TransitionKind::created;
    // A runtime prepared for a smaller callback block may own undersized scratch.
    // Replacing it is safer than assuming it can grow without preparation.
    if (previous != nullptr && previous->compatibility_key == request.compatibility_key &&
        max_frames <= prior->max_frames_) {
      runtime = previous->owner;
      kind = TransitionKind::reused;
    } else {
      if (!request.create_runtime) {
        return fail(foundation::ErrorCode::invalid_argument,
                    "new or replaced processor requires a runtime factory");
      }
      try {
        runtime = request.create_runtime();
      } catch (const std::exception& error) {
        return fail(foundation::ErrorCode::unavailable,
                    std::string("runtime preparation failed: ") + error.what());
      } catch (...) {
        return fail(foundation::ErrorCode::unavailable,
                    "runtime preparation failed with an unknown exception");
      }
      if (runtime == nullptr) {
        return fail(foundation::ErrorCode::unavailable, "runtime factory returned no processor");
      }
      if (previous != nullptr) {
        kind = TransitionKind::replaced;
      }
    }
    auto* raw = runtime.get();
    nodes.push_back({request.instance_id, request.compatibility_key, std::move(runtime), raw});
    transitions.push_back({request.instance_id, kind,
                           kind == TransitionKind::replaced ||
                               (kind == TransitionKind::created && prior != nullptr)});
  }
  if (prior != nullptr) {
    for (const auto& old_node : prior->nodes_) {
      if (std::ranges::none_of(
              nodes, [&](const auto& node) { return node.instance_id == old_node.instance_id; })) {
        transitions.push_back({old_node.instance_id, TransitionKind::retired, true});
      }
    }
  }
  const auto base_revision = prior == nullptr ? 0 : prior->revision_;
  return foundation::Result<std::unique_ptr<RenderPlan>>::success(
      std::unique_ptr<RenderPlan>(new RenderPlan({revision, base_revision, max_frames},
                                                 std::move(nodes), std::move(transitions))));
}

GraphPublisher::~GraphPublisher() = default;

PublishStatus GraphPublisher::publish(std::unique_ptr<RenderPlan> plan) {
  if (plan == nullptr) {
    return PublishStatus::invalid_plan;
  }
  const auto requested_revision = plan->revision();
  const auto current_revision = active_revision();
  const bool retry = requested_revision == highest_requested_revision_ &&
                     requested_revision == retryable_revision_ &&
                     pending_.load(std::memory_order_acquire) == nullptr &&
                     rejected_.load(std::memory_order_acquire) == nullptr &&
                     current_revision < requested_revision &&
                     plan->base_revision() == current_revision;
  if (requested_revision < highest_requested_revision_ ||
      (requested_revision == highest_requested_revision_ && !retry)) {
    return PublishStatus::stale_revision;
  }
  auto* raw = plan.get();
  owned_.push_back(std::move(plan));
  highest_requested_revision_ = requested_revision;
  retryable_revision_ = 0;
  auto* superseded = pending_.exchange(raw, std::memory_order_acq_rel);
  if (superseded != nullptr) {
    destroy_owned(superseded);
  }
  return PublishStatus::accepted;
}

CollectResult GraphPublisher::collect_retired() {
  CollectResult result;
  auto* retired = retired_.exchange(nullptr, std::memory_order_acq_rel);
  if (retired != nullptr) {
    destroy_owned(retired);
    ++result.reclaimed;
  }
  auto* rejected = rejected_.exchange(nullptr, std::memory_order_acq_rel);
  if (rejected != nullptr) {
    result.rejected_revision = rejected->revision();
    if (result.rejected_revision == highest_requested_revision_ &&
        pending_.load(std::memory_order_acquire) == nullptr &&
        active_revision() < result.rejected_revision) {
      retryable_revision_ = result.rejected_revision;
    }
    destroy_owned(rejected);
    ++result.reclaimed;
  }
  return result;
}

void GraphPublisher::destroy_owned(const RenderPlan* plan) {
  const auto found =
      std::ranges::find_if(owned_, [plan](const auto& entry) { return entry.get() == plan; });
  if (found != owned_.end()) {
    owned_.erase(found);
  }
}

const RenderPlan* GraphPublisher::active_plan() const noexcept {
  return active_.load(std::memory_order_acquire);
}

std::uint64_t GraphPublisher::active_revision() const noexcept {
  const auto* active = active_plan();
  return active == nullptr ? 0 : active->revision();
}

bool GraphPublisher::has_pending() const noexcept {
  return pending_.load(std::memory_order_acquire) != nullptr;
}

bool GraphPublisher::render_block(std::span<StereoFrame> stereo, BlockContext context) noexcept {
  if (stereo.empty() || stereo.size() > max_supported_frames) {
    return false;
  }
  if (context.transport_generation == 0 ||
      stereo.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    std::ranges::fill(stereo, StereoFrame{0.0F, 0.0F});
    last_output_ = {0.0F, 0.0F};
    return false;
  }
  // The callback alone produces retirement. A full slot keeps pending intact.
  if (retired_.load(std::memory_order_acquire) == nullptr &&
      rejected_.load(std::memory_order_acquire) == nullptr) {
    auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (next != nullptr) {
      if (stereo.size() > next->max_frames_) {
        // Control may publish a newer plan while this slot is temporarily empty.
        // Restore only if still empty; otherwise retire this superseded plan
        // without destruction or ownership work on the callback.
        RenderPlan* empty = nullptr;
        if (!pending_.compare_exchange_strong(empty, next, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          retired_.store(next, std::memory_order_release);
        }
      } else {
        auto* current = active_.load(std::memory_order_relaxed);
        const auto current_revision = current == nullptr ? 0 : current->revision();
        if (next->base_revision() == current_revision) {
          if (current != nullptr) {
            for (const auto& node : current->nodes_)
              node.runtime->graph_deactivated(current_revision);
          }
          active_.store(next, std::memory_order_release);
          for (const auto& node : next->nodes_)
            node.runtime->graph_activated(next->revision());
          fade_next_block_ = next->requires_fallback_;
          if (current != nullptr) {
            retired_.store(current, std::memory_order_release);
          }
        } else {
          rejected_.store(next, std::memory_order_release);
        }
      }
    }
  }
  auto* active = active_.load(std::memory_order_relaxed);
  if (active == nullptr || stereo.size() > active->max_frames_) {
    std::ranges::fill(stereo, StereoFrame{0.0F, 0.0F});
    last_output_ = {0.0F, 0.0F};
    return false;
  }
  active->render(stereo, context);
  if (fade_next_block_) {
    const auto count = stereo.size();
    for (std::size_t index = 0; index < count; ++index) {
      const auto mix = static_cast<float>(index + 1) / static_cast<float>(count);
      stereo[index].left = last_output_.left * (1.0F - mix) + stereo[index].left * mix;
      stereo[index].right = last_output_.right * (1.0F - mix) + stereo[index].right * mix;
    }
    fade_next_block_ = false;
  }
  last_output_ = stereo.back();
  return true;
}

} // namespace oscillate::engine
