#include "oscillate/routing/route_gain_blocks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace oscillate::routing {
namespace {
using foundation::ErrorCode;
using Result = foundation::Result<std::shared_ptr<PreparedRouteGainBlocks>>;

[[nodiscard]] Result fail(ErrorCode code, const char* message) {
  return Result::failure({code, message});
}

// Per-frame scratch for the given bindings: a node holds its stereo samples
// and a value buffer for each of its three channel controls; an edge holds its
// send-gain and mute lane values and its gain samples. Every admission check
// uses this one cost.
[[nodiscard]] std::size_t storage_per_frame(std::size_t nodes, std::size_t edges) noexcept {
  constexpr std::size_t channel_controls = 3;
  constexpr std::size_t edge_lanes = 2;
  return nodes * (channel_controls * sizeof(double) + sizeof(engine::StereoFrame)) +
         edges * (edge_lanes * sizeof(double) + sizeof(float));
}

// A mute glides to its lane's step over this long, so a toggle never clicks.
constexpr double mute_glide_seconds = 0.005;

[[nodiscard]] double mute_glide_step(std::uint32_t sample_rate) noexcept {
  return 1.0 / (mute_glide_seconds * static_cast<double>(sample_rate));
}

// A saved sidechain send whose target effect is switched off compiles to no
// edge; its gain lane stays saved for when the key returns.
[[nodiscard]] bool dormant_send(const project::Snapshot& snapshot, project::TrackId track,
                                project::SendId send) {
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end())
    return false;
  const auto saved = std::ranges::find(owner->sends, send, &project::SendRoute::id);
  return saved != owner->sends.end() && saved->purpose == project::SendPurpose::sidechain &&
         !live_sidechain(snapshot, *saved);
}

// A sidechain key whose source is silent (silent_key_sources) sounds at zero
// whatever its lane says.
[[nodiscard]] bool silent_key(const project::Snapshot& snapshot,
                              const std::set<project::TrackId>& silent_keys, project::TrackId track,
                              project::SendId send) {
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end())
    return false;
  const auto saved = std::ranges::find(owner->sends, send, &project::SendRoute::id);
  return saved != owner->sends.end() && saved->purpose == project::SendPurpose::sidechain &&
         silent_keys.contains(owner->id);
}
} // namespace

// Binds each send-gain and channel lane to its edge or node, then each key
// from a source under a mute lane to that lane, all within the storage budget.
Result PreparedRouteGainBlocks::prepare(
    const project::Snapshot& snapshot, const ProjectRoutePlan& route,
    std::span<const automation_project::PreparedTargetAutomation> targets, std::size_t max_frames) {
  if (max_frames == 0 || max_frames > max_block_frames || snapshot.revision == 0)
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
    const auto silent_keys = silent_key_sources(snapshot);
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
          if (count > project::max_automation_lanes_per_project ||
              storage_per_frame(count, result->edge_bindings_.size()) >
                  storage_budget_bytes / max_frames)
            return fail(ErrorCode::resource_exhausted, "route gain block storage exceeds budget");
          NodeBinding added;
          added.node_index = node_index;
          added.level = track->level;
          added.pan = track->pan;
          added.mute = {.step = mute_glide_step(snapshot.sample_rate),
                        .gain = track->muted ? 0.0 : 1.0};
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
      if (mapped == route.sends.end() && dormant_send(snapshot, target.track(), send->send))
        continue;
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
      if (count > project::max_sends_per_project ||
          storage_per_frame(result->node_bindings_.size(), count) >
              storage_budget_bytes / max_frames)
        return fail(ErrorCode::resource_exhausted, "route gain block storage exceeds budget");
      const bool silent = silent_key(snapshot, silent_keys, target.track(), send->send);
      result->edge_bindings_.push_back({.target_index = index,
                                        .edge_index = edge_index,
                                        .values = std::vector<double>(max_frames),
                                        .samples = std::vector<float>(max_frames),
                                        .key_gain = silent ? 0.0F : 1.0F});
    }
    // A key from a source under a mute lane is silent wherever the lane mutes
    // its channel (ADR-0094).
    for (const auto& routed : route.sends) {
      const auto owner = std::ranges::find(snapshot.tracks, routed.track, &project::Track::id);
      if (owner == snapshot.tracks.end())
        continue;
      const auto saved = std::ranges::find(owner->sends, routed.send, &project::SendRoute::id);
      const auto mapped =
          std::ranges::find(route.track_nodes, routed.track, &ProjectRouteTrackNode::track);
      const auto planned =
          std::ranges::find(route.latency.edges, routed.edge, &engine::PlannedEdgeDelay::id);
      if (saved == owner->sends.end() || saved->purpose != project::SendPurpose::sidechain ||
          mapped == route.track_nodes.end() || planned == route.latency.edges.end())
        continue;
      const auto node =
          std::ranges::find(result->node_spans_, mapped->node, &engine::NodeGainSpan::id);
      const auto node_index = static_cast<std::size_t>(node - result->node_spans_.begin());
      const auto muting =
          std::ranges::find_if(result->node_bindings_, [&](const NodeBinding& item) {
            return item.node_index == node_index && item.mute_target.has_value();
          });
      if (muting == result->node_bindings_.end())
        continue;
      const auto edge_index = static_cast<std::size_t>(planned - route.latency.edges.begin());
      auto bound = std::ranges::find(result->edge_bindings_, edge_index, &EdgeBinding::edge_index);
      if (bound == result->edge_bindings_.end()) {
        const auto count = result->edge_bindings_.size() + 1;
        if (count > project::max_sends_per_project ||
            storage_per_frame(result->node_bindings_.size(), count) >
                storage_budget_bytes / max_frames)
          return fail(ErrorCode::resource_exhausted, "route gain block storage exceeds budget");
        result->edge_bindings_.push_back(
            {.target_index = std::nullopt,
             .edge_index = edge_index,
             .values = {},
             .samples = std::vector<float>(max_frames),
             .saved_gain = send_edge_gain(silent_keys, *owner, *saved)});
        bound = std::prev(result->edge_bindings_.end());
      }
      bound->mute_target = muting->mute_target;
      bound->mute_delay = planned->compensation_frames;
      bound->mute_values.resize(max_frames);
      bound->mute = {.step = mute_glide_step(snapshot.sample_rate),
                     .gain = owner->muted ? 0.0 : 1.0};
    }
    return Result::success(std::move(result));
  } catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted, "send gain preparation ran out of memory");
  }
}

// Evaluates every bound lane for one block: edges (with any key's mute
// gate) and then channel nodes. False leaves `gains` empty.
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
      auto samples = std::span(binding.samples).first(frames);
      if (binding.target_index) {
        auto values = std::span(binding.values).first(frames);
        if (!targets_[*binding.target_index].evaluate_block(
                {first}, std::span(ticks_).first(frames), values))
          return false;
        for (std::size_t index = 0; index < frames; ++index) {
          if (!std::isfinite(values[index]) || values[index] < 0.0 || values[index] > 4.0)
            return false;
          samples[index] = static_cast<float>(values[index]) * binding.key_gain;
        }
      } else {
        std::ranges::fill(samples, binding.saved_gain);
      }
      if (binding.mute_target && !gate_by_mute(binding, targets_[*binding.mute_target], first,
                                               samples, context.state_generation()))
        return false;
      edge_spans_[binding.edge_index].values = samples;
    }
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
      const bool restart = binding.mute.generation != context.state_generation();
      binding.mute.generation = context.state_generation();
      for (std::size_t index = 0; index < frames; ++index) {
        const double level = binding.level_target ? binding.level_values[index] : binding.level;
        const double pan = binding.pan_target ? binding.pan_values[index] : binding.pan;
        if (binding.mute_target)
          binding.mute.advance(binding.mute_values[index], restart && index == 0);
        // Single precision, as the mixer's static channel gains are, so an
        // unmuted channel matches its unautomated gain bit for bit.
        const auto gain = static_cast<float>(level);
        const float left = pan > 0.0 ? static_cast<float>(1.0 - pan) : 1.0F;
        const float right = pan < 0.0 ? static_cast<float>(1.0 + pan) : 1.0F;
        const auto mute = static_cast<float>(binding.mute.gain);
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

// Moves the gain at most one step toward silence or unity, or straight to
// the lane's value on a restart.
void PreparedRouteGainBlocks::MuteGlide::advance(double lane, bool restart) noexcept {
  const double target = mute_lane_mutes(lane) ? 0.0 : 1.0;
  gain = restart ? target : gain + std::clamp(target - gain, -step, step);
}

// Evaluates the source's mute lane `mute_delay` frames back, glides the
// key's own copy of the mute gain along it and multiplies it into `samples`.
// False when the lane cannot be evaluated or leaves its domain.
bool PreparedRouteGainBlocks::gate_by_mute(
    EdgeBinding& binding, const automation_project::PreparedTargetAutomation& target,
    std::int64_t first, std::span<float> samples, std::uint64_t generation) {
  const auto frames = samples.size();
  auto lane = std::span(binding.mute_values).first(frames);
  // The key heard now left its source `mute_delay` frames earlier; before
  // the project's start the lane holds its first value.
  const auto from = first - static_cast<std::int64_t>(binding.mute_delay);
  const auto lead = static_cast<std::size_t>(
      std::clamp<std::int64_t>(-from, 0, static_cast<std::int64_t>(frames)));
  if (lead > 0) {
    if (!target.evaluate_block({0}, lane.first(1)))
      return false;
    std::ranges::fill(lane.first(lead), lane.front());
  }
  if (lead < frames &&
      !target.evaluate_block({from + static_cast<std::int64_t>(lead)}, lane.subspan(lead)))
    return false;
  const bool restart = binding.mute.generation != generation;
  binding.mute.generation = generation;
  for (std::size_t index = 0; index < frames; ++index) {
    if (!std::isfinite(lane[index]) || lane[index] < 0.0 || lane[index] > 1.0)
      return false;
    binding.mute.advance(lane[index], restart && index == 0);
    samples[index] *= static_cast<float>(binding.mute.gain);
  }
  return true;
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
