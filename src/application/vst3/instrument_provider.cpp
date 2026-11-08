#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/synth/plan_mailbox.hpp"
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

  // What a saved revision gives the instrument (ADR-0101): its MIDI, tempo
  // and parameter lanes, swapped whole at a block start. Note numbers are the
  // runtime's, stable across plans, so a held note keeps its number.
  struct Plan {
    std::uint64_t revision = 0;
    std::shared_ptr<const engine::InstrumentEvents> events;
    std::shared_ptr<const engine::NoteSpans> spans;
    std::shared_ptr<const timeline::TempoMap> tempo;
    std::vector<TempoBoundary> tempo_boundaries;
    bool has_ramps = false;
    // Note id bits and their numbers, sorted by bits.
    std::vector<std::pair<foundation::IdBits, std::int32_t>> note_numbers;
    std::vector<automation_project::PreparedTargetAutomation> automation;
  };

  Vst3InstrumentRuntime(
      std::unique_ptr<LoadedVst3Effect> instrument, project::TrackId track,
      project::PluginInstanceId plugin,
      // The format is passed as one validated pair at preparation.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::uint32_t sample_rate, std::uint32_t max_frames, std::size_t automation_lanes,
      bool pedal_events, std::uint32_t event_capacity,
      std::shared_ptr<Vst3ManualEventSlot> manual_events,
      std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters = {})
      : instrument_(std::move(instrument)), track_(track), plugin_(plugin),
        sample_rate_(sample_rate), max_frames_(max_frames), automation_lanes_(automation_lanes),
        pedal_events_(pedal_events), compiled_(engine::HeldNotes::block_capacity),
        notes_(engine::HeldNotes::block_capacity), left_(max_frames), right_(max_frames),
        automation_values_(max_frames), event_capacity_(event_capacity),
        manual_events_(std::move(manual_events)), pedal_parameters_(pedal_parameters),
        pedals_(engine::max_pedal_events_per_block + project::midi_pedal_count),
        retired_events_(std::make_shared<const engine::InstrumentEvents>(
            track, std::vector<engine::ScheduledMidiEvent>{})),
        retired_spans_(std::make_shared<const engine::NoteSpans>(*retired_events_)) {
    parameter_events_.reserve(event_capacity_);
  }

  ~Vst3InstrumentRuntime() override { manual_events_->retire(); }

  // Control only: the plan for `snapshot`, checked against what this runtime
  // was prepared for.
  [[nodiscard]] Result<std::unique_ptr<Plan>> make_plan(const project::Snapshot& snapshot) {
    using PlanResult = Result<std::unique_ptr<Plan>>;
    if (snapshot.sample_rate != sample_rate_)
      return PlanResult::failure(
          {ErrorCode::conflict, "VST3 instrument plan does not match its sample rate"});
    auto boundaries = detail::compile_vst3_tempo(snapshot.tempo, snapshot.sample_rate);
    if (!boundaries)
      return PlanResult::failure(boundaries.error());
    auto compiled = engine::compile_midi_events(snapshot);
    if (!compiled)
      return PlanResult::failure(compiled.error());
    const auto events =
        std::ranges::find(compiled.value().instruments(), track_, &engine::InstrumentEvents::track);
    if (events == compiled.value().instruments().end())
      return PlanResult::failure(
          {ErrorCode::corrupt_data, "Compiled VST3 instrument MIDI track is absent"});
    auto prepared = automation_project::prepare_project_automation(snapshot, snapshot.revision);
    if (!prepared)
      return PlanResult::failure(prepared.error());
    try {
      auto plan = std::make_unique<Plan>();
      plan->revision = snapshot.revision;
      plan->events = std::make_shared<const engine::InstrumentEvents>(*events);
      plan->spans = std::make_shared<const engine::NoteSpans>(*events);
      plan->tempo = std::make_shared<const timeline::TempoMap>(snapshot.tempo);
      plan->tempo_boundaries = std::move(boundaries).value();
      plan->has_ramps = std::ranges::any_of(snapshot.tempo.events(), [](const auto& event) {
        return event.shape_to_next != timeline::TempoShape::step;
      });
      for (const auto& event : events->events()) {
        if (note_numbers_.contains(event.note.bits()))
          continue;
        if (next_note_number_ == std::numeric_limits<std::int32_t>::max())
          return PlanResult::failure(
              {ErrorCode::resource_exhausted, "VST3 instrument note identity space is exhausted"});
        note_numbers_.emplace(event.note.bits(), next_note_number_++);
      }
      plan->note_numbers.assign(note_numbers_.begin(), note_numbers_.end());
      for (auto& lane : prepared.value()) {
        const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target());
        if (target != nullptr && target->plugin == plugin_)
          plan->automation.push_back(std::move(lane));
      }
      std::ranges::sort(plan->automation, {}, [](const auto& lane) {
        return std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id;
      });
      if (!pedal_events_ && !events->pedals().empty())
        return PlanResult::failure(
            {ErrorCode::unsupported, "A VST3 instrument's first pedals need a fresh preparation"});
      if (plan->automation.size() > automation_lanes_)
        return PlanResult::failure(
            {ErrorCode::unsupported, "A VST3 instrument's new lanes need a fresh preparation"});
      return PlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
      return PlanResult::failure(
          {ErrorCode::resource_exhausted, "VST3 instrument plan allocation failed"});
    }
  }

  // Control only, before the first callback.
  void install(std::unique_ptr<Plan> plan) {
    const auto revision = plan->revision;
    plans_->install(std::move(plan), revision);
  }

  // Control only: the plan for `snapshot`, for the next block or, with
  // `at_switch`, for the block the graph of the snapshot's revision takes
  // over (graph_activated).
  [[nodiscard]] Result<void> offer(const project::Snapshot& snapshot, bool at_switch) {
    // A switch withdrawn before it took over stages the same revision again.
    if (at_switch ? plans_->stale_for_stage(snapshot.revision) : plans_->stale(snapshot.revision))
      return Result<void>::failure({ErrorCode::conflict, "VST3 instrument plan is stale"});
    if (!plans_->make_room())
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "VST3 instrument plan mailbox is full"});
    auto plan = make_plan(snapshot);
    if (!plan)
      return Result<void>::failure(plan.error());
    if (at_switch)
      plans_->stage(std::move(plan).value(), snapshot.revision);
    else
      plans_->offer(std::move(plan).value(), snapshot.revision);
    return Result<void>::success();
  }

  [[nodiscard]] std::uint64_t applied_revision() const noexcept {
    return plans_->applied_revision();
  }

  void graph_activated(std::uint64_t revision) noexcept override {
    retiring_.store(false, std::memory_order_relaxed);
    plans_->release_staged(revision);
    if (!failed())
      manual_events_->activate(revision);
  }

  // A graph that no longer runs this instrument: it rings out on its drain,
  // so it releases its notes and plays no more (ADR-0101). A graph that keeps
  // it activates it again before its next block.
  void graph_deactivated(std::uint64_t) noexcept override {
    retiring_.store(true, std::memory_order_relaxed);
    manual_events_->deactivate();
  }

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
    auto* plan = plans_->current();
    if (plan == nullptr) {
      fault(stereo);
      return;
    }
    // A retired instrument plays no notes; its first block releases those it held.
    const bool retiring = retiring_.load(std::memory_order_relaxed);
    const auto* events = retiring ? retired_events_.get() : plan->events.get();
    const auto* spans = retiring ? retired_spans_.get() : plan->spans.get();
    const bool restarted = !rendered_ || context.transport_generation != rendered_generation_;
    const auto* replaced = !restarted && rendered_events_ != events ? rendered_events_ : nullptr;
    const auto manual = manual_events_->claim();
    const auto transient = manual ? std::nullopt : manual_events_->claim_preview();
    bool manual_applied = false;
    std::size_t rendered = 0;
    while (rendered < stereo.size()) {
      const auto position = context.project_sample_position + rendered;
      const auto next =
          std::ranges::upper_bound(plan->tempo_boundaries, position, {}, &TempoBoundary::sample);
      const auto active = next - 1;
      const auto available = stereo.size() - rendered;
      const auto count =
          next == plan->tempo_boundaries.end()
              ? available
              : std::min(available, static_cast<std::size_t>(next->sample - position));
      double bpm = active->bpm;
      if (plan->has_ramps && !plan->tempo->bpm_at_sample_rt({static_cast<std::int64_t>(position)},
                                                            sample_rate_, bpm)) {
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
      const Segment segment{*plan, *events, rendered == 0 ? replaced : nullptr,
                            rendered == 0 && replaced != nullptr ? spans : nullptr};
      if (!render_segment(stereo.subspan(rendered, count), segment,
                          static_cast<std::int64_t>(position), bpm, context.transport_generation,
                          selected)) {
        if (manual)
          manual_events_->finish(false, -1);
        fault(stereo);
        return;
      }
      rendered += count;
    }
    rendered_ = true;
    rendered_generation_ = context.transport_generation;
    rendered_events_ = events;
    plans_->applied(plan->revision);
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
    // Before the first callback, the installed plan is the only one.
    auto* plan = plans_->current();
    try {
      const auto& events = *plan->events;
      std::vector<MidiEvent> cut;
      cut.reserve(events.events().size());
      std::map<project::NoteId, MidiEvent> held;
      for (const auto& event : events.events()) {
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
      auto exported = std::make_shared<const engine::InstrumentEvents>(
          events.track(), std::move(cut),
          events.pedals_until(static_cast<std::int64_t>(end_sample)));
      plan->spans = std::make_shared<const engine::NoteSpans>(*exported);
      plan->events = std::move(exported);
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
    const auto* plan = plans_->current();
    if (plan == nullptr)
      return false;
    const auto tick =
        plan->tempo->sample_to_tick({static_cast<std::int64_t>(last_position_)}, sample_rate_);
    if (!tick)
      return false;
    const auto next = std::ranges::upper_bound(plan->tempo_boundaries, last_position_, {},
                                               &TempoBoundary::sample);
    double bpm = (next - 1)->bpm;
    if (plan->has_ramps && !plan->tempo->bpm_at_sample_rt(
                               {static_cast<std::int64_t>(last_position_)}, sample_rate_, bpm))
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
  // One tempo segment's inputs: the plan and the events it plays, and, on
  // the first segment of a new plan's first block, the events and spans the
  // held notes hand over between.
  struct Segment {
    const Plan& plan;
    const engine::InstrumentEvents& events;
    const engine::InstrumentEvents* replaced;
    const engine::NoteSpans* replacement;
  };

  [[nodiscard]] bool render_segment(std::span<StereoFrame> stereo, const Segment& segment,
                                    std::int64_t position, double bpm, std::uint64_t generation,
                                    std::optional<Vst3ParameterEvent> manual) noexcept {
    const auto& plan = segment.plan;
    // Held notes the new plan no longer holds are released at offset 0.
    const auto copied =
        held_->copy_block(segment.events, {position}, static_cast<std::uint32_t>(stereo.size()),
                          generation, compiled_, segment.replacement);
    timeline::TickPosition tick{};
    if (copied.status != engine::BlockMidiStatus::ready ||
        !plan.tempo->sample_to_tick_rt({position}, sample_rate_, tick))
      return false;
    for (std::size_t index = 0; index < copied.count; ++index) {
      const auto& event = compiled_[index];
      const auto found =
          std::ranges::lower_bound(plan.note_numbers, event.note_id_bits, {},
                                   &std::pair<foundation::IdBits, std::int32_t>::first);
      if (found == plan.note_numbers.end() || found->first != event.note_id_bits)
        return false;
      notes_[index] = {.kind = event.kind == engine::MidiEventKind::note_on ? Vst3NoteKind::on
                                                                            : Vst3NoteKind::off,
                       .sample_offset = event.offset,
                       .channel = 0,
                       .pitch = event.key,
                       .velocity = static_cast<float>(event.velocity) /
                                   static_cast<float>(foundation::max_note_velocity),
                       .note_id = found->second};
    }
    const auto count = stereo.size();
    const Vst3InstrumentTransport transport{.project_sample = position,
                                            .quarter_position = static_cast<double>(tick.value) /
                                                                timeline::ticks_per_quarter,
                                            .tempo_bpm = bpm,
                                            .generation = generation,
                                            .playing = true};
    parameter_events_.clear();
    for (const auto& lane : plan.automation) {
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
    // Pedals as their mapped controllers' parameters, chased after a seek;
    // a pedal an edit changed takes its new value at once.
    const auto pedals =
        segment.events.copy_pedals({position}, static_cast<std::uint32_t>(count),
                                   pedal_generation_ != generation, pedals_, segment.replaced);
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
  project::TrackId track_;
  project::PluginInstanceId plugin_;
  std::uint32_t sample_rate_;
  std::uint32_t max_frames_;
  // The lanes the event budget was sized for, and whether it holds pedals.
  std::size_t automation_lanes_;
  bool pedal_events_;
  // Control only: each note's number, assigned once.
  std::map<foundation::IdBits, std::int32_t> note_numbers_;
  std::int32_t next_note_number_ = 1;
  std::unique_ptr<native_dsp::synth::PlanMailbox<Plan>> plans_ =
      std::make_unique<native_dsp::synth::PlanMailbox<Plan>>();
  std::unique_ptr<engine::HeldNotes> held_ = std::make_unique<engine::HeldNotes>();
  // Callback only: the events the last rendered block used.
  const engine::InstrumentEvents* rendered_events_ = nullptr;
  std::uint64_t rendered_generation_ = 0;
  bool rendered_ = false;
  std::vector<engine::BlockMidiEvent> compiled_;
  std::vector<Vst3NoteEvent> notes_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::vector<double> automation_values_;
  std::uint32_t event_capacity_;
  std::vector<Vst3ParameterEvent> parameter_events_;
  std::shared_ptr<Vst3ManualEventSlot> manual_events_;
  // The parameters the plugin's IMidiMapping assigns to CC64, CC66 and CC67
  // (ADR-0035), indexed by MidiPedal; pedals without one are not sent.
  std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters_;
  std::vector<engine::BlockPedalEvent> pedals_;
  std::uint64_t pedal_generation_{0};
  // What a retired instrument plays: nothing.
  std::shared_ptr<const engine::InstrumentEvents> retired_events_;
  std::shared_ptr<const engine::NoteSpans> retired_spans_;
  std::atomic<bool> retiring_{false};
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
    automated.reserve(instrument_automation.size());
    for (const auto& lane : instrument_automation)
      automated.push_back(std::get<project::Vst3ParameterTarget>(lane.target()).parameter_id);
    auto manual_events = std::make_shared<Vst3ManualEventSlot>(
        Vst3InstanceKey{.instance_id = instance_id, .compatibility_key = compatibility_key},
        std::move(eligible), std::move(automated), fixed_latency_verified,
        std::move(qualified_blocks));
    // Pedals reach the plugin through its own MIDI mapping (ADR-0035): the
    // standard controllers sustain (64), sostenuto (66) and soft (67).
    constexpr std::array<std::int16_t, project::midi_pedal_count> pedal_controllers{64, 66, 67};
    std::array<std::optional<std::uint32_t>, project::midi_pedal_count> pedal_parameters{};
    for (std::size_t pedal = 0; pedal < pedal_controllers.size(); ++pedal)
      pedal_parameters[pedal] = loaded->host().map_midi_controller(pedal_controllers[pedal]);
    auto runtime = std::make_shared<Vst3InstrumentRuntime>(
        std::move(loaded), owner->id, saved.id, sample_rate, max_frames,
        instrument_automation.size(), pedal_budget != 0, *event_budget, manual_events,
        pedal_parameters);
    auto first_plan = runtime->make_plan(snapshot);
    if (!first_plan)
      return fail(first_plan.error().code, first_plan.error().message);
    runtime->install(std::move(first_plan).value());
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
    prepared.offer_events = [weak = std::weak_ptr<Vst3InstrumentRuntime>(runtime)](
                                const project::Snapshot& next, bool at_switch) {
      if (const auto bound = weak.lock())
        return bound->offer(next, at_switch);
      return Result<void>::failure({ErrorCode::unavailable, "VST3 instrument has retired"});
    };
    prepared.applied_events_revision = [weak = std::weak_ptr<Vst3InstrumentRuntime>(runtime)] {
      const auto bound = weak.lock();
      return bound ? bound->applied_revision() : std::uint64_t{0};
    };
    prepared.finite_tail_frames = finite_tail;
    prepared.fixed_tail_qualified = tail_qualified;
    if (fixed_latency_verified) {
      prepared.manual_events = std::move(manual_events);
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
