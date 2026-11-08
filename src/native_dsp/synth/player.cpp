#include "oscillate/native_dsp/synth/player.hpp"

#include "oscillate/native_dsp/synth/project_binding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::native_dsp::synth {
namespace {

using foundation::ErrorCode;
using foundation::Result;

Result<NativeSynthPlayer> fail(ErrorCode code, std::string message) {
  return Result<NativeSynthPlayer>::failure({code, std::move(message)});
}

// Frames the track's enabled note effects keep notes sounding after the
// last one ends (echo repeats, a last arp step), at the song's slowest tempo.
std::uint64_t note_tail_frames(const project::Snapshot& snapshot, const project::Track& track) {
  double slowest = 0.0;
  for (const auto& event : snapshot.tempo.events())
    slowest = slowest == 0.0 ? event.bpm : std::min(slowest, event.bpm);
  if (slowest <= 0.0)
    return 0;
  double beats = 0.0;
  for (const auto& insert : track.note_effects) {
    if (!insert.enabled)
      continue;
    if (auto resolved = note_effects::resolve_note_effect(insert); resolved)
      beats += resolved.value().engine->tail_beats(resolved.value().values);
  }
  return static_cast<std::uint64_t>(
      std::ceil(beats * 60.0 / slowest * static_cast<double>(snapshot.sample_rate)));
}

// Kick & Bass Lock (ADR-0072): the notes of every unmuted track playing the
// engine this one follows, when Lock is on at both ends, with each followed
// hit's envelope for its velocity, scaled by its track's level against the
// loudest followed track, so a quiet layer ducks only as much as it sounds.
// Empty when nothing is followed.
Result<std::shared_ptr<const LockState>> compile_lock(const project::Snapshot& snapshot,
                                                      const SynthEngine& engine,
                                                      std::span<const double> values) {
  using R = Result<std::shared_ptr<const LockState>>;
  auto state = std::make_shared<LockState>();
  if (engine.lock_follows.empty() || engine.lock_enabled == nullptr || !engine.lock_enabled(values))
    return R::success(std::move(state));
  struct Followed {
    project::TrackId track;
    double level = 1.0;
    ResolvedNativeInstrument instrument;
  };
  std::vector<Followed> followed;
  for (const auto& track : snapshot.tracks) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (patch == nullptr || track.muted || patch->engine != engine.lock_follows)
      continue;
    auto resolved = resolve_native_instrument(*patch);
    if (!resolved)
      continue; // A patch that cannot play has no notes to follow.
    const auto& other = *resolved.value().engine;
    if (other.lock_enabled != nullptr && other.lock_enabled(resolved.value().values))
      followed.push_back({track.id, track.level, std::move(resolved).value()});
  }
  if (followed.empty())
    return R::success(std::move(state));
  auto plan = engine::compile_midi_events(snapshot);
  if (!plan)
    return R::failure(plan.error());
  const auto rate = static_cast<double>(snapshot.sample_rate);
  const double loudest = std::ranges::max(followed, {}, &Followed::level).level;
  for (const auto& [track, level, instrument] : followed) {
    const double scale = loudest > 0.0 ? level / loudest : 0.0;
    const auto found =
        std::ranges::find(plan.value().instruments(), track, &engine::InstrumentEvents::track);
    if (found == plan.value().instruments().end())
      continue;
    // The track's notes first, paired with their ends; then each hit's
    // envelope, shared by hits alike in velocity, length and tempo.
    const auto first = state->notes.size();
    std::map<std::tuple<foundation::IdBits, foundation::IdBits>, std::vector<std::size_t>> open;
    for (const auto& event : found->events()) {
      const auto at = static_cast<std::uint64_t>(std::max<std::int64_t>(0, event.sample.value));
      const auto id = std::tuple{event.clip.bits(), event.note.bits()};
      if (event.kind == engine::MidiEventKind::note_on) {
        if (event.boundary_crop)
          continue;
        open[id].push_back(state->notes.size());
        state->notes.push_back(
            {at, std::numeric_limits<std::uint64_t>::max(), event.key, event.velocity});
      } else if (auto waiting = open.find(id); waiting != open.end() && !waiting->second.empty()) {
        state->notes[waiting->second.front()].end = at;
        waiting->second.erase(waiting->second.begin());
      }
    }
    if (instrument.engine->lock_envelope == nullptr)
      continue;
    std::map<std::tuple<std::uint8_t, std::uint32_t, double>, std::uint32_t> envelope_of;
    for (auto& note : std::span(state->notes).subspan(first)) {
      const auto frames =
          note.end == std::numeric_limits<std::uint64_t>::max()
              ? 0U
              : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    note.end - note.start, std::numeric_limits<std::uint32_t>::max()));
      double bpm = 0.0;
      if (!snapshot.tempo.bpm_at_sample_rt({static_cast<std::int64_t>(note.start)},
                                           snapshot.sample_rate, bpm))
        bpm = 0.0;
      auto [slot, added] =
          envelope_of.try_emplace(std::tuple{note.velocity, frames, bpm},
                                  static_cast<std::uint32_t>(state->envelopes.size()));
      if (added) {
        auto envelope = instrument.engine->lock_envelope(instrument.values, instrument.document,
                                                         note.velocity, frames, bpm, rate);
        for (auto& value : envelope)
          value = static_cast<float>(value * scale);
        state->envelopes.push_back(std::move(envelope));
      }
      note.envelope = slot->second;
    }
  }
  std::ranges::sort(state->notes, [](const LockNote& a, const LockNote& b) {
    return std::tie(a.start, a.key) < std::tie(b.start, b.key);
  });
  return R::success(std::move(state));
}

} // namespace

Result<void> NativeSynthPlayer::refresh_lock(const project::Snapshot& snapshot) {
  const auto& engine = runtime_->engine();
  if (engine.lock_follows.empty() || retired_)
    return Result<void>::success();
  const auto track = std::ranges::find(snapshot.tracks, track_, &project::Track::id);
  const auto* patch = track != snapshot.tracks.end()
                          ? std::get_if<project::NativeInstrumentPatch>(&track->instrument_source)
                          : nullptr;
  // A replaced instrument rings out with the Lock it had.
  if (patch == nullptr || patch->engine != engine.descriptor->id)
    return Result<void>::success();
  auto resolved = resolve_native_instrument(*patch);
  if (!resolved)
    return Result<void>::failure(resolved.error());
  try {
    auto state = compile_lock(snapshot, engine, resolved.value().values);
    if (!state)
      return Result<void>::failure(state.error());
    (void)runtime_->offer_lock(std::move(state).value());
  } catch (const std::bad_alloc&) {
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "Cannot prepare the synth's Kick & Bass Lock"});
  }
  return Result<void>::success();
}

Result<NativeSynthPlayer>
NativeSynthPlayer::prepare(const project::Snapshot& snapshot, const project::Track& track,
                           const engine::InstrumentEvents& events,
                           std::span<const automation_project::PreparedTargetAutomation> automation,
                           std::size_t max_frames, std::uint64_t last_safe_automation_sample,
                           SynthMedia media) {
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  if (patch == nullptr || track.kind != project::TrackKind::instrument) {
    return fail(ErrorCode::invalid_argument, "Track has no built-in synth");
  }
  if (max_frames == 0 || max_frames > engine::max_midi_block_frames || events.track() != track.id) {
    return fail(ErrorCode::invalid_argument, "Synth player block size or MIDI plan is invalid");
  }
  auto resolved = resolve_native_instrument(*patch);
  if (!resolved) {
    return fail(resolved.error().code, resolved.error().message);
  }
  const SynthEngine& engine = *resolved.value().engine;
  auto runtime =
      NativeSynthRuntime::create(engine, resolved.value().values, resolved.value().document,
                                 std::move(media), snapshot.sample_rate);
  if (!runtime) {
    return fail(runtime.error().code, runtime.error().message);
  }

  NativeSynthPlayer player(track.id);
  player.sample_rate_ = snapshot.sample_rate;
  player.max_frames_ = max_frames;
  player.runtime_ = std::shared_ptr<NativeSynthRuntime>(std::move(runtime).value());
  player.runtime_->offer_project_key(snapshot.key);
  player.values_ = std::move(resolved.value().values);
  player.block_events_.resize(engine::HeldNotes::instrument_capacity);
  player.block_pedals_.resize(engine::max_pedal_events_per_block + project::midi_pedal_count);
  player.tail_frames_ =
      engine.tail_frames(player.values_, static_cast<double>(snapshot.sample_rate));
  if (!track.note_effects.empty()) {
    auto chain = note_effects::NoteEffectChain::create(track.note_effects);
    if (!chain)
      return fail(chain.error().code, chain.error().message);
    player.note_effects_ = std::shared_ptr<note_effects::NoteEffectChain>(std::move(chain).value());
    player.note_scratch_.resize(note_effects::block_event_capacity);
    player.note_ahead_.resize(note_effects::ahead_event_capacity);
    player.tail_frames_ += note_tail_frames(snapshot, track);
  }
  auto plan = player.make_plan(snapshot, track, events, automation, last_safe_automation_sample);
  if (!plan)
    return fail(plan.error().code, plan.error().message);
  player.plans_->install(std::move(plan).value(), snapshot.revision);
  if (auto lock = player.refresh_lock(snapshot); !lock)
    return fail(lock.error().code, lock.error().message);
  return Result<NativeSynthPlayer>::success(std::move(player));
}

Result<std::unique_ptr<NativeSynthPlayer::Plan>> NativeSynthPlayer::make_plan(
    const project::Snapshot& snapshot, const project::Track& track,
    const engine::InstrumentEvents& events,
    std::span<const automation_project::PreparedTargetAutomation> automation,
    std::uint64_t last_safe_automation_sample) const {
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  if (snapshot.revision == 0 || snapshot.sample_rate != sample_rate_ || track.id != track_ ||
      events.track() != track_ || !patch || patch->engine != runtime_->engine().descriptor->id)
    return Result<std::unique_ptr<Plan>>::failure(
        {ErrorCode::conflict, "Synth plan does not match the running source"});
  auto plan = std::make_unique<Plan>();
  plan->revision = snapshot.revision;
  plan->tempo = std::make_shared<const timeline::TempoMap>(snapshot.tempo);
  plan->bars = BarGrid(snapshot.meter);
  plan->events = std::make_shared<const engine::InstrumentEvents>(events);
  plan->spans = std::make_shared<const engine::NoteSpans>(events);
  plan->last_safe_sample = last_safe_automation_sample;
  const auto matches_engine = [&](const project::AutomationLane& lane) {
    const auto* native = std::get_if<project::NativeParameterTarget>(&lane.target);
    if (native == nullptr)
      return true;
    const auto domain = native_automation_domain(runtime_->engine(), native->parameter_id);
    // The lane's range must sit inside the parameter's, so no value it
    // evaluates is out of range when a block is rendered.
    return domain && domain.value().discrete == lane.domain.discrete &&
           domain.value().step_count == lane.domain.step_count &&
           lane.domain.minimum >= domain.value().minimum &&
           lane.domain.maximum <= domain.value().maximum;
  };
  for (const auto& lane : track.automation) {
    if (!matches_engine(lane))
      return Result<std::unique_ptr<Plan>>::failure(
          {ErrorCode::invalid_argument, "Synth automation lane does not match its parameter"});
  }
  for (const auto& midi_clip : track.midi_clips) {
    for (const auto& lane : midi_clip.automation) {
      if (!matches_engine(lane))
        return Result<std::unique_ptr<Plan>>::failure(
            {ErrorCode::invalid_argument, "Synth automation lane does not match its parameter"});
    }
  }
  for (const auto& prepared : automation) {
    const auto* target = std::get_if<project::NativeParameterTarget>(&prepared.target());
    if (!target || target->track != track.id)
      continue;
    if (prepared.snapshot_revision() != snapshot.revision)
      return Result<std::unique_ptr<Plan>>::failure(
          {ErrorCode::conflict, "Synth automation plan revision is stale"});
    auto index = native_parameter_index(runtime_->engine(), target->parameter_id);
    if (!index)
      return Result<std::unique_ptr<Plan>>::failure(index.error());
    plan->evaluators.push_back(
        std::make_shared<const automation_project::PreparedTargetAutomation>(prepared));
    plan->parameters.push_back(static_cast<std::uint32_t>(index.value()));
    plan->values.emplace_back(max_frames_);
  }
  plan->lanes.resize(plan->evaluators.size());
  if (note_effects_) {
    if (!note_effects_->same_layout(track.note_effects))
      return Result<std::unique_ptr<Plan>>::failure(
          {ErrorCode::conflict, "Note effects do not match the running chain"});
    const auto stages = note_effects_->size();
    plan->note_values.reserve(stages);
    for (const auto& insert : track.note_effects) {
      auto resolved = note_effects::resolve_note_effect(insert);
      if (!resolved)
        return Result<std::unique_ptr<Plan>>::failure(resolved.error());
      plan->note_values.push_back(std::move(resolved.value().values));
    }
    // Lanes of note effect parameters (a macro mapped to one, ADR-0059).
    std::vector<std::pair<std::size_t, std::uint32_t>> bindings;
    for (const auto& prepared : automation) {
      const auto* target = std::get_if<project::NativeEffectParameterTarget>(&prepared.target());
      if (!target)
        continue;
      const auto stage =
          std::ranges::find(track.note_effects, target->slot, &project::NoteEffectInsert::id);
      if (stage == track.note_effects.end())
        continue;
      if (prepared.snapshot_revision() != snapshot.revision)
        return Result<std::unique_ptr<Plan>>::failure(
            {ErrorCode::conflict, "Note effect automation plan revision is stale"});
      const auto index = static_cast<std::size_t>(stage - track.note_effects.begin());
      auto parameter = note_effects::note_effect_parameter_index(note_effects_->engine(index),
                                                                 target->parameter_id);
      if (!parameter)
        return Result<std::unique_ptr<Plan>>::failure(parameter.error());
      plan->note_evaluators.push_back(
          std::make_shared<const automation_project::PreparedTargetAutomation>(prepared));
      bindings.emplace_back(index, static_cast<std::uint32_t>(parameter.value()));
    }
    plan->note_lane_values.assign(plan->note_evaluators.size(), std::vector<double>(max_frames_));
    plan->note_lanes.resize(stages);
    for (std::size_t lane = 0; lane < bindings.size(); ++lane)
      plan->note_lanes[bindings[lane].first].push_back(
          {bindings[lane].second, plan->note_lane_values[lane]});
    plan->note_controls.reserve(stages);
    for (std::size_t stage = 0; stage < stages; ++stage)
      plan->note_controls.push_back(
          {track.note_effects[stage].enabled, plan->note_values[stage], plan->note_lanes[stage]});
  }
  return Result<std::unique_ptr<Plan>>::success(std::move(plan));
}

NativeSynthPlayer::PreparedPlan::PreparedPlan(std::unique_ptr<Plan> plan,
                                              std::uint64_t revision) noexcept
    : plan_(std::move(plan)), revision_(revision) {}

NativeSynthPlayer::PreparedPlan::~PreparedPlan() = default;

Result<NativeSynthPlayer::PreparedPlan> NativeSynthPlayer::prepare_plan(
    const project::Snapshot& snapshot, const project::Track& track,
    const engine::InstrumentEvents& events,
    std::span<const automation_project::PreparedTargetAutomation> automation,
    std::uint64_t last_safe_automation_sample) const {
  using Prepared = Result<PreparedPlan>;
  if (plans_->stale(snapshot.revision))
    return Prepared::failure({ErrorCode::conflict, "Synth plan revision is stale"});
  try {
    auto plan = make_plan(snapshot, track, events, automation, last_safe_automation_sample);
    if (!plan)
      return Prepared::failure(plan.error());
    return Prepared::success(PreparedPlan(std::move(plan).value(), snapshot.revision));
  } catch (const std::bad_alloc&) {
    return Prepared::failure({ErrorCode::resource_exhausted, "Cannot prepare a live synth plan"});
  }
}

void NativeSynthPlayer::commit_plan(PreparedPlan plan, const project::Snapshot& snapshot) {
  // has_room() was true, so the mailbox's reserved storage takes the plan
  // without reallocating.
  plans_->offer(std::move(plan.plan_), plan.revision_);
  // Lock follows this track's own switch too; a failure keeps the notes it had.
  (void)refresh_lock(snapshot);
}

bool NativeSynthPlayer::stage_plan(PreparedPlan plan) {
  if (!has_room())
    return false;
  plans_->stage(std::move(plan.plan_), plan.revision_);
  return true;
}

Result<void> NativeSynthPlayer::offer_plan(
    const project::Snapshot& snapshot, const project::Track& track,
    const engine::InstrumentEvents& events,
    std::span<const automation_project::PreparedTargetAutomation> automation,
    std::uint64_t last_safe_automation_sample) {
  auto prepared = prepare_plan(snapshot, track, events, automation, last_safe_automation_sample);
  if (!prepared)
    return Result<void>::failure(prepared.error());
  if (!has_room())
    return Result<void>::failure({ErrorCode::resource_exhausted,
                                  "Live synth plan mailbox is full; retry after the callback"});
  commit_plan(std::move(prepared).value(), snapshot);
  return Result<void>::success();
}

std::uint64_t NativeSynthPlayer::applied_plan_revision() const noexcept {
  return plans_->applied_revision();
}

bool NativeSynthPlayer::automated() const noexcept {
  const auto* plan = plans_->current();
  return plan && !plan->evaluators.empty();
}

Result<void> NativeSynthPlayer::prepare_export_end(std::uint64_t end) {
  if (export_end_)
    return *export_end_ == end ? Result<void>::success()
                               : Result<void>::failure(
                                     {ErrorCode::conflict, "Synth export end is already prepared"});
  if (end == 0 || end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      automated()) {
    return Result<void>::failure(
        {ErrorCode::unsupported,
         "Native synth export needs a finite end and fixed release settings"});
  }
  // Before the first callback, the installed plan is the only one.
  auto* plan = plans_->current();
  if (plan == nullptr || rendered_)
    return Result<void>::failure(
        {ErrorCode::conflict, "Synth export end needs a player that has not rendered"});
  const auto& events = *plan->events;
  using Identity = std::pair<project::ClipId, project::NoteId>;
  std::map<Identity, std::pair<engine::ScheduledMidiEvent, std::size_t>> held;
  std::vector<engine::ScheduledMidiEvent> cut;
  cut.reserve(events.events().size());
  for (const auto& event : events.events()) {
    const auto at = static_cast<std::uint64_t>(event.sample.value);
    if (at > end || (at == end && event.kind == engine::MidiEventKind::note_on))
      break;
    const Identity identity{event.clip, event.note};
    if (event.kind == engine::MidiEventKind::note_on) {
      if (auto found = held.find(identity); found != held.end()) {
        found->second.first = event;
        ++found->second.second;
      } else {
        held.emplace(identity, std::pair{event, std::size_t{1}});
      }
    } else if (const auto found = held.find(identity); found != held.end()) {
      if (--found->second.second == 0)
        held.erase(found);
    }
    cut.push_back(event);
  }
  std::size_t new_events = 0;
  for (const auto& [identity, pending] : held) {
    (void)identity;
    if (pending.second > project::max_midi_events_per_project - new_events)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "Native synth export has too many held notes"});
    new_events += pending.second;
  }
  if (cut.size() > project::max_midi_events_per_project - new_events)
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "Native synth export MIDI plan is too large"});
  for (const auto& [identity, pending] : held) {
    (void)identity;
    for (std::size_t index = 0; index < pending.second; ++index) {
      auto off = pending.first;
      off.sample = {static_cast<std::int64_t>(end)};
      off.kind = engine::MidiEventKind::note_off;
      off.velocity = 0;
      off.boundary_crop = true;
      cut.push_back(off);
    }
  }
  std::ranges::sort(cut, [](const auto& a, const auto& b) {
    if (a.sample != b.sample)
      return a.sample < b.sample;
    if (a.kind != b.kind)
      return a.kind < b.kind;
    return a.note < b.note;
  });
  for (std::size_t i = engine::max_midi_events_per_block; i < cut.size(); ++i) {
    if (cut[i].sample.value - cut[i - engine::max_midi_events_per_block].sample.value <
        engine::max_midi_block_frames)
      return Result<void>::failure({ErrorCode::resource_exhausted,
                                    "Native synth export cut exceeds callback MIDI capacity"});
  }
  auto exported = std::make_shared<const engine::InstrumentEvents>(
      track_, std::move(cut), events.pedals_until(static_cast<std::int64_t>(end)));
  plan->spans = std::make_shared<const engine::NoteSpans>(*exported);
  plan->events = std::move(exported);
  export_end_ = end;
  return Result<void>::success();
}

bool NativeSynthPlayer::render(std::span<engine::StereoFrame> output, std::uint64_t project_frame,
                               std::uint64_t transport_generation,
                               engine::AudioWorkerPool* workers) noexcept {
  const std::size_t frames = output.size();
  if (frames == 0 || frames > max_frames_ ||
      project_frame >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - frames) {
    return false;
  }
  const timeline::SamplePosition first{static_cast<std::int64_t>(project_frame)};
  auto* plan = plans_->current();
  if (!plan)
    return false;
  const auto& events = *plan->events;
  // A seek or loop resets the voices, live ones included.
  const bool restarted = !rendered_ || transport_generation != rendered_generation_;
  // An edit made while playing: this block is the new plan's first, and the
  // voices sounding now hand over to it (released where it has no note).
  const auto* replaced = !restarted && rendered_events_ != &events ? rendered_events_ : nullptr;
  // Notes held across a planned loop wrap are released at the wrap.
  const auto copied =
      held_->copy_block(events, first, static_cast<std::uint32_t>(frames), transport_generation,
                        std::span(block_events_).first(engine::HeldNotes::block_capacity),
                        replaced != nullptr ? plan->spans.get() : nullptr);
  if (copied.status != engine::BlockMidiStatus::ready) {
    return false;
  }
  if (rendered_ && transport_generation != rendered_generation_)
    live_->sounding.fill(0);
  const auto event_count = merge_live_notes(copied.count);
  // After a seek or loop (and on the first block) the runtime starts with
  // every pedal up, so pedals held at the playhead are sent again first:
  // live playback and export chase alike (ADR-0035).
  const auto pedals = events.copy_pedals(first, static_cast<std::uint32_t>(frames), restarted,
                                         block_pedals_, replaced);
  if (pedals.status != engine::BlockMidiStatus::ready) {
    return false;
  }
  if (!plan->evaluators.empty() && (project_frame > plan->last_safe_sample ||
                                    frames - 1 > plan->last_safe_sample - project_frame)) {
    return false;
  }
  for (std::size_t lane = 0; lane < plan->evaluators.size(); ++lane) {
    const auto values = std::span(plan->values[lane]).first(frames);
    if (!plan->evaluators[lane]->evaluate_block(first, values)) {
      return false;
    }
    plan->lanes[lane] = {plan->parameters[lane], values};
  }
  // Note effects turn the block's notes (the project's and live ones) into
  // the notes the synth plays (ADR-0059).
  std::span<const engine::BlockMidiEvent> notes = std::span(block_events_).first(event_count);
  if (note_effects_) {
    if (plan->note_controls.size() != note_effects_->size())
      return false;
    for (std::size_t lane = 0; lane < plan->note_evaluators.size(); ++lane) {
      if (!plan->note_evaluators[lane]->evaluate_block(
              first, std::span(plan->note_lane_values[lane]).first(frames)))
        return false;
    }
    const note_effects::NoteClock clock(*plan->tempo, sample_rate_, project_frame,
                                        static_cast<std::uint32_t>(frames));
    if (!clock.valid())
      return false;
    if (restarted)
      note_effects_->reset();
    const auto ahead = note_effects::copy_ahead(events.events(), clock, note_ahead_);
    notes = note_effects_->process(block_events_, event_count, note_scratch_, clock,
                                   plan->note_controls, std::span(note_ahead_).first(ahead));
  }
  // Musical position for tempo-synced modulation, at the block start.
  SynthTransport transport;
  timeline::TickPosition tick{};
  double bpm = 0.0;
  if (plan->tempo->sample_to_tick_rt(first, sample_rate_, tick) &&
      plan->tempo->bpm_at_sample_rt(first, sample_rate_, bpm) && bpm > 0.0) {
    transport.beat_at_block_start =
        static_cast<double>(tick.value) / static_cast<double>(timeline::ticks_per_quarter);
    transport.beats_per_frame = bpm / 60.0 / static_cast<double>(sample_rate_);
    plan->bars.locate(tick.value, transport.bar_at_block_start, transport.beats_per_bar);
  }
  runtime_->use_workers(workers);
  const auto report =
      runtime_->process(output, notes, std::span(block_pedals_).first(pedals.count), project_frame,
                        transport_generation, transport, plan->lanes);
  if (report.status != SynthProcessStatus::rendered)
    return false;
  commit_live_notes();
  rendered_ = true;
  rendered_generation_ = transport_generation;
  rendered_events_ = &events;
  plans_->applied(plan->revision);
  return true;
}

void NativeSynthPlayer::commit_live_notes() noexcept {
  auto& live = *live_;
  if (!live.staged)
    return;
  live.sounding = live.staged_sounding;
  live.serial = live.staged_serial;
  live.read.store(live.staged_read, std::memory_order_release);
  live.staged = false;
}

bool NativeSynthPlayer::same_note_effects(
    std::span<const project::NoteEffectInsert> inserts) const {
  return note_effects_ ? note_effects_->same_layout(inserts) : inserts.empty();
}

bool NativeSynthPlayer::queue_live_note(std::uint8_t key, std::uint8_t velocity, bool on) noexcept {
  if (key > 127 || (on && (velocity == 0 || velocity > 127)))
    return false;
  auto& live = *live_;
  const auto next = live.write.load(std::memory_order_relaxed);
  if (next - live.read.load(std::memory_order_acquire) >= LiveNotes::capacity)
    return false;
  live.ring[next % LiveNotes::capacity] = {key, velocity, on};
  live.write.store(next + 1, std::memory_order_release);
  return true;
}

std::size_t NativeSynthPlayer::merge_live_notes(std::size_t count) noexcept {
  auto& live = *live_;
  live.staged = false;
  auto next = live.read.load(std::memory_order_relaxed);
  const auto end = live.write.load(std::memory_order_acquire);
  if (next == end)
    return count;
  live.staged_sounding = live.sounding;
  live.staged_serial = live.serial;
  // Live notes sound under their own clip id, so they never pair with the
  // project's notes.
  constexpr foundation::IdBits live_clip{0x4c495645, 1};
  const auto limit = engine::HeldNotes::instrument_capacity;
  const std::size_t first = count;
  const auto emit = [&](std::uint8_t key, std::uint64_t id, bool on, std::uint8_t velocity) {
    auto& event = block_events_[count++];
    event = {};
    event.offset = 0;
    event.clip_id_bits = live_clip;
    event.note_id_bits = {0, id};
    event.kind = on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off;
    event.key = key;
    event.velocity = on ? velocity : 0;
  };
  for (; next != end && count + 2 <= limit; ++next) {
    const auto note = live.ring[next % LiveNotes::capacity];
    auto& sounding = live.staged_sounding[note.key];
    if (sounding != 0) {
      emit(note.key, sounding, false, 0);
      sounding = 0;
    }
    if (note.on) {
      sounding = ++live.staged_serial;
      emit(note.key, sounding, true, note.velocity);
    }
  }
  live.staged_read = next;
  live.staged = true;
  if (count != first)
    std::ranges::sort(std::span(block_events_).first(count),
                      [](const engine::BlockMidiEvent& a, const engine::BlockMidiEvent& b) {
                        return std::tie(a.offset, a.kind, a.note_id_bits) <
                               std::tie(b.offset, b.kind, b.note_id_bits);
                      });
  return count;
}
} // namespace oscillate::native_dsp::synth
