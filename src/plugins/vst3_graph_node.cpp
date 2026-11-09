#include "oscillate/plugins/vst3_graph_node.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::plugins {
Vst3ManualEventSlot::Vst3ManualEventSlot(
    Vst3InstanceKey instance, std::vector<std::uint32_t> eligible_ids,
    std::vector<std::uint32_t> automated_ids, bool fixed_latency_verified,
    std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks)
    : instance_id_(instance.instance_id), compatibility_key_(instance.compatibility_key),
      eligible_ids_(std::move(eligible_ids)), automated_ids_(std::move(automated_ids)),
      fixed_latency_verified_(fixed_latency_verified),
      qualified_blocks_(std::move(qualified_blocks)) {
  if (!qualified_blocks_)
    return;
  std::ranges::sort(*qualified_blocks_, {}, &Vst3QualifiedManualBlock::block_frames);
  std::vector<Vst3QualifiedManualBlock> merged;
  merged.reserve(std::min<std::size_t>(qualified_blocks_->size(), 128));
  for (auto& block : *qualified_blocks_) {
    if (block.block_frames == 0)
      continue;
    if (!merged.empty() && merged.back().block_frames == block.block_frames) {
      merged.back().parameter_ids.insert(merged.back().parameter_ids.end(),
                                         block.parameter_ids.begin(), block.parameter_ids.end());
    } else {
      merged.push_back(std::move(block));
    }
    if (merged.size() > 128) {
      merged.clear(); // More than 128 distinct process sizes fails closed.
      break;
    }
  }
  for (auto& block : merged) {
    std::ranges::sort(block.parameter_ids);
    auto unique = std::ranges::unique(block.parameter_ids);
    block.parameter_ids.erase(unique.begin(), unique.end());
  }
  qualified_blocks_ = std::move(merged);
}

Vst3ActiveGraphIdentity Vst3ManualEventSlot::active_graph() const noexcept {
  const auto revision = active_graph_revision_.load(std::memory_order_acquire);
  return revision == 0 ? Vst3ActiveGraphIdentity{}
                       : Vst3ActiveGraphIdentity{revision, instance_id_, compatibility_key_};
}

Vst3ManualEventStatus Vst3ManualEventSlot::submit(const Vst3ActiveGraphIdentity& target,
                                                  std::uint64_t revision, Vst3ParameterEvent event,
                                                  std::uint64_t gesture_sequence) noexcept {
  const auto status = admission(target, revision, event);
  if (status != Vst3ManualEventStatus::accepted)
    return status;
  // One control producer writes payload before publishing the pending phase.
  event_ = event;
  revision_ = revision;
  event_graph_revision_ = target.graph_revision;
  last_submitted_revision_ = revision;
  last_submitted_gesture_sequence_ = gesture_sequence;
  // A queued older completion can arrive after the editor has already staged
  // the next drag. Its committed value renders first; preserve only the later
  // tagged preview for the following block. Untagged callers retain the
  // conservative behaviour of discarding every unconsumed preview.
  if (gesture_sequence == 0 || preview_gesture_sequence_.load(std::memory_order_relaxed) == 0 ||
      (preview_parameter_id_ == event.id &&
       preview_gesture_sequence_.load(std::memory_order_relaxed) <= gesture_sequence))
    preview_version_.store(0, std::memory_order_release);
  applied_sample_.store(-1, std::memory_order_relaxed);
  phase_.store(Vst3ManualEventPhase::pending, std::memory_order_release);
  // The audio consumer may have already rendered the later held preview
  // before this queued completion arrives. Re-arm that preview after the
  // older committed event overrides it, without writing consumer state here.
  submitted_manual_revision_.store(revision, std::memory_order_release);
  // The RMW orders this check against callback deactivation: either it sees
  // the new identity, or deactivation follows publication and fails pending.
  if (active_graph_revision_.fetch_add(0, std::memory_order_acq_rel) != target.graph_revision) {
    auto expected = Vst3ManualEventPhase::pending;
    if (phase_.compare_exchange_strong(expected, Vst3ManualEventPhase::failed,
                                       std::memory_order_acq_rel) ||
        expected == Vst3ManualEventPhase::failed)
      return Vst3ManualEventStatus::stale_instance;
  }
  return Vst3ManualEventStatus::accepted;
}

Vst3ManualEventStatus Vst3ManualEventSlot::preview(const Vst3ActiveGraphIdentity& target,
                                                   Vst3ParameterEvent event,
                                                   std::uint64_t gesture_sequence) noexcept {
  if (last_submitted_revision_ == std::numeric_limits<std::uint64_t>::max())
    return Vst3ManualEventStatus::invalid_revision;
  const auto status = admission(target, last_submitted_revision_ + 1, event);
  // A committed value must render first. A later drag may still stage a
  // transient value while that receipt is pending; the callback consumes it
  // only after the committed event has finished successfully.
  const auto phase = phase_.load(std::memory_order_acquire);
  if (status != Vst3ManualEventStatus::accepted &&
      !(status == Vst3ManualEventStatus::busy &&
        (phase == Vst3ManualEventPhase::pending || phase == Vst3ManualEventPhase::processing ||
         phase == Vst3ManualEventPhase::applied)))
    return status;
  if (next_preview_version_ == std::numeric_limits<std::uint64_t>::max())
    return Vst3ManualEventStatus::unavailable;
  constexpr auto scale = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
  const auto normalized = static_cast<std::uint32_t>(std::llround(event.normalized_value * scale));
  preview_graph_revision_.store(target.graph_revision, std::memory_order_relaxed);
  preview_word_.store((static_cast<std::uint64_t>(event.id) << 32U) | normalized,
                      std::memory_order_relaxed);
  preview_parameter_id_ = event.id;
  preview_gesture_sequence_.store(gesture_sequence, std::memory_order_relaxed);
  preview_version_.store(++next_preview_version_, std::memory_order_release);
  const auto active = active_graph_revision_.fetch_add(0, std::memory_order_acq_rel);
  const auto first = first_reusable_graph_revision_.load(std::memory_order_acquire);
  if (active == 0 || target.graph_revision < first || target.graph_revision > active) {
    preview_version_.store(0, std::memory_order_release);
    return Vst3ManualEventStatus::stale_instance;
  }
  if (phase_.load(std::memory_order_acquire) == Vst3ManualEventPhase::failed) {
    if (gesture_sequence == 0 || gesture_sequence <= last_submitted_gesture_sequence_) {
      preview_version_.store(0, std::memory_order_release);
      return Vst3ManualEventStatus::busy;
    }
  }
  return Vst3ManualEventStatus::accepted;
}

Vst3ManualEventStatus Vst3ManualEventSlot::admission(const Vst3ActiveGraphIdentity& target,
                                                     std::uint64_t revision,
                                                     Vst3ParameterEvent event) const noexcept {
  if (target.instance_id != instance_id_ || target.compatibility_key != compatibility_key_)
    return Vst3ManualEventStatus::stale_instance;
  if (target.graph_revision == 0 ||
      active_graph_revision_.load(std::memory_order_acquire) != target.graph_revision)
    return Vst3ManualEventStatus::stale_instance;
  if (revision == 0 || revision <= last_submitted_revision_)
    return Vst3ManualEventStatus::invalid_revision;
  if (!std::isfinite(event.normalized_value) || event.normalized_value < 0.0 ||
      event.normalized_value > 1.0 || event.sample_offset != 0)
    return Vst3ManualEventStatus::invalid_value;
  if (std::ranges::binary_search(automated_ids_, event.id))
    return Vst3ManualEventStatus::automated;
  if (!std::ranges::binary_search(eligible_ids_, event.id))
    return Vst3ManualEventStatus::unavailable;
  if (!fixed_latency_verified_)
    return Vst3ManualEventStatus::latency_unverified;
  if (phase_.load(std::memory_order_acquire) != Vst3ManualEventPhase::empty)
    return Vst3ManualEventStatus::busy;
  return Vst3ManualEventStatus::accepted;
}

Vst3ManualEventReceipt Vst3ManualEventSlot::receipt() const noexcept {
  Vst3ManualEventPhase before;
  Vst3ManualEventPhase after;
  std::int64_t sample;
  do {
    before = phase_.load(std::memory_order_acquire);
    sample = applied_sample_.load(std::memory_order_acquire);
    after = phase_.load(std::memory_order_acquire);
  } while (before != after);
  return {after == Vst3ManualEventPhase::empty ? 0 : revision_, after, sample};
}

std::optional<Vst3ManualEventReceipt> Vst3ManualEventSlot::collect() noexcept {
  const auto current = receipt();
  if (current.phase != Vst3ManualEventPhase::applied &&
      current.phase != Vst3ManualEventPhase::failed)
    return std::nullopt;
  if (current.phase == Vst3ManualEventPhase::failed &&
      (preview_gesture_sequence_.load(std::memory_order_acquire) == 0 ||
       preview_gesture_sequence_.load(std::memory_order_acquire) <=
           last_submitted_gesture_sequence_))
    preview_version_.store(0, std::memory_order_release);
  phase_.store(Vst3ManualEventPhase::empty, std::memory_order_release);
  return current;
}

std::optional<Vst3ParameterEvent> Vst3ManualEventSlot::claim() noexcept {
  if (phase_.load(std::memory_order_acquire) != Vst3ManualEventPhase::pending)
    return std::nullopt;
  if (active_graph_revision_.load(std::memory_order_acquire) != event_graph_revision_) {
    phase_.store(Vst3ManualEventPhase::failed, std::memory_order_release);
    return std::nullopt;
  }
  const auto event = event_;
  phase_.store(Vst3ManualEventPhase::processing, std::memory_order_release);
  return event;
}

std::optional<Vst3ParameterEvent> Vst3ManualEventSlot::claim_preview() noexcept {
  const auto available = [](Vst3ManualEventPhase phase) {
    return phase == Vst3ManualEventPhase::empty || phase == Vst3ManualEventPhase::applied;
  };
  if (!available(phase_.load(std::memory_order_acquire)))
    return std::nullopt;
  const auto manual_revision = submitted_manual_revision_.load(std::memory_order_acquire);
  const auto before = preview_version_.load(std::memory_order_acquire);
  if (before == 0 ||
      (before == consumed_preview_version_ && manual_revision == consumed_manual_revision_) ||
      active_graph_revision_.load(std::memory_order_acquire) == 0)
    return std::nullopt;
  const auto revision = preview_graph_revision_.load(std::memory_order_relaxed);
  const auto word = preview_word_.load(std::memory_order_relaxed);
  const auto first = first_reusable_graph_revision_.load(std::memory_order_acquire);
  const auto active = active_graph_revision_.load(std::memory_order_acquire);
  if (preview_version_.load(std::memory_order_acquire) != before || active == 0 ||
      revision < first || revision > active || !available(phase_.load(std::memory_order_acquire)))
    return std::nullopt;
  released_preview_version_ = consumed_preview_version_;
  released_manual_revision_ = consumed_manual_revision_;
  consumed_preview_version_ = before;
  consumed_manual_revision_ = manual_revision;
  constexpr auto scale = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
  return Vst3ParameterEvent{static_cast<std::uint32_t>(word >> 32U), 0,
                            static_cast<double>(static_cast<std::uint32_t>(word)) / scale};
}

void Vst3ManualEventSlot::release_preview() noexcept {
  consumed_preview_version_ = released_preview_version_;
  consumed_manual_revision_ = released_manual_revision_;
}

bool Vst3ManualEventSlot::qualified_for_block(std::uint32_t parameter_id,
                                              std::size_t block_frames) const noexcept {
  if (!qualified_blocks_)
    return true; // Unscoped whole-binary or synthetic fixture admission.
  if (block_frames > std::numeric_limits<std::uint32_t>::max())
    return false;
  const auto found =
      std::ranges::lower_bound(*qualified_blocks_, static_cast<std::uint32_t>(block_frames), {},
                               &Vst3QualifiedManualBlock::block_frames);
  return found != qualified_blocks_->end() && found->block_frames == block_frames &&
         std::ranges::binary_search(found->parameter_ids, parameter_id);
}

void Vst3ManualEventSlot::finish(bool applied, std::int64_t project_sample) noexcept {
  if (phase_.load(std::memory_order_acquire) != Vst3ManualEventPhase::processing)
    return;
  applied_sample_.store(applied ? project_sample : -1, std::memory_order_release);
  phase_.store(applied ? Vst3ManualEventPhase::applied : Vst3ManualEventPhase::failed,
               std::memory_order_release);
}

void Vst3ManualEventSlot::activate(std::uint64_t graph_revision) noexcept {
  preview_version_.store(0, std::memory_order_release);
  first_reusable_graph_revision_.store(graph_revision, std::memory_order_release);
  active_graph_revision_.store(graph_revision, std::memory_order_release);
}

bool Vst3ManualEventSlot::ready_for_control_overlay() const noexcept {
  return active_graph_revision_.load(std::memory_order_acquire) != 0 &&
         phase_.load(std::memory_order_acquire) == Vst3ManualEventPhase::empty;
}

void Vst3ManualEventSlot::control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept {
  auto expected = from;
  if (from == 0 || to <= from ||
      !active_graph_revision_.compare_exchange_strong(expected, to, std::memory_order_acq_rel)) {
    deactivate(); // Unexpected identity fails closed rather than authorising a stale slot.
    return;
  }
  // A control producer may have submitted after the off-callback preflight.
  // Such an event names the old graph and must not cross this boundary.
  auto pending = Vst3ManualEventPhase::pending;
  (void)phase_.compare_exchange_strong(pending, Vst3ManualEventPhase::failed,
                                       std::memory_order_acq_rel);
  // Untagged previews have no gesture age and cannot safely cross the
  // revision boundary. CAS avoids erasing a newer producer publication.
  const auto version = preview_version_.load(std::memory_order_acquire);
  if (version != 0 && preview_gesture_sequence_.load(std::memory_order_acquire) == 0) {
    auto expected_version = version;
    (void)preview_version_.compare_exchange_strong(expected_version, 0, std::memory_order_acq_rel);
  }
}

void Vst3ManualEventSlot::deactivate() noexcept {
  active_graph_revision_.store(0, std::memory_order_release);
  first_reusable_graph_revision_.store(0, std::memory_order_release);
  preview_version_.store(0, std::memory_order_release);
  auto expected = Vst3ManualEventPhase::pending;
  (void)phase_.compare_exchange_strong(expected, Vst3ManualEventPhase::failed,
                                       std::memory_order_acq_rel);
}

void Vst3ManualEventSlot::retire() noexcept { deactivate(); }

namespace {
using detail::maximum_live_sample;
using detail::TempoBoundary;

class Vst3GraphRuntime final : public engine::SidechainProcessorRuntime {
public:
  explicit Vst3GraphRuntime(Vst3GraphRuntimeInputs inputs)
      : effect_(std::move(inputs.effect)), input_left_(inputs.max_frames),
        input_right_(inputs.max_frames), output_left_(inputs.max_frames),
        output_right_(inputs.max_frames),
        sidechain_left_(inputs.routed_sidechain ? inputs.max_frames : 0),
        sidechain_right_(inputs.routed_sidechain ? inputs.max_frames : 0),
        tempo_boundaries_(std::move(inputs.tempo.boundaries)),
        tempo_clock_(std::move(inputs.tempo.clock)), has_ramps_(inputs.tempo.ramps),
        status_(std::move(inputs.status)), automation_(std::move(inputs.automation)),
        manual_events_(std::move(inputs.manual_events)), automation_values_(inputs.max_frames),
        event_capacity_(inputs.event_capacity), routed_sidechain_(inputs.routed_sidechain) {
    events_.reserve(event_capacity_);
  }
  ~Vst3GraphRuntime() override { manual_events_->retire(); }

  void graph_activated(std::uint64_t revision) noexcept override {
    if (status_->load(std::memory_order_acquire) == Vst3GraphRuntimeStatus::ready)
      manual_events_->activate(revision);
  }

  void graph_deactivated(std::uint64_t) noexcept override { manual_events_->deactivate(); }

  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override {
    return status_->load(std::memory_order_acquire) == Vst3GraphRuntimeStatus::ready &&
           manual_events_->ready_for_control_overlay();
  }

  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept override {
    manual_events_->control_overlay_rebound(from, to);
  }

  // Result::value() follows an explicit success check. The analyzer cannot
  // prove that checked variant access is nonthrowing.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext block) noexcept override {
    render(stereo, {}, block);
  }

  void process_with_sidechain(std::span<engine::StereoFrame> stereo,
                              std::span<const engine::StereoFrame> sidechain,
                              engine::BlockContext block) noexcept override {
    render(stereo, sidechain, block);
  }

private:
  void render(std::span<engine::StereoFrame> stereo, std::span<const engine::StereoFrame> sidechain,
              engine::BlockContext block) noexcept {
    if (status_->load(std::memory_order_relaxed) == Vst3GraphRuntimeStatus::process_failed) {
      manual_events_->retire();
      silence(stereo);
      return;
    }
    if (stereo.empty() || stereo.size() > input_left_.size() ||
        (routed_sidechain_ ? sidechain.size() != stereo.size() : !sidechain.empty()) ||
        block.project_sample_position > maximum_live_sample - stereo.size()) {
      status_->store(Vst3GraphRuntimeStatus::process_failed, std::memory_order_release);
      manual_events_->retire();
      silence(stereo);
      return;
    }
    for (std::size_t frame = 0; frame < stereo.size(); ++frame) {
      input_left_[frame] = stereo[frame].left;
      input_right_[frame] = stereo[frame].right;
      if (routed_sidechain_) {
        sidechain_left_[frame] = sidechain[frame].left;
        sidechain_right_[frame] = sidechain[frame].right;
      }
    }
    const auto manual = manual_events_->claim();
    const auto transient = manual ? std::nullopt : manual_events_->claim_preview();
    bool manual_applied = false;
    std::size_t rendered = 0;
    while (rendered < stereo.size()) {
      const auto position = block.project_sample_position + rendered;
      const auto next =
          std::ranges::upper_bound(tempo_boundaries_, position, {}, &TempoBoundary::sample);
      const auto available = stereo.size() - rendered;
      const auto count =
          next == tempo_boundaries_.end()
              ? available
              : std::min(available, static_cast<std::size_t>(next->sample - position));
      double bpm = (next - 1)->bpm;
      if (has_ramps_ && !tempo_clock_->bpm_at(static_cast<std::int64_t>(position), bpm)) {
        status_->store(Vst3GraphRuntimeStatus::process_failed, std::memory_order_release);
        if (manual)
          manual_events_->finish(false, -1);
        manual_events_->retire();
        silence(stereo);
        return;
      }
      // A tempo event rounded onto this sample takes effect here, including
      // multiple events that collapse to the same sample (last event wins).
      if ((next - 1)->sample == position)
        bpm = (next - 1)->bpm;
      auto selected = rendered == 0 ? (manual ? manual : transient) : std::nullopt;
      if (selected && !manual_events_->qualified_for_block(selected->id, count)) {
        // An unqualified preview stays available for a later block.
        if (!manual)
          manual_events_->release_preview();
        selected.reset();
      }
      if (selected && manual)
        manual_applied = true;
      if (!render_segment(rendered, count, static_cast<std::int64_t>(position), bpm, selected)) {
        status_->store(Vst3GraphRuntimeStatus::process_failed, std::memory_order_release);
        if (manual)
          manual_events_->finish(false, -1);
        manual_events_->retire();
        silence(stereo);
        return;
      }
      rendered += count;
    }
    for (std::size_t frame = 0; frame < stereo.size(); ++frame)
      stereo[frame] = {output_left_[frame], output_right_[frame]};
    if (manual)
      manual_events_->finish(manual_applied,
                             static_cast<std::int64_t>(block.project_sample_position));
  }

  [[nodiscard]] bool render_segment(std::size_t offset, std::size_t count, std::int64_t position,
                                    double bpm, std::optional<Vst3ParameterEvent> manual) noexcept {
    double quarter_notes = 0.0;
    if (!tempo_clock_->quarter_notes_at(position, quarter_notes))
      return false;
    events_.clear();
    for (const auto& lane : automation_) {
      if (!lane.curve->evaluate_block(position, std::span(automation_values_).first(count)))
        return false;
      const auto id = lane.parameter_id;
      for (std::size_t frame = 0; frame < count; ++frame) {
        if (frame != 0 && automation_values_[frame] == automation_values_[frame - 1])
          continue;
        // Preparation proves the budget; never reallocate or drop a point here.
        if (events_.size() >= event_capacity_)
          return false;
        events_.push_back({id, static_cast<std::uint32_t>(frame), automation_values_[frame]});
      }
    }
    if (manual) {
      if (events_.size() >= event_capacity_)
        return false;
      const auto insertion =
          std::ranges::lower_bound(events_, manual->id, {}, &Vst3ParameterEvent::id);
      events_.insert(insertion, *manual);
    }
    const auto process_status = effect_->host().process(
        {std::span(input_left_).subspan(offset, count),
         std::span(input_right_).subspan(offset, count),
         std::span(output_left_).subspan(offset, count),
         std::span(output_right_).subspan(offset, count), position, quarter_notes, bpm, true,
         routed_sidechain_ ? std::span(sidechain_left_).subspan(offset, count)
                           : std::span<const float>{},
         routed_sidechain_ ? std::span(sidechain_right_).subspan(offset, count)
                           : std::span<const float>{}},
        events_);
    if (process_status != Vst3ProcessStatus::ok)
      return false;
    return true;
  }
  static void silence(std::span<engine::StereoFrame> stereo) noexcept {
    std::ranges::fill(stereo, engine::StereoFrame{0.0F, 0.0F});
  }

  // Declaration order keeps the module loaded until the host is destroyed.
  std::unique_ptr<LoadedVst3Effect> effect_;
  std::vector<float> input_left_;
  std::vector<float> input_right_;
  std::vector<float> output_left_;
  std::vector<float> output_right_;
  std::vector<float> sidechain_left_;
  std::vector<float> sidechain_right_;
  std::vector<TempoBoundary> tempo_boundaries_;
  std::shared_ptr<const Vst3TempoClock> tempo_clock_;
  bool has_ramps_{false};
  std::shared_ptr<std::atomic<Vst3GraphRuntimeStatus>> status_;
  std::vector<Vst3AutomationLane> automation_;
  std::shared_ptr<Vst3ManualEventSlot> manual_events_;
  std::vector<double> automation_values_;
  std::uint32_t event_capacity_;
  std::vector<Vst3ParameterEvent> events_;
  bool routed_sidechain_;
};

// The callback trusts these: a runtime is made only from complete inputs.
[[nodiscard]] bool complete(const Vst3GraphRuntimeInputs& inputs) noexcept {
  const auto& tempo = inputs.tempo;
  if (!inputs.effect || !inputs.status || !inputs.manual_events || !tempo.clock ||
      inputs.max_frames == 0 || tempo.boundaries.empty() || tempo.boundaries.front().sample != 0)
    return false;
  if (!std::ranges::all_of(inputs.automation,
                           [](const auto& lane) { return lane.curve != nullptr; }))
    return false;
  // One lane per parameter, in parameter order.
  return std::ranges::adjacent_find(inputs.automation, std::ranges::greater_equal{},
                                    &Vst3AutomationLane::parameter_id) == inputs.automation.end();
}

} // namespace

engine::NodeRequest make_vst3_graph_request(std::uint64_t engine_instance_id,
                                            std::uint64_t compatibility_key,
                                            Vst3GraphRuntimeInputs inputs) {
  auto factory = std::make_shared<Vst3GraphRuntimeInputs>(std::move(inputs));
  return {.instance_id = engine_instance_id,
          .compatibility_key = compatibility_key,
          .create_runtime = [factory]() -> std::unique_ptr<engine::ProcessorRuntime> {
            if (!complete(*factory))
              return {};
            return std::make_unique<Vst3GraphRuntime>(std::move(*factory));
          }};
}

} // namespace oscillate::plugins
