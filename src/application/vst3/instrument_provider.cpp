#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_tempo_plan.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::plugins {
namespace {
using engine::StereoFrame;
using foundation::ErrorCode;
using foundation::Result;

using detail::maximum_live_sample;
using detail::TempoBoundary;

std::atomic<std::uint64_t> next_manual_instance{1};

[[nodiscard]] Result<application::PreparedTrackInsert> fail(ErrorCode code, std::string message) {
  return Result<application::PreparedTrackInsert>::failure({code, std::move(message)});
}

[[nodiscard]] bool matches_tail_qualification(const Vst3InstrumentTailQualification& qualified,
                                              const project::Vst3PluginInstance& saved,
                                              const ScanResult& scanned, std::uint32_t sample_rate,
                                              std::uint32_t max_frames,
                                              std::optional<std::uint32_t> declared_tail) {
  if (!qualified.note_off_release_tested_64_frames ||
      !qualified.note_off_release_tested_128_frames || (max_frames != 64 && max_frames != 128) ||
      sample_rate != 48'000 || qualified.sample_rate != sample_rate || !declared_tail ||
      *declared_tail != qualified.declared_tail_frames || qualified.binary_sha256.empty() ||
      qualified.binary_byte_size == 0 || qualified.binary_sha256 != scanned.binary.sha256 ||
      qualified.binary_sha256 != saved.binary_sha256 ||
      qualified.binary_byte_size != scanned.binary.byte_size ||
      qualified.class_id != saved.class_id || qualified.vendor != saved.vendor ||
      qualified.name != saved.name || qualified.version != saved.version ||
      qualified.component_state != saved.component_state ||
      qualified.controller_state != saved.controller_state ||
      qualified.parameters != saved.parameters)
    return false;
  return std::ranges::any_of(scanned.metadata.classes, [&](const Vst3Class& klass) {
    return klass.id == qualified.class_id && klass.vendor == qualified.vendor &&
           klass.name == qualified.name && klass.version == qualified.version;
  });
}

} // namespace

class Vst3InstrumentRuntime final : public engine::ProcessorRuntime {
public:
  enum class Phase : std::uint8_t { fresh, preparing, prepared, running };
  static_assert(std::atomic<Phase>::is_always_lock_free);
  Vst3InstrumentRuntime(
      std::unique_ptr<LoadedVst3Effect> instrument, engine::InstrumentEvents events,
      timeline::TempoMap tempo, std::vector<TempoBoundary> tempo_boundaries,
      // The format is passed as one validated pair at preparation.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::uint32_t sample_rate, std::uint32_t max_frames, std::vector<foundation::IdBits> note_ids,
      std::vector<automation_project::PreparedTargetAutomation> automation,
      std::uint32_t event_capacity, std::shared_ptr<Vst3ManualEventSlot> manual_events,
      std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters = {})
      : instrument_(std::move(instrument)), events_(std::move(events)), tempo_(std::move(tempo)),
        tempo_boundaries_(std::move(tempo_boundaries)), sample_rate_(sample_rate),
        max_frames_(max_frames), note_ids_(std::move(note_ids)),
        compiled_(engine::max_midi_events_per_block), notes_(engine::max_midi_events_per_block),
        left_(max_frames), right_(max_frames), automation_(std::move(automation)),
        automation_values_(max_frames), event_capacity_(event_capacity),
        manual_events_(std::move(manual_events)), pedal_parameters_(pedal_parameters),
        pedals_(engine::max_pedal_events_per_block + project::midi_pedal_count) {
    has_ramps_ = std::ranges::any_of(tempo_.events(), [](const auto& event) {
      return event.shape_to_next != timeline::TempoShape::step;
    });
    parameter_events_.reserve(event_capacity_);
  }

  ~Vst3InstrumentRuntime() override { manual_events_->retire(); }

  void graph_activated(std::uint64_t revision) noexcept override {
    if (!failed())
      manual_events_->activate(revision);
  }

  void graph_deactivated(std::uint64_t) noexcept override { manual_events_->deactivate(); }

  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override {
    return !failed() && manual_events_->ready_for_control_overlay();
  }

  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept override {
    manual_events_->control_overlay_rebound(from, to);
  }

  // Result::value() follows an explicit success check. The analyzer cannot
  // prove that checked variant access is nonthrowing.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  void process(std::span<StereoFrame> stereo, engine::BlockContext context) noexcept override {
    for (;;) {
      auto phase = phase_.load(std::memory_order_acquire);
      if (phase == Phase::preparing) {
        // A callback that arrives during control-side preparation invalidates
        // that preparation without reading the schedule being replaced.
        if (!phase_.compare_exchange_strong(phase, Phase::running, std::memory_order_acq_rel))
          continue;
        fault(stereo);
        return;
      }
      if (phase == Phase::fresh || phase == Phase::prepared) {
        if (!phase_.compare_exchange_strong(phase, Phase::running, std::memory_order_acq_rel))
          continue;
      }
      break;
    }
    if (failed_.load(std::memory_order_relaxed)) {
      manual_events_->retire();
      silence(stereo);
      return;
    }
    if (stereo.empty() || stereo.size() > max_frames_ || context.transport_generation == 0 ||
        context.project_sample_position > maximum_live_sample - stereo.size()) {
      fault(stereo);
      return;
    }
    const auto manual = manual_events_->claim();
    const auto transient = manual ? std::nullopt : manual_events_->claim_preview();
    bool manual_applied = false;
    std::size_t rendered = 0;
    while (rendered < stereo.size()) {
      const auto position = context.project_sample_position + rendered;
      const auto next =
          std::ranges::upper_bound(tempo_boundaries_, position, {}, &TempoBoundary::sample);
      const auto active = next - 1;
      const auto available = stereo.size() - rendered;
      const auto count =
          next == tempo_boundaries_.end()
              ? available
              : std::min(available, static_cast<std::size_t>(next->sample - position));
      double bpm = active->bpm;
      if (has_ramps_ &&
          !tempo_.bpm_at_sample_rt({static_cast<std::int64_t>(position)}, sample_rate_, bpm)) {
        if (manual)
          manual_events_->finish(false, -1);
        fault(stereo);
        return;
      }
      if (active->sample == position)
        bpm = active->bpm;
      auto selected = rendered == 0 ? (manual ? manual : transient) : std::nullopt;
      if (selected && !manual_events_->qualified_for_block(selected->id, count))
        selected.reset();
      if (selected && manual)
        manual_applied = true;
      if (!render_segment(stereo.subspan(rendered, count), static_cast<std::int64_t>(position), bpm,
                          context.transport_generation, selected)) {
        if (manual)
          manual_events_->finish(false, -1);
        fault(stereo);
        return;
      }
      rendered += count;
    }
    last_position_ = context.project_sample_position + stereo.size();
    last_generation_ = context.transport_generation;
    if (manual)
      manual_events_->finish(manual_applied,
                             static_cast<std::int64_t>(context.project_sample_position));
  }

  [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

  [[nodiscard]] Result<std::uint64_t> prepare_export_end(std::uint64_t end_sample) {
    using MidiEvent = engine::ScheduledMidiEvent;
    if (end_sample == 0 || end_sample > maximum_live_sample)
      return Result<std::uint64_t>::failure(
          {ErrorCode::invalid_argument,
           "VST3 instrument export end is outside the render horizon"});
    auto expected = Phase::fresh;
    if (!phase_.compare_exchange_strong(expected, Phase::preparing, std::memory_order_acq_rel))
      return Result<std::uint64_t>::failure(
          {ErrorCode::unavailable, "VST3 instrument export end needs a fresh prepared runtime"});
    try {
      std::vector<MidiEvent> cut;
      cut.reserve(events_.events().size());
      std::map<project::NoteId, MidiEvent> held;
      for (const auto& event : events_.events()) {
        if (event.sample.value >= static_cast<std::int64_t>(end_sample))
          break;
        cut.push_back(event);
        if (event.kind == engine::MidiEventKind::note_on)
          held.insert_or_assign(event.note, event);
        else
          held.erase(event.note);
      }
      for (const auto& [note, onset] : held) {
        (void)note;
        cut.push_back({{static_cast<std::int64_t>(end_sample)},
                       onset.clip,
                       onset.note,
                       engine::MidiEventKind::note_off,
                       onset.key,
                       0,
                       true});
      }
      std::ranges::sort(cut, [](const MidiEvent& a, const MidiEvent& b) {
        if (a.sample != b.sample)
          return a.sample < b.sample;
        if (a.kind != b.kind)
          return a.kind < b.kind;
        return a.note < b.note;
      });
      // Any callback segment is at most max_frames_ long. This sliding window
      // proves the cutoff cannot overflow the preallocated 1,024-event buffer.
      for (std::size_t index = engine::max_midi_events_per_block; index < cut.size(); ++index) {
        if (cut[index].sample.value - cut[index - engine::max_midi_events_per_block].sample.value <
            max_frames_) {
          auto preparing = Phase::preparing;
          (void)phase_.compare_exchange_strong(preparing, Phase::fresh, std::memory_order_acq_rel);
          return Result<std::uint64_t>::failure(
              {ErrorCode::resource_exhausted, "VST3 export cutoff exceeds MIDI block capacity"});
        }
      }
      events_ =
          engine::InstrumentEvents(events_.track(), std::move(cut),
                                   events_.pedals_until(static_cast<std::int64_t>(end_sample)));
      auto preparing = Phase::preparing;
      if (!phase_.compare_exchange_strong(preparing, Phase::prepared, std::memory_order_acq_rel))
        return Result<std::uint64_t>::failure(
            {ErrorCode::unavailable, "VST3 instrument callback interrupted export preparation"});
      prepared_export_end_ = end_sample;
      return Result<std::uint64_t>::success(end_sample);
    } catch (const std::bad_alloc&) {
      auto preparing = Phase::preparing;
      (void)phase_.compare_exchange_strong(preparing, Phase::fresh, std::memory_order_acq_rel);
      return Result<std::uint64_t>::failure(
          {ErrorCode::resource_exhausted, "VST3 export cutoff allocation failed"});
    }
  }

  [[nodiscard]] std::optional<std::uint64_t> prepared_export_end() const noexcept {
    return prepared_export_end_;
  }

  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] bool stop_after_join() noexcept {
    manual_events_->deactivate();
    if (failed())
      return false;
    const auto tick =
        tempo_.sample_to_tick({static_cast<std::int64_t>(last_position_)}, sample_rate_);
    if (!tick)
      return false;
    const auto next =
        std::ranges::upper_bound(tempo_boundaries_, last_position_, {}, &TempoBoundary::sample);
    double bpm = (next - 1)->bpm;
    if (has_ramps_ &&
        !tempo_.bpm_at_sample_rt({static_cast<std::int64_t>(last_position_)}, sample_rate_, bpm))
      return false;
    if ((next - 1)->sample == last_position_)
      bpm = (next - 1)->bpm;
    Vst3InstrumentTransport transport{.project_sample = static_cast<std::int64_t>(last_position_),
                                      .quarter_position = static_cast<double>(tick.value().value) /
                                                          timeline::ticks_per_quarter,
                                      .tempo_bpm = bpm,
                                      .generation = last_generation_ == 0 ? 1 : last_generation_,
                                      .playing = false};
    const auto result = instrument_->host().process_instrument(
        {std::span(left_).first(1), std::span(right_).first(1)}, {}, transport);
    if (result != Vst3ProcessStatus::ok)
      failed_.store(true, std::memory_order_release);
    return result == Vst3ProcessStatus::ok;
  }

private:
  [[nodiscard]] bool render_segment(std::span<StereoFrame> stereo, std::int64_t position,
                                    double bpm, std::uint64_t generation,
                                    std::optional<Vst3ParameterEvent> manual) noexcept {
    const auto copied =
        events_.copy_block({position}, static_cast<std::uint32_t>(stereo.size()), compiled_);
    timeline::TickPosition tick{};
    if (copied.status != engine::BlockMidiStatus::ready ||
        !tempo_.sample_to_tick_rt({position}, sample_rate_, tick))
      return false;
    for (std::size_t index = 0; index < copied.count; ++index) {
      const auto& event = compiled_[index];
      const auto found = std::ranges::lower_bound(note_ids_, event.note_id_bits);
      if (found == note_ids_.end() || *found != event.note_id_bits)
        return false;
      notes_[index] = {.kind = event.kind == engine::MidiEventKind::note_on ? Vst3NoteKind::on
                                                                            : Vst3NoteKind::off,
                       .sample_offset = event.offset,
                       .channel = 0,
                       .pitch = event.key,
                       .velocity = static_cast<float>(event.velocity) /
                                   static_cast<float>(foundation::max_note_velocity),
                       .note_id = static_cast<std::int32_t>(found - note_ids_.begin() + 1)};
    }
    const auto count = stereo.size();
    const Vst3InstrumentTransport transport{.project_sample = position,
                                            .quarter_position = static_cast<double>(tick.value) /
                                                                timeline::ticks_per_quarter,
                                            .tempo_bpm = bpm,
                                            .generation = generation,
                                            .playing = true};
    parameter_events_.clear();
    for (const auto& lane : automation_) {
      const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
      if (target == nullptr)
        return false;
      if (!lane.evaluate_block({position}, std::span(automation_values_).first(count))) {
        return false;
      }
      const auto id = target->parameter_id;
      for (std::size_t frame = 0; frame < count; ++frame) {
        if (frame != 0 && automation_values_[frame] == automation_values_[frame - 1])
          continue;
        // Preparation proves the budget; never reallocate or drop a point here.
        if (parameter_events_.size() >= event_capacity_)
          return false;
        parameter_events_.push_back(
            {id, static_cast<std::uint32_t>(frame), automation_values_[frame]});
      }
    }
    // Pedals as their mapped controllers' parameters, chased after a seek.
    const auto pedals = events_.copy_pedals({position}, static_cast<std::uint32_t>(count),
                                            pedal_generation_ != generation, pedals_);
    if (pedals.status != engine::BlockMidiStatus::ready)
      return false;
    for (std::size_t index = 0; index < pedals.count; ++index) {
      const auto& pedal = pedals_[index];
      const auto& id = pedal_parameters_[static_cast<std::size_t>(pedal.pedal)];
      if (!id)
        continue;
      if (!place_parameter_event({*id, pedal.offset, static_cast<double>(pedal.value)}))
        return false;
    }
    pedal_generation_ = generation;
    // A manual edit comes last, so it replaces a pedal or automation value
    // for its parameter at its offset rather than sending two.
    if (manual && !place_parameter_event(*manual))
      return false;
    if (instrument_->host().process_instrument(
            {std::span(left_).first(count), std::span(right_).first(count)},
            std::span(notes_).first(copied.count), transport,
            parameter_events_) != Vst3ProcessStatus::ok) {
      return false;
    }
    for (std::size_t frame = 0; frame < count; ++frame)
      stereo[frame] = {left_[frame], right_[frame]};
    return true;
  }
  // Grouped by parameter, offsets ascending, one value per parameter and
  // offset: a later value at the same offset replaces the earlier one.
  // Preparation proves the capacity; false rather than reallocate.
  [[nodiscard]] bool place_parameter_event(const Vst3ParameterEvent& event) noexcept {
    const auto at = std::ranges::lower_bound(
        parameter_events_, std::pair{event.id, event.sample_offset}, {},
        [](const Vst3ParameterEvent& item) { return std::pair{item.id, item.sample_offset}; });
    if (at != parameter_events_.end() && at->id == event.id &&
        at->sample_offset == event.sample_offset) {
      *at = event;
      return true;
    }
    if (parameter_events_.size() >= event_capacity_)
      return false;
    parameter_events_.insert(at, event);
    return true;
  }
  static void silence(std::span<StereoFrame> stereo) noexcept {
    std::ranges::fill(stereo, StereoFrame{});
  }
  void fault(std::span<StereoFrame> stereo) noexcept {
    failed_.store(true, std::memory_order_release);
    manual_events_->retire();
    silence(stereo);
  }

  // Host destruction precedes module unload. Only the callback processes
  // while running; control may call stop_after_join once that callback ends.
  std::unique_ptr<LoadedVst3Effect> instrument_;
  engine::InstrumentEvents events_;
  timeline::TempoMap tempo_;
  std::vector<TempoBoundary> tempo_boundaries_;
  std::uint32_t sample_rate_;
  bool has_ramps_{false};
  std::uint32_t max_frames_;
  std::vector<foundation::IdBits> note_ids_;
  std::vector<engine::BlockMidiEvent> compiled_;
  std::vector<Vst3NoteEvent> notes_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::vector<automation_project::PreparedTargetAutomation> automation_;
  std::vector<double> automation_values_;
  std::uint32_t event_capacity_;
  std::vector<Vst3ParameterEvent> parameter_events_;
  std::shared_ptr<Vst3ManualEventSlot> manual_events_;
  // The parameters the plugin's IMidiMapping assigns to CC64, CC66 and CC67
  // (ADR-0035), indexed by MidiPedal; pedals without one are not sent.
  std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters_;
  std::vector<engine::BlockPedalEvent> pedals_;
  std::uint64_t pedal_generation_{0};
  std::atomic<bool> failed_{false};
  std::atomic<Phase> phase_{Phase::fresh};
  std::optional<std::uint64_t> prepared_export_end_;
  std::uint64_t last_position_ = 0;
  std::uint64_t last_generation_ = 0;
};

Vst3InstrumentTrackProvider::Vst3InstrumentTrackProvider(
    Vst3ProcessingMode mode, bool fixed_latency_verified,
    std::vector<std::uint32_t> qualified_manual_ids)
    : mode_(mode), fixed_latency_verified_(fixed_latency_verified),
      qualified_manual_ids_(std::move(qualified_manual_ids)) {
  std::ranges::sort(qualified_manual_ids_);
  auto unique = std::ranges::unique(qualified_manual_ids_);
  qualified_manual_ids_.erase(unique.begin(), unique.end());
}

void Vst3InstrumentTrackProvider::set_scan_receipt(project::PluginInstanceId id,
                                                   ScanResult scanned) {
  receipts_.insert_or_assign(id, std::move(scanned));
}

void Vst3InstrumentTrackProvider::set_manual_qualification(
    project::PluginInstanceId id, bool fixed_latency_verified,
    std::vector<std::uint32_t> qualified_manual_ids) {
  std::ranges::sort(qualified_manual_ids);
  auto unique = std::ranges::unique(qualified_manual_ids);
  qualified_manual_ids.erase(unique.begin(), unique.end());
  manual_qualifications_.insert_or_assign(
      id,
      ManualQualification{fixed_latency_verified, std::move(qualified_manual_ids), std::nullopt});
}

void Vst3InstrumentTrackProvider::set_size_scoped_manual_qualification(
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

void Vst3InstrumentTrackProvider::set_tail_qualification(
    project::PluginInstanceId id, Vst3InstrumentTailQualification qualification) {
  tail_qualifications_.insert_or_assign(id, std::move(qualification));
}

Result<application::PreparedTrackInsert>
Vst3InstrumentTrackProvider::prepare(const project::Snapshot& snapshot,
                                     const project::Vst3PluginInstance& saved,
                                     std::uint32_t max_frames) {
  if (max_frames == 0 || max_frames > 128)
    return fail(ErrorCode::invalid_argument, "Instrument binding has no matching project format");
  auto tempo_boundaries = detail::compile_vst3_tempo(snapshot.tempo, snapshot.sample_rate);
  if (!tempo_boundaries)
    return fail(tempo_boundaries.error().code, tempo_boundaries.error().message);
  const project::Track* owner = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
    if (!source || source->id != saved.id)
      continue;
    if (owner || *source != saved || track.kind != project::TrackKind::instrument)
      return fail(ErrorCode::conflict, "VST3 instrument saved source identity is ambiguous");
    owner = &track;
  }
  if (!owner)
    return fail(ErrorCode::unsupported, "VST3 instrument is missing from the saved project");
  const auto sample_rate = snapshot.sample_rate;
  const auto receipt = receipts_.find(saved.id);
  if (receipt == receipts_.end() || !receipt->second.ok())
    return fail(ErrorCode::unavailable, "VST3 instrument has no successful current scan receipt");
  const auto& scanned = receipt->second;
  auto plan = engine::compile_midi_events(snapshot);
  if (!plan)
    return fail(plan.error().code, plan.error().message);
  const auto events = std::ranges::find_if(
      plan.value().instruments(), [owner](const auto& item) { return item.track() == owner->id; });
  if (events == plan.value().instruments().end())
    return fail(ErrorCode::corrupt_data, "Compiled VST3 instrument MIDI track is absent");
  std::vector<foundation::IdBits> note_ids;
  note_ids.reserve(events->events().size());
  for (const auto& event : events->events())
    note_ids.push_back(event.note.bits());
  std::ranges::sort(note_ids);
  const auto duplicates = std::ranges::unique(note_ids);
  note_ids.erase(duplicates.begin(), duplicates.end());
  if (note_ids.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - 1))
    return fail(ErrorCode::resource_exhausted, "VST3 instrument note identity space is exhausted");
  auto prepared_automation =
      automation_project::prepare_project_automation(snapshot, snapshot.revision);
  if (!prepared_automation)
    return fail(prepared_automation.error().code, prepared_automation.error().message);
  std::vector<automation_project::PreparedTargetAutomation> instrument_automation;
  for (auto& lane : prepared_automation.value()) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
    if (target != nullptr && target->plugin == saved.id)
      instrument_automation.push_back(std::move(lane));
  }
  // Size the host before loading so admission and preallocation agree.
  // Automation and manual events, plus, when the track's clips have pedals
  // (ADR-0035), every pedal event a block can carry with its chase.
  const auto pedal_budget = events->pedals().empty()
                                ? 0U
                                : static_cast<std::uint32_t>(engine::max_pedal_events_per_block +
                                                             project::midi_pedal_count);
  auto event_budget = vst3_automation_event_budget(instrument_automation.size(), max_frames);
  if (!event_budget || *event_budget > max_vst3_parameter_events - pedal_budget)
    return fail(ErrorCode::resource_exhausted,
                "VST3 instrument automation exceeds the bounded event budget");
  *event_budget += pedal_budget;
  auto binding =
      bind_vst3_project_instrument(saved, scanned, sample_rate, 128, mode_, *event_budget);
  if (!binding.ready()) {
    if (binding.status == Vst3BindingStatus::load_failed)
      return fail(ErrorCode::unavailable,
                  "Verified VST3 instrument could not load: " + binding.diagnostic);
    if (binding.status == Vst3BindingStatus::identity_mismatch) {
      const auto known_class =
          std::ranges::find_if(scanned.metadata.classes, [&saved](const Vst3Class& item) {
            return item.id == saved.class_id && item.vendor == saved.vendor &&
                   item.name == saved.name && item.version == saved.version;
          });
      if (known_class == scanned.metadata.classes.end())
        return fail(ErrorCode::unavailable, "Saved VST3 instrument class differs from scan");
    }
    return fail(ErrorCode::unavailable,
                "Saved VST3 instrument could not bind: " + binding.diagnostic);
  }
  auto loaded = std::move(binding.effect);
  for (const auto& lane : instrument_automation) {
    const auto id = std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id;
    const auto& available = loaded->host().parameters();
    const auto parameter = std::ranges::find(available, id, &Vst3ParameterInfo::id);
    if (lane.track() != owner->id || parameter == available.end() || parameter->step_count != 0 ||
        loaded->host().parameter_precision(id) != Vst3ParameterPrecision::sample_offsets_forwarded)
      return fail(ErrorCode::unavailable, "VST3 instrument automation parameter is unavailable");
  }
  const auto parameter_id = [](const auto& lane) {
    return std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id;
  };
  std::ranges::sort(instrument_automation, {}, parameter_id);
  if (std::ranges::adjacent_find(instrument_automation, {}, parameter_id) !=
      instrument_automation.end())
    return fail(ErrorCode::unavailable, "VST3 instrument automation repeats a parameter");
  const auto latency = loaded->host().latency_samples();
  const auto finite_tail = loaded->host().finite_tail_samples();
  const auto qualified_tail = tail_qualifications_.find(saved.id);
  const auto manual = manual_qualifications_.find(saved.id);
  const bool fixed_latency_verified = manual == manual_qualifications_.end()
                                          ? fixed_latency_verified_
                                          : manual->second.fixed_latency_verified;
  const auto& qualified_manual_ids = manual == manual_qualifications_.end()
                                         ? qualified_manual_ids_
                                         : manual->second.qualified_manual_ids;
  auto qualified_blocks = manual == manual_qualifications_.end()
                              ? std::optional<std::vector<Vst3QualifiedManualBlock>>{}
                              : manual->second.qualified_blocks;
  const bool tail_qualified =
      mode_ == Vst3ProcessingMode::realtime && qualified_tail != tail_qualifications_.end() &&
      instrument_automation.empty() && qualified_manual_ids.empty() && !fixed_latency_verified &&
      matches_tail_qualification(qualified_tail->second, saved, scanned, sample_rate, max_frames,
                                 finite_tail);
  try {
    const auto instance_id = next_manual_instance.fetch_add(1, std::memory_order_relaxed);
    if (instance_id == 0 || instance_id == std::numeric_limits<std::uint64_t>::max())
      return fail(ErrorCode::resource_exhausted, "VST3 manual instance identities exhausted");
    // This token identifies one prepared host lifetime. A replacement gets a
    // new token; a topology-only graph revision retains the old token.
    const auto compatibility_key = instance_id * 0x9e3779b97f4a7c15ULL;
    // The event budget always reserves one manual slot beyond full automation.
    std::vector<std::uint32_t> eligible;
    for (const auto& parameter : loaded->host().parameters()) {
      if (parameter.step_count == 0 && !parameter.read_only &&
          loaded->host().parameter_precision(parameter.id) ==
              Vst3ParameterPrecision::sample_offsets_forwarded)
        eligible.push_back(parameter.id);
    }
    std::ranges::sort(eligible);
    if (!qualified_manual_ids.empty()) {
      std::erase_if(eligible, [&qualified_manual_ids](std::uint32_t id) {
        return !std::ranges::binary_search(qualified_manual_ids, id);
      });
    }
    if (qualified_blocks) {
      auto blocks = *qualified_blocks;
      for (auto& block : blocks) {
        std::ranges::sort(block.parameter_ids);
        auto unique = std::ranges::unique(block.parameter_ids);
        block.parameter_ids.erase(unique.begin(), unique.end());
        std::erase_if(block.parameter_ids, [&eligible](std::uint32_t id) {
          return !std::ranges::binary_search(eligible, id);
        });
      }
      std::erase_if(eligible, [&blocks](std::uint32_t id) {
        return !std::ranges::any_of(blocks, [id](const auto& block) {
          return block.block_frames != 0 && std::ranges::binary_search(block.parameter_ids, id);
        });
      });
      qualified_blocks = std::move(blocks);
    }
    std::vector<std::uint32_t> automated;
    for (const auto& lane : instrument_automation)
      automated.push_back(std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id);
    auto manual_events = std::make_shared<Vst3ManualEventSlot>(
        instance_id, compatibility_key, std::move(eligible), std::move(automated),
        fixed_latency_verified, std::move(qualified_blocks));
    // Pedals reach the plugin through its own MIDI mapping (ADR-0035): the
    // standard controllers sustain (64), sostenuto (66) and soft (67).
    constexpr std::array<std::int16_t, project::midi_pedal_count> pedal_controllers{64, 66, 67};
    std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters{};
    for (std::size_t pedal = 0; pedal < pedal_controllers.size(); ++pedal)
      pedal_parameters[pedal] = loaded->host().map_midi_controller(pedal_controllers[pedal]);
    auto runtime = std::make_shared<Vst3InstrumentRuntime>(
        std::move(loaded), *events, snapshot.tempo, std::move(tempo_boundaries).value(),
        sample_rate, max_frames, std::move(note_ids), std::move(instrument_automation),
        *event_budget, manual_events, pedal_parameters);
    prepared_.insert_or_assign(saved.id,
                               PreparedSource{owner->id, runtime, manual_events,
                                              tail_qualified ? finite_tail : std::nullopt});
    application::PreparedTrackInsert prepared{};
    prepared.processor = runtime;
    prepared.latency_frames = latency;
    prepared.process_failed = [runtime]() noexcept { return runtime->failed(); };
    prepared.instrument_source = true;
    prepared.prepared_export_end = [weak = std::weak_ptr<Vst3InstrumentRuntime>(runtime)] {
      if (const auto bound = weak.lock())
        return bound->prepared_export_end();
      return std::optional<std::uint64_t>{};
    };
    prepared.finite_tail_frames = finite_tail;
    prepared.fixed_tail_qualified = tail_qualified;
    if (fixed_latency_verified) {
      prepared.manual_events = manual_events;
      prepared.active_manual_graph = [slot = prepared.manual_events]() noexcept {
        return slot->active_graph();
      };
    }
    return Result<application::PreparedTrackInsert>::success(std::move(prepared));
  } catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted, "VST3 instrument runtime allocation failed");
  }
}

bool Vst3InstrumentTrackProvider::stop_after_join() noexcept {
  // An instance a live edit already retired has nothing left to stop.
  bool all_stopped = true;
  for (const auto& [id, source] : prepared_) {
    (void)id;
    const auto runtime = source.runtime.lock();
    if (runtime && !runtime->stop_after_join())
      all_stopped = false;
  }
  return all_stopped;
}

bool Vst3InstrumentTrackProvider::stop_after_join(project::TrackId track,
                                                  project::PluginInstanceId plugin) noexcept {
  const auto found = prepared_.find(plugin);
  if (found == prepared_.end() || found->second.track != track)
    return false;
  const auto runtime = found->second.runtime.lock();
  return runtime && runtime->stop_after_join();
}

Result<std::uint64_t> Vst3InstrumentTrackProvider::prepare_export_end(std::uint64_t end_sample) {
  if (prepared_.size() != 1)
    return Result<std::uint64_t>::failure(
        {ErrorCode::unsupported, "VST3 instrument export needs one exact source instance"});
  return prepare_export_end(prepared_.begin()->second.track, prepared_.begin()->first, end_sample);
}

Result<std::uint64_t> Vst3InstrumentTrackProvider::prepare_export_end(
    project::TrackId track, project::PluginInstanceId plugin, std::uint64_t end_sample) {
  const auto found = prepared_.find(plugin);
  if (found == prepared_.end() || found->second.track != track)
    return Result<std::uint64_t>::failure(
        {ErrorCode::unavailable, "VST3 instrument export source is not prepared"});
  const auto runtime = found->second.runtime.lock();
  if (!runtime || !found->second.export_tail_frames)
    return Result<std::uint64_t>::failure(
        {ErrorCode::unavailable, "VST3 instrument export needs qualified immutable release state"});
  if (end_sample > maximum_live_sample - *found->second.export_tail_frames)
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "VST3 instrument release exceeds the render horizon"});
  return runtime->prepare_export_end(end_sample);
}

std::optional<std::uint64_t> Vst3InstrumentTrackProvider::prepared_export_end() const noexcept {
  if (prepared_.size() != 1)
    return std::nullopt;
  return prepared_export_end(prepared_.begin()->second.track, prepared_.begin()->first);
}

std::optional<std::uint64_t>
Vst3InstrumentTrackProvider::prepared_export_end(project::TrackId track,
                                                 project::PluginInstanceId plugin) const noexcept {
  const auto found = prepared_.find(plugin);
  if (found != prepared_.end() && found->second.track == track) {
    const auto runtime = found->second.runtime.lock();
    if (!runtime)
      return std::nullopt;
    return runtime->prepared_export_end();
  }
  return std::nullopt;
}

std::shared_ptr<Vst3ManualEventSlot> Vst3InstrumentTrackProvider::manual_events() const noexcept {
  return prepared_.size() == 1 ? prepared_.begin()->second.manual_events : nullptr;
}

std::shared_ptr<Vst3ManualEventSlot>
Vst3InstrumentTrackProvider::manual_events(project::TrackId track,
                                           project::PluginInstanceId plugin) const noexcept {
  const auto found = prepared_.find(plugin);
  return found == prepared_.end() || found->second.track != track ? nullptr
                                                                  : found->second.manual_events;
}

} // namespace oscillate::plugins
