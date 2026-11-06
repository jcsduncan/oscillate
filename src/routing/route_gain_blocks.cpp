#include "oscillate/routing/route_gain_blocks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace oscillate::routing {
namespace {
using foundation::ErrorCode;
using Result = foundation::Result<std::shared_ptr<PreparedRouteGainBlocks>>;

[[nodiscard]] Result fail(ErrorCode code, const char* message) {
  return Result::failure({code, message});
}
} // namespace

Result PreparedRouteGainBlocks::prepare(
    const project::Snapshot& snapshot, const ProjectRoutePlan& route,
    std::span<const automation_project::PreparedTargetAutomation> targets, std::size_t max_frames) {
  if (max_frames == 0 || max_frames > 8192 || snapshot.revision == 0)
    return fail(ErrorCode::invalid_argument, "invalid route gain block size or revision");
  try {
    auto result = std::make_shared<PreparedRouteGainBlocks>();
    result->sample_rate_ = snapshot.sample_rate;
    result->tempo_ = std::make_shared<const timeline::TempoMap>(snapshot.tempo);
    result->edge_spans_.reserve(route.latency.edges.size());
    for (const auto& edge : route.latency.edges)
      result->edge_spans_.push_back({edge.id, {}});
    result->node_spans_.reserve(route.latency.nodes.size());
    for (const auto& node : route.latency.nodes)
      result->node_spans_.push_back({node.id, {}});
    result->ticks_.resize(max_frames);
    result->targets_.assign(targets.begin(), targets.end());
    for (std::size_t index = 0; index < targets.size(); ++index) {
      const auto& target = targets[index];
      if (target.snapshot_revision() != snapshot.revision ||
          target.tempo_revision() != snapshot.tempo.revision())
        return fail(ErrorCode::conflict, "route gain preparation has a stale project revision");
      const auto* send = std::get_if<project::SendGainTarget>(&target.target());
      const auto* control = std::get_if<project::TrackControlTarget>(&target.target());
      if (send == nullptr && (control == nullptr || !project::is_channel_control(control->key)))
        continue;
      if (control != nullptr) {
        const auto track = std::ranges::find(snapshot.tracks, target.track(), &project::Track::id);
        const auto mapped =
            std::ranges::find(route.track_nodes, target.track(), &ProjectRouteTrackNode::track);
        if (track == snapshot.tracks.end() || mapped == route.track_nodes.end() ||
            control->track != target.track())
          return fail(ErrorCode::unavailable, "channel automation has no routed track");
        const auto node =
            std::ranges::find(result->node_spans_, mapped->node, &engine::NodeGainSpan::id);
        if (node == result->node_spans_.end())
          return fail(ErrorCode::unavailable, "channel automation node is absent from mixer plan");
        const auto node_index = static_cast<std::size_t>(node - result->node_spans_.begin());
        auto binding =
            std::ranges::find(result->node_bindings_, node_index, &NodeBinding::node_index);
        if (binding == result->node_bindings_.end()) {
          const auto count = result->node_bindings_.size() + 1;
          const auto storage_per_frame =
              count * (3 * sizeof(double) + sizeof(engine::StereoFrame)) +
              result->edge_bindings_.size() * (sizeof(double) + sizeof(float));
          if (count > project::max_automation_lanes_per_project ||
              storage_per_frame > (16ULL * 1024 * 1024) / max_frames)
            return fail(ErrorCode::resource_exhausted, "route gain block storage exceeds budget");
          NodeBinding added;
          added.node_index = node_index;
          added.level = track->level;
          added.pan = track->pan;
          added.mute_gain = track->muted ? 0.0 : 1.0;
          added.samples.resize(max_frames);
          result->node_bindings_.push_back(std::move(added));
          binding = std::prev(result->node_bindings_.end());
        }
        auto& slot =
            control->key == project::TrackControlTarget::Key::channel_level ? binding->level_target
            : control->key == project::TrackControlTarget::Key::channel_pan ? binding->pan_target
                                                                            : binding->mute_target;
        auto& values =
            control->key == project::TrackControlTarget::Key::channel_level ? binding->level_values
            : control->key == project::TrackControlTarget::Key::channel_pan ? binding->pan_values
                                                                            : binding->mute_values;
        if (slot)
          return fail(ErrorCode::conflict, "two automation targets bind the same channel control");
        slot = index;
        values.resize(max_frames);
        continue;
      }
      const auto mapped = std::ranges::find_if(route.sends, [&](const ProjectRouteSend& item) {
        return item.track == target.track() && item.send == send->send;
      });
      if (mapped == route.sends.end())
        return fail(ErrorCode::unavailable, "send gain has no routed edge");
      const auto edge =
          std::ranges::find_if(result->edge_spans_, [&](const engine::EdgeGainSpan& item) {
            return item.id == mapped->edge;
          });
      if (edge == result->edge_spans_.end())
        return fail(ErrorCode::unavailable, "send gain edge is absent from mixer plan");
      const auto edge_index = static_cast<std::size_t>(edge - result->edge_spans_.begin());
      if (std::ranges::any_of(result->edge_bindings_, [edge_index](const EdgeBinding& item) {
            return item.edge_index == edge_index;
          }))
        return fail(ErrorCode::conflict, "two automation targets bind the same send edge");
      const auto count = result->edge_bindings_.size() + 1;
      const auto storage_per_frame =
          count * (sizeof(double) + sizeof(float)) +
          result->node_bindings_.size() * (sizeof(double) + sizeof(engine::StereoFrame));
      if (count > project::max_sends_per_project ||
          storage_per_frame > (16ULL * 1024 * 1024) / max_frames)
        return fail(ErrorCode::resource_exhausted, "route gain block storage exceeds budget");
      result->edge_bindings_.push_back(
          {index, edge_index, std::vector<double>(max_frames), std::vector<float>(max_frames)});
    }
    return Result::success(std::move(result));
  } catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted, "send gain preparation ran out of memory");
  }
}

bool PreparedRouteGainBlocks::fill_block(engine::BlockContext context, std::size_t frames,
                                         engine::RoutedGainSpans& gains) noexcept {
  try {
    gains = {};
    if (context.transport_generation == 0 || frames == 0 || frames > ticks_.size() ||
        context.project_sample_position >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - frames)
      return false;
    if (edge_bindings_.empty() && node_bindings_.empty()) {
      gains.edges = edge_spans_;
      return true;
    }
    const auto first = static_cast<std::int64_t>(context.project_sample_position);
    for (std::size_t index = 0; index < frames; ++index) {
      timeline::TickPosition tick{};
      if (!tempo_->sample_to_tick_rt({first + static_cast<std::int64_t>(index)}, sample_rate_,
                                     tick))
        return false;
      ticks_[index] = tick;
    }
    for (auto& binding : edge_bindings_) {
      auto values = std::span(binding.values).first(frames);
      if (!targets_[binding.target_index].evaluate_block({first}, std::span(ticks_).first(frames),
                                                         values))
        return false;
      for (std::size_t index = 0; index < frames; ++index) {
        if (!std::isfinite(values[index]) || values[index] < 0.0 || values[index] > 4.0)
          return false;
        binding.samples[index] = static_cast<float>(values[index]);
      }
      edge_spans_[binding.edge_index].values = std::span(binding.samples).first(frames);
    }
    // A seek or loop restarts the mute glide at the lane's value.
    const double mute_step = 1.0 / (0.005 * static_cast<double>(sample_rate_));
    for (auto& binding : node_bindings_) {
      const auto evaluate = [&](const std::optional<std::size_t>& target,
                                std::vector<double>& storage, double low, double high) {
        if (!target)
          return true;
        auto values = std::span(storage).first(frames);
        if (!targets_[*target].evaluate_block({first}, std::span(ticks_).first(frames), values))
          return false;
        return std::ranges::all_of(values, [low, high](double value) {
          return std::isfinite(value) && value >= low && value <= high;
        });
      };
      if (!evaluate(binding.level_target, binding.level_values, 0.0, 4.0) ||
          !evaluate(binding.pan_target, binding.pan_values, -1.0, 1.0) ||
          !evaluate(binding.mute_target, binding.mute_values, 0.0, 1.0))
        return false;
      const bool restart = binding.mute_generation != context.state_generation();
      binding.mute_generation = context.state_generation();
      for (std::size_t index = 0; index < frames; ++index) {
        const double level = binding.level_target ? binding.level_values[index] : binding.level;
        const double pan = binding.pan_target ? binding.pan_values[index] : binding.pan;
        if (binding.mute_target) {
          const double target = binding.mute_values[index] >= 0.5 ? 0.0 : 1.0;
          if (restart && index == 0)
            binding.mute_gain = target;
          else
            binding.mute_gain += std::clamp(target - binding.mute_gain, -mute_step, mute_step);
        }
        // Single precision, as the mixer's static channel gains are, so an
        // unmuted channel matches its unautomated gain bit for bit.
        const auto gain = static_cast<float>(level);
        const float left = pan > 0.0 ? static_cast<float>(1.0 - pan) : 1.0F;
        const float right = pan < 0.0 ? static_cast<float>(1.0 + pan) : 1.0F;
        const auto mute = static_cast<float>(binding.mute_gain);
        binding.samples[index] = {gain * left * mute, gain * right * mute};
      }
      node_spans_[binding.node_index].values = std::span(binding.samples).first(frames);
    }
    gains.edges = edge_spans_;
    if (!node_bindings_.empty())
      gains.nodes = node_spans_;
    return true;
  } catch (...) {
    gains = {};
    return false;
  }
}

bool PreparedRouteGainBlocks::binds_node_gain(engine::RoutingNodeId node) const noexcept {
  const auto found = std::ranges::find(node_spans_, node, &engine::NodeGainSpan::id);
  if (found == node_spans_.end())
    return true;
  const auto index = static_cast<std::size_t>(found - node_spans_.begin());
  return std::ranges::any_of(
      node_bindings_, [index](const NodeBinding& binding) { return binding.node_index == index; });
}

bool PreparedRouteGainBlocks::binds_edge_gain(engine::RoutingEdgeId edge) const noexcept {
  const auto found = std::ranges::find(edge_spans_, edge, &engine::EdgeGainSpan::id);
  if (found == edge_spans_.end())
    return true;
  const auto index = static_cast<std::size_t>(found - edge_spans_.begin());
  return std::ranges::any_of(
      edge_bindings_, [index](const EdgeBinding& binding) { return binding.edge_index == index; });
}

} // namespace oscillate::routing
