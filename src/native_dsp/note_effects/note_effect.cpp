#include "oscillate/native_dsp/note_effects/note_effect.hpp"

#include "kernel_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace oscillate::native_dsp::note_effects {

extern const NoteEffectEngine arp_engine;
extern const NoteEffectEngine chord_engine;
extern const NoteEffectEngine echo_engine;
extern const NoteEffectEngine random_engine;
extern const NoteEffectEngine scale_engine;
extern const NoteEffectEngine comp_engine;

namespace {

using foundation::ErrorCode;
using foundation::Result;

const std::array<const NoteEffectEngine*, 6> registry{&arp_engine,    &chord_engine, &echo_engine,
                                                      &random_engine, &scale_engine, &comp_engine};

[[nodiscard]] bool tick_of(const timeline::TempoMap& tempo, std::uint32_t rate, std::uint64_t frame,
                           std::int64_t& tick) noexcept {
  if (frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  timeline::TickPosition position{};
  if (!tempo.sample_to_tick_rt({static_cast<std::int64_t>(frame)}, rate, position))
    return false;
  tick = position.value;
  return true;
}

// Chain stages make notes under their own clip ids, one per stage.
constexpr std::uint64_t generated_clip_tag = 0x4e4f'5445'4658'0000ULL; // "NOTEFX"

} // namespace

NoteClock::NoteClock(const timeline::TempoMap& tempo, std::uint32_t sample_rate,
                     std::uint64_t first_frame, std::uint32_t frames) noexcept
    : tempo_(&tempo), sample_rate_(sample_rate), first_frame_(first_frame), frames_(frames) {
  if (frames == 0 || sample_rate == 0)
    return;
  valid_ = tick_of(tempo, sample_rate, first_frame, first_tick_) &&
           tick_of(tempo, sample_rate, first_frame + frames - 1, last_tick_) &&
           (first_frame == 0 || tick_of(tempo, sample_rate, first_frame - 1, previous_tick_));
}

std::int64_t NoteClock::tick_at(std::uint32_t offset) const noexcept {
  if (offset == 0)
    return first_tick_;
  if (offset + 1 >= frames_)
    return last_tick_;
  std::int64_t tick = 0;
  return tick_of(*tempo_, sample_rate_, first_frame_ + offset, tick) ? tick : last_tick_;
}

std::uint32_t NoteClock::offset_of(std::int64_t tick) const noexcept {
  if (tick <= first_tick_)
    return 0;
  if (tick > last_tick_)
    return frames_;
  // The first frame whose tick reaches `tick`: ticks never fall with time.
  std::uint32_t low = 1;
  std::uint32_t high = frames_ - 1;
  while (low < high) {
    const std::uint32_t middle = low + (high - low) / 2;
    if (tick_at(middle) >= tick)
      high = middle;
    else
      low = middle + 1;
  }
  return low;
}

std::size_t copy_ahead(std::span<const engine::ScheduledMidiEvent> events, const NoteClock& clock,
                       std::span<AheadEvent> output) noexcept {
  if (!clock.valid() || output.empty())
    return 0;
  const auto end_frame = clock.first_frame() + clock.frames();
  if (end_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      clock.last_tick() > std::numeric_limits<std::int64_t>::max() - note_lookahead_ticks)
    return 0;
  const auto limit = clock.last_tick() + note_lookahead_ticks;
  const auto first = std::ranges::lower_bound(
      events, static_cast<std::int64_t>(end_frame), std::less<>{},
      [](const engine::ScheduledMidiEvent& event) { return event.sample.value; });
  std::size_t count = 0;
  // Every event from the block's end frame on comes after the block's last
  // frame, whose tick is last_tick().
  for (auto event = first; event != events.end() && count < output.size(); ++event) {
    std::int64_t previous = clock.last_tick();
    if (static_cast<std::uint64_t>(event->sample.value) > end_frame &&
        !tick_of(clock.tempo(), clock.sample_rate(),
                 static_cast<std::uint64_t>(event->sample.value) - 1, previous))
      break;
    if (previous >= limit)
      break;
    output[count++] = {previous, event->clip.bits(), event->note.bits(), event->kind, event->key};
  }
  return count;
}

double NoteParameters::at(std::size_t parameter, std::uint32_t offset) const noexcept {
  for (const auto& lane : lanes_) {
    if (lane.parameter == parameter && offset < lane.values.size())
      return lane.values[offset];
  }
  return parameter < values_.size() ? values_[parameter] : 0.0;
}

int NoteParameters::index_at(std::size_t parameter, std::uint32_t offset) const noexcept {
  return static_cast<int>(std::lround(at(parameter, offset)));
}

std::span<const NoteEffectEngine* const> engines() noexcept { return registry; }

const NoteEffectEngine* find_engine(std::string_view id) noexcept {
  for (const auto* engine : registry) {
    if (engine->descriptor->id == id)
      return engine;
  }
  return nullptr;
}

Result<ResolvedNoteEffect>
resolve_note_effect(std::string_view id, std::uint32_t engine_version,
                    std::span<const project::NativeInstrumentParameter> parameters) {
  using R = Result<ResolvedNoteEffect>;
  const auto* engine = find_engine(id);
  if (engine == nullptr)
    return R::failure({ErrorCode::unavailable,
                       "Note effect '" + std::string(id) + "' is not available in this version"});
  if (engine_version > engine->descriptor->version)
    return R::failure({ErrorCode::unsupported, "This " + std::string(engine->descriptor->name) +
                                                   " was saved by a newer version"});
  if (engine_version < engine->descriptor->version)
    return R::failure({ErrorCode::unsupported, "No upgrade path for this note effect version"});
  std::vector<synth::NamedValue> saved;
  saved.reserve(parameters.size());
  for (const auto& parameter : parameters)
    saved.push_back({parameter.id, parameter.value});
  auto values = synth::resolve_values(*engine->descriptor, saved);
  if (!values)
    return R::failure(values.error());
  return R::success({engine, std::move(values).value()});
}

Result<ResolvedNoteEffect> resolve_note_effect(const project::NoteEffectInsert& insert) {
  return resolve_note_effect(insert.engine, insert.engine_version, insert.parameters);
}

std::vector<project::NativeInstrumentParameter>
sparse_note_effect_parameters(const NoteEffectEngine& engine, std::span<const double> values) {
  std::vector<project::NativeInstrumentParameter> parameters;
  for (auto& entry : synth::sparse_values(*engine.descriptor, values))
    parameters.push_back({std::move(entry.id), entry.value});
  return parameters;
}

Result<std::size_t> note_effect_parameter_index(const NoteEffectEngine& engine,
                                                std::string_view parameter_id) {
  const auto index = synth::find_parameter(*engine.descriptor, parameter_id);
  if (!index)
    return Result<std::size_t>::failure(
        {ErrorCode::not_found, "Unknown " + std::string(engine.descriptor->name) + " parameter '" +
                                   std::string(parameter_id) + "'"});
  return Result<std::size_t>::success(*index);
}

Result<project::AutomationDomain> note_effect_automation_domain(const NoteEffectEngine& engine,
                                                                std::string_view parameter_id) {
  using R = Result<project::AutomationDomain>;
  const auto index = note_effect_parameter_index(engine, parameter_id);
  if (!index)
    return R::failure(index.error());
  const auto& parameter = engine.descriptor->parameters[index.value()];
  // Stepped and choice parameters automate in whole steps (ADR-0034).
  const bool discrete = synth::is_discrete(parameter);
  const auto steps =
      discrete ? static_cast<std::uint32_t>(parameter.maximum - parameter.minimum) : 0U;
  return R::success({project::AutomationRepresentation::physical, std::string(parameter.id),
                     std::string(parameter.unit), parameter.minimum, parameter.maximum,
                     parameter.default_value, discrete, steps, project::AutomationScale::linear});
}

Result<std::unique_ptr<NoteEffectChain>>
NoteEffectChain::create(std::span<const project::NoteEffectInsert> inserts) {
  using R = Result<std::unique_ptr<NoteEffectChain>>;
  auto chain = std::make_unique<NoteEffectChain>();
  chain->stages_.reserve(inserts.size());
  for (std::size_t index = 0; index < inserts.size(); ++index) {
    auto resolved = resolve_note_effect(inserts[index]);
    if (!resolved)
      return R::failure(resolved.error());
    auto kernel = resolved.value().engine->create_kernel({generated_clip_tag | (index + 1), 1});
    if (!kernel)
      return R::failure({ErrorCode::resource_exhausted, "Cannot prepare a note effect"});
    chain->stages_.push_back({inserts[index].id, resolved.value().engine, std::move(kernel)});
  }
  return R::success(std::move(chain));
}

bool NoteEffectChain::same_layout(
    std::span<const project::NoteEffectInsert> inserts) const noexcept {
  if (inserts.size() != stages_.size())
    return false;
  for (std::size_t index = 0; index < inserts.size(); ++index) {
    if (inserts[index].id != stages_[index].slot ||
        inserts[index].engine != stages_[index].engine->descriptor->id ||
        inserts[index].engine_version != stages_[index].engine->descriptor->version)
      return false;
  }
  return true;
}

std::span<const engine::BlockMidiEvent>
NoteEffectChain::process(std::span<engine::BlockMidiEvent> events, std::size_t count,
                         std::span<engine::BlockMidiEvent> scratch, const NoteClock& clock,
                         std::span<const StageControls> controls,
                         std::span<const AheadEvent> ahead) noexcept {
  if (!clock.valid() || controls.size() != stages_.size() || events.size() < block_event_capacity ||
      scratch.size() < block_event_capacity || count > block_event_capacity)
    return {};
  std::span<engine::BlockMidiEvent> current = events;
  std::span<engine::BlockMidiEvent> next = scratch;
  for (std::size_t index = 0; index < stages_.size(); ++index) {
    auto& stage = stages_[index];
    const auto& control = controls[index];
    if (!control.enabled) {
      if (!stage.enabled)
        continue;
      // Switched off: release what it made, then pass notes through.
      stage.enabled = false;
      const auto room = block_event_capacity - count;
      count += stage.kernel->flush(current.subspan(count, room));
      std::sort(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(count),
                support::event_less);
      continue;
    }
    stage.enabled = true;
    const NoteParameters parameters(control.values, control.lanes, ahead);
    count = stage.kernel->process(current.first(count), next.first(block_event_capacity), clock,
                                  parameters);
    std::swap(current, next);
  }
  return current.first(count);
}

void NoteEffectChain::reset() noexcept {
  for (auto& stage : stages_)
    stage.kernel->reset();
}

} // namespace oscillate::native_dsp::note_effects
