#include "oscillate/plugins/vst3_graph_node.hpp"
#include "oscillate/plugins/vst3_tempo_plan.hpp"

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
    std::uint64_t instance_id, std::uint64_t compatibility_key,
    std::vector<std::uint32_t> eligible_ids, std::vector<std::uint32_t> automated_ids,
    bool fixed_latency_verified,
    std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks)
    : instance_id_(instance_id), compatibility_key_(compatibility_key),
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

Vst3ManualEventStatus Vst3ManualEventSlot::submit(std::uint64_t graph_revision,
                                                  std::uint64_t instance_id,
                                                  std::uint64_t compatibility_key,
                                                  std::uint64_t revision, Vst3ParameterEvent event,
                                                  std::uint64_t gesture_sequence) noexcept {
  const auto status = admission(graph_revision, instance_id, compatibility_key, revision, event);
  if (status != Vst3ManualEventStatus::accepted)
    return status;
  // One control producer writes payload before publishing the pending phase.
  event_ = event;
  revision_ = revision;
  event_graph_revision_ = graph_revision;
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
  if (active_graph_revision_.fetch_add(0, std::memory_order_acq_rel) != graph_revision) {
    auto expected = Vst3ManualEventPhase::pending;
    if (phase_.compare_exchange_strong(expected, Vst3ManualEventPhase::failed,
                                       std::memory_order_acq_rel) ||
        expected == Vst3ManualEventPhase::failed)
      return Vst3ManualEventStatus::stale_instance;
  }
  return Vst3ManualEventStatus::accepted;
}

Vst3ManualEventStatus Vst3ManualEventSlot::preview(std::uint64_t graph_revision,
                                                   std::uint64_t instance_id,
                                                   std::uint64_t compatibility_key,
                                                   Vst3ParameterEvent event,
                                                   std::uint64_t gesture_sequence) noexcept {
  if (last_submitted_revision_ == std::numeric_limits<std::uint64_t>::max())
    return Vst3ManualEventStatus::invalid_revision;
  const auto status = admission(graph_revision, instance_id, compatibility_key,
                                last_submitted_revision_ + 1, event);
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
  preview_graph_revision_.store(graph_revision, std::memory_order_relaxed);
  preview_word_.store((static_cast<std::uint64_t>(event.id) << 32U) | normalized,
                      std::memory_order_relaxed);
  preview_parameter_id_ = event.id;
  preview_gesture_sequence_.store(gesture_sequence, std::memory_order_relaxed);
  preview_version_.store(++next_preview_version_, std::memory_order_release);
  const auto active = active_graph_revision_.fetch_add(0, std::memory_order_acq_rel);
  const auto first = first_reusable_graph_revision_.load(std::memory_order_acquire);
  if (active == 0 || graph_revision < first || graph_revision > active) {
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

Vst3ManualEventStatus Vst3ManualEventSlot::admission(std::uint64_t graph_revision,
                                                     std::uint64_t instance_id,
                                                     std::uint64_t compatibility_key,
                                                     std::uint64_t revision,
                                                     Vst3ParameterEvent event) const noexcept {
  if (instance_id != instance_id_ || compatibility_key != compatibility_key_)
    return Vst3ManualEventStatus::stale_instance;
  if (graph_revision == 0 ||
      active_graph_revision_.load(std::memory_order_acquire) != graph_revision)
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

class Vst3GraphRuntime final : public engine::SidechainProcessorRuntime {
public:
  Vst3GraphRuntime(std::unique_ptr<LoadedVst3Effect> effect, std::uint32_t max_frames,
                   timeline::TempoMap tempo, std::vector<TempoBoundary> tempo_boundaries,
                   std::uint32_t sample_rate,
                   std::shared_ptr<std::atomic<Vst3GraphRuntimeStatus>> status,
                   std::vector<automation_project::PreparedTargetAutomation> automation,
                   std::uint32_t event_capacity, std::shared_ptr<Vst3ManualEventSlot> manual_events,
                   bool routed_sidechain)
      : effect_(std::move(effect)), input_left_(max_frames), input_right_(max_frames),
        output_left_(max_frames), output_right_(max_frames),
        sidechain_left_(routed_sidechain ? max_frames : 0),
        sidechain_right_(routed_sidechain ? max_frames : 0), tempo_(std::move(tempo)),
        tempo_boundaries_(std::move(tempo_boundaries)), sample_rate_(sample_rate),
        status_(std::move(status)), automation_(std::move(automation)),
        manual_events_(std::move(manual_events)), automation_values_(max_frames),
        event_capacity_(event_capacity), routed_sidechain_(routed_sidechain) {
    has_ramps_ = std::ranges::any_of(tempo_.events(), [](const auto& event) {
      return event.shape_to_next != timeline::TempoShape::step;
    });
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
      if (has_ramps_ &&
          !tempo_.bpm_at_sample_rt({static_cast<std::int64_t>(position)}, sample_rate_, bpm)) {
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
    timeline::TickPosition tick{};
    if (!tempo_.sample_to_tick_rt({position}, sample_rate_, tick))
      return false;
    events_.clear();
    for (const auto& lane : automation_) {
      const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
      if (target == nullptr ||
          !lane.evaluate_block({position}, std::span(automation_values_).first(count)))
        return false;
      const auto id = target->parameter_id;
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
         std::span(output_right_).subspan(offset, count), position,
         static_cast<double>(tick.value) / timeline::ticks_per_quarter, bpm, true,
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
  timeline::TempoMap tempo_;
  std::vector<TempoBoundary> tempo_boundaries_;
  std::uint32_t sample_rate_;
  bool has_ramps_{false};
  std::shared_ptr<std::atomic<Vst3GraphRuntimeStatus>> status_;
  std::vector<automation_project::PreparedTargetAutomation> automation_;
  std::shared_ptr<Vst3ManualEventSlot> manual_events_;
  std::vector<double> automation_values_;
  std::uint32_t event_capacity_;
  std::vector<Vst3ParameterEvent> events_;
  bool routed_sidechain_;
};

struct FactoryState {
  std::unique_ptr<LoadedVst3Effect> effect;
  std::shared_ptr<std::atomic<Vst3GraphRuntimeStatus>> status;
  std::uint32_t max_frames{0};
  timeline::TempoMap tempo;
  std::vector<TempoBoundary> tempo_boundaries;
  std::uint32_t sample_rate{0};
  std::vector<automation_project::PreparedTargetAutomation> automation;
  std::uint32_t event_capacity{0};
  std::shared_ptr<Vst3ManualEventSlot> manual_events;
  bool routed_sidechain{false};
};

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
  auto tempo_boundaries =
      detail::compile_vst3_tempo(tempo, static_cast<std::uint32_t>(sample_rate));
  if (!tempo_boundaries) {
    result.binding_status = Vst3BindingStatus::timing_unsupported;
    result.diagnostic = tempo_boundaries.error().message;
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
      engine_instance_id, key, std::move(eligible), std::move(automated), fixed_latency_verified,
      std::move(qualified_blocks));
  auto factory = std::make_shared<FactoryState>(
      FactoryState{std::move(binding.effect), result.runtime_status, max_block_frames, tempo,
                   std::move(tempo_boundaries).value(), static_cast<std::uint32_t>(sample_rate),
                   std::move(automation), event_capacity, result.manual_events, routed_sidechain});
  result.request = {.instance_id = engine_instance_id,
                    .compatibility_key = key,
                    .create_runtime = [factory]() -> std::unique_ptr<engine::ProcessorRuntime> {
                      if (!factory->effect)
                        return {};
                      return std::make_unique<Vst3GraphRuntime>(
                          std::move(factory->effect), factory->max_frames, factory->tempo,
                          std::move(factory->tempo_boundaries), factory->sample_rate,
                          factory->status, std::move(factory->automation), factory->event_capacity,
                          factory->manual_events, factory->routed_sidechain);
                    }};
  return result;
}

} // namespace oscillate::plugins
