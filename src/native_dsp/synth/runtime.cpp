#include "oscillate/native_dsp/synth/runtime.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::synth {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] bool event_before_or_equal(const engine::BlockMidiEvent& previous,
                                         const engine::BlockMidiEvent& current) noexcept {
  if (previous.offset != current.offset) {
    return previous.offset < current.offset;
  }
  if (previous.kind != current.kind) {
    return previous.kind < current.kind; // Offs before ons at the same frame.
  }
  return previous.note_id_bits <= current.note_id_bits;
}

[[nodiscard]] bool valid_pedals(std::span<const engine::BlockPedalEvent> pedals,
                                std::size_t frames) noexcept {
  if (pedals.size() > engine::max_pedal_events_per_block + project::midi_pedal_count) {
    return false;
  }
  for (std::size_t i = 0; i < pedals.size(); ++i) {
    const auto& event = pedals[i];
    if (event.offset >= frames ||
        static_cast<std::size_t>(event.pedal) >= project::midi_pedal_count ||
        !std::isfinite(event.value) || event.value < 0.0F || event.value > 1.0F ||
        (i != 0 && pedals[i - 1].offset > event.offset)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_events(std::span<const engine::BlockMidiEvent> events,
                                std::size_t frames) noexcept {
  if (events.size() > engine::HeldNotes::instrument_capacity) {
    return false;
  }
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& event = events[i];
    if (event.offset >= frames || event.clip_id_bits.is_nil() || event.note_id_bits.is_nil() ||
        event.key > 127 ||
        (event.kind != engine::MidiEventKind::note_on &&
         event.kind != engine::MidiEventKind::note_off) ||
        (event.kind == engine::MidiEventKind::note_on &&
         (event.velocity == 0 || event.velocity > 127)) ||
        (i != 0 && !event_before_or_equal(events[i - 1], event))) {
      return false;
    }
  }
  return true;
}

// The managed media a document plays, by slot: two documents can share one
// prepared runtime only when these match exactly.
[[nodiscard]] bool same_managed_media(const project::NativeSynthDocument& document,
                                      const SynthMedia& media) {
  std::size_t managed = 0;
  for (const auto& slot : document.media) {
    const auto* reference = std::get_if<project::ManagedAudioReference>(&slot.media);
    if (!reference)
      continue;
    ++managed;
    const auto found = std::ranges::find(media, slot.id, &SynthMediaEntry::slot);
    if (found == media.end() || found->sha256 != reference->sha256)
      return false;
  }
  return managed == media.size();
}

} // namespace

Result<std::unique_ptr<NativeSynthRuntime>>
NativeSynthRuntime::create(const SynthEngine& engine, std::span<const double> values,
                           double sample_rate) {
  return create(engine, values, {}, {}, sample_rate);
}

Result<std::unique_ptr<NativeSynthRuntime>>
NativeSynthRuntime::create(const SynthEngine& engine, std::span<const double> values,
                           const project::NativeSynthDocument& document, SynthMedia media,
                           double sample_rate) {
  using R = Result<std::unique_ptr<NativeSynthRuntime>>;
  if (engine.descriptor == nullptr || engine.create_kernel == nullptr ||
      !(sample_rate >= 8'000.0 && sample_rate <= 384'000.0)) {
    return R::failure({ErrorCode::invalid_argument, "incomplete synth engine or bad rate"});
  }
  if (!ParameterControl::valid_values(*engine.descriptor, values)) {
    return R::failure({ErrorCode::invalid_argument, "synth values do not match the engine"});
  }
  if (!same_managed_media(document, media)) {
    return R::failure({ErrorCode::invalid_argument, "synth media do not match the document"});
  }
  std::shared_ptr<const DocumentState> state;
  if (engine.prepare_document != nullptr) {
    auto prepared = engine.prepare_document(document, media, sample_rate, nullptr);
    if (!prepared) {
      return R::failure(prepared.error());
    }
    state = std::move(prepared).value();
  } else if (!document.empty()) {
    return R::failure({ErrorCode::invalid_argument,
                       std::string(engine.descriptor->name) + " has no structured state"});
  }
  if (engine.fetch_content != nullptr) {
    engine.fetch_content(values);
  }
  auto kernel = engine.create_kernel(sample_rate);
  if (!kernel) {
    return R::failure({ErrorCode::internal, "synth engine did not create a kernel"});
  }
  if (state) {
    kernel->adopt_document(*state);
  }
  auto runtime = std::unique_ptr<NativeSynthRuntime>(
      new NativeSynthRuntime(engine, std::move(kernel), values, sample_rate));
  runtime->document_ = document;
  runtime->media_ = std::move(media);
  if (state) {
    runtime->documents_.push_back(std::make_unique<DocumentOffer>(0, std::move(state)));
  }
  return R::success(std::move(runtime));
}

Result<bool> NativeSynthRuntime::offer_document(const project::NativeSynthDocument& document,
                                                std::uint64_t revision) {
  if (engine_.prepare_document == nullptr) {
    if (!document.empty()) {
      return Result<bool>::failure(
          {ErrorCode::invalid_argument,
           std::string(engine_.descriptor->name) + " has no structured state"});
    }
    return Result<bool>::success(true);
  }
  if (!documents_.empty() && revision < documents_.back()->revision) {
    return Result<bool>::failure({ErrorCode::conflict, "synth document revision went backwards"});
  }
  if (!same_managed_media(document, media_)) {
    return Result<bool>::success(false);
  }
  auto prepared =
      engine_.prepare_document(document, media_, sample_rate_,
                               documents_.empty() ? nullptr : documents_.back()->state.get());
  if (!prepared) {
    return Result<bool>::failure(prepared.error());
  }
  // Retire offers the callback has moved past; keep the one it reads.
  // Offers are in revision order and the callback adopts only the newest, so
  // nothing older than its adopted revision is read again.
  const auto applied = document_mailbox_->applied.load(std::memory_order_acquire);
  std::erase_if(documents_, [applied](const auto& offer) { return offer->revision < applied; });
  documents_.push_back(std::make_unique<DocumentOffer>(revision, std::move(prepared).value()));
  document_mailbox_->offered.store(documents_.back().get(), std::memory_order_release);
  document_ = document;
  return Result<bool>::success(true);
}

bool NativeSynthRuntime::offer_lock(std::shared_ptr<const LockState> state) {
  if (!state) {
    return false;
  }
  const auto applied = lock_mailbox_->applied.load(std::memory_order_acquire);
  std::erase_if(locks_, [applied](const auto& offer) { return offer->serial < applied; });
  locks_.push_back(std::make_unique<LockOffer>(++lock_serial_, std::move(state)));
  lock_mailbox_->offered.store(locks_.back().get(), std::memory_order_release);
  return true;
}

void NativeSynthRuntime::adopt_lock() noexcept {
  const auto* offer = lock_mailbox_->offered.exchange(nullptr, std::memory_order_acq_rel);
  if (offer == nullptr) {
    return;
  }
  kernel_->adopt_lock(*offer->state);
  lock_mailbox_->applied.store(offer->serial, std::memory_order_release);
}

namespace {
// A key as one atomic word: 0 for none, else a set bit above the tonic and
// mode, so every key differs from none.
constexpr std::uint16_t key_present = 0x100;
constexpr std::uint16_t key_minor = 0x80;
constexpr std::uint16_t key_tonic = 0x7F;

[[nodiscard]] std::uint16_t pack_key(std::optional<project::MusicalKey> key) noexcept {
  if (!key)
    return 0;
  return static_cast<std::uint16_t>(key_present | (key->tonic & key_tonic) |
                                    (key->mode == project::KeyMode::minor ? key_minor : 0));
}

[[nodiscard]] std::optional<project::MusicalKey> unpack_key(std::uint16_t packed) noexcept {
  if ((packed & key_present) == 0)
    return std::nullopt;
  return project::MusicalKey{static_cast<std::uint8_t>(packed & key_tonic),
                             (packed & key_minor) != 0 ? project::KeyMode::minor
                                                       : project::KeyMode::major};
}
} // namespace

void NativeSynthRuntime::offer_project_key(std::optional<project::MusicalKey> key) noexcept {
  offered_key_.store(pack_key(key), std::memory_order_release);
}

void NativeSynthRuntime::adopt_project_key() noexcept {
  const auto offered = offered_key_.load(std::memory_order_acquire);
  if (offered == adopted_key_)
    return;
  adopted_key_ = offered;
  kernel_->project_key(unpack_key(offered));
}

void NativeSynthRuntime::adopt_document() noexcept {
  const auto* offer = document_mailbox_->offered.exchange(nullptr, std::memory_order_acq_rel);
  if (offer == nullptr) {
    return;
  }
  kernel_->adopt_document(*offer->state);
  document_mailbox_->applied.store(offer->revision, std::memory_order_release);
}

NativeSynthRuntime::NativeSynthRuntime(const SynthEngine& engine,
                                       std::unique_ptr<SynthKernel> kernel,
                                       std::span<const double> values, double sample_rate)
    : engine_(engine), kernel_(std::move(kernel)), count_(values.size()), sample_rate_(sample_rate),
      control_(*engine.descriptor, values, sample_rate),
      trace_instance_(foundation::trace::next_instance()) {}

bool NativeSynthRuntime::offer_values(std::span<const double> values,
                                      std::uint64_t revision) noexcept {
  const bool offered = control_.offer(values, revision);
  if (offered && engine_.fetch_content != nullptr) {
    engine_.fetch_content(values);
  }
  return offered;
}

bool NativeSynthRuntime::offer_live_values(std::span<const double> values) noexcept {
  const bool offered = control_.offer_live(values);
  if (offered && engine_.fetch_content != nullptr) {
    engine_.fetch_content(values);
  }
  return offered;
}

std::uint64_t NativeSynthRuntime::applied_revision() const noexcept {
  return control_.applied_revision();
}

void NativeSynthRuntime::tick(std::uint32_t frames_to_next_tick, std::size_t block_offset,
                              const Transport& transport,
                              std::span<const ParameterLane> lanes) noexcept {
  const auto effective = control_.tick(lanes, block_offset);
  Transport at_tick = transport;
  const double beats = transport.beats_per_frame * static_cast<double>(block_offset);
  at_tick.beat_at_block_start += beats;
  at_tick.bar_at_block_start += beats / transport.beats_per_bar;
  kernel_->control(effective, frames_to_next_tick, at_tick);
}

SynthProcessReport NativeSynthRuntime::process(std::span<engine::StereoFrame> output,
                                               std::span<const engine::BlockMidiEvent> events,
                                               std::uint64_t project_frame,
                                               std::uint64_t transport_generation,
                                               const Transport& transport,
                                               std::span<const ParameterLane> lanes) noexcept {
  return process(output, events, {}, project_frame, transport_generation, transport, lanes);
}

void NativeSynthRuntime::apply_pedal(const engine::BlockPedalEvent& event) noexcept {
  if (engine_.models_pedals) {
    kernel_->pedal(event.pedal, event.value);
    return;
  }
  pedals_->pedal(event.pedal, event.value, [this](const PedalLatch::Note& note) {
    kernel_->note_off({note.clip, note.note, note.key, 0});
  });
}

void NativeSynthRuntime::apply_note(const engine::BlockMidiEvent& event) noexcept {
  NoteEvent note{event.clip_id_bits, event.note_id_bits, event.key, event.velocity, event.frames};
  if (engine_.models_pedals) {
    if (event.kind == engine::MidiEventKind::note_off) {
      kernel_->note_off(note);
    } else {
      kernel_->note_on(note);
    }
    return;
  }
  const PedalLatch::Note held{event.clip_id_bits, event.note_id_bits, event.key};
  if (event.kind == engine::MidiEventKind::note_off) {
    if (pedals_->note_off(held)) {
      kernel_->note_off(note);
    }
    return;
  }
  note.velocity = pedals_->note_on(held, event.velocity, [this](const PedalLatch::Note& earlier) {
    kernel_->note_off({earlier.clip, earlier.note, earlier.key, 0});
  });
  kernel_->note_on(note);
}

SynthProcessReport NativeSynthRuntime::process(std::span<engine::StereoFrame> output,
                                               std::span<const engine::BlockMidiEvent> events,
                                               std::span<const engine::BlockPedalEvent> pedals,
                                               std::uint64_t project_frame,
                                               std::uint64_t transport_generation,
                                               const Transport& transport,
                                               std::span<const ParameterLane> lanes) noexcept {
  // The engine's display name is a string literal, so it names the span;
  // the instance number tells two of the same engine apart.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the name is a string literal.
  OSCILLATE_TRACE_SPAN(engine_.descriptor->name.data(), trace_instance_);
  const engine::DenormalsFlushed flushed;
  SynthProcessReport report;
  if (output.empty() || output.size() > engine::max_midi_block_frames) {
    report.status = SynthProcessStatus::invalid_block;
    return report;
  }
  if (!valid_events(events, output.size()) || !valid_pedals(pedals, output.size())) {
    report.status = SynthProcessStatus::invalid_events;
    return report;
  }
  if (!control_.valid_lanes(lanes, output.size())) {
    report.status = SynthProcessStatus::invalid_lanes;
    return report;
  }
  // Values first: a document is offered before the values of its revision,
  // so seeing those values guarantees the document is visible too.
  control_.adopt();
  adopt_document();
  adopt_lock();
  adopt_project_key();

  bool force_tick = !started_;
  if (started_ && transport_generation != generation_) {
    kernel_->reset();
    pedals_->reset();
    force_tick = true;
  }
  started_ = true;
  generation_ = transport_generation;

  std::size_t next_event = 0;
  std::size_t next_pedal = 0;
  std::size_t frame = 0;
  while (frame < output.size()) {
    const std::uint64_t absolute = project_frame + frame;
    kernel_->locate(absolute);
    const auto phase = static_cast<std::uint32_t>(absolute % control_interval_frames);
    const std::uint32_t to_tick = control_interval_frames - phase;
    if (phase == 0 || force_tick) {
      tick(to_tick, frame, transport, lanes);
      force_tick = false;
    }
    // Pedals before notes: a note struck with the pedal at the same frame
    // is already under it.
    while (next_pedal < pedals.size() && pedals[next_pedal].offset == frame) {
      apply_pedal(pedals[next_pedal++]);
    }
    while (next_event < events.size() && events[next_event].offset == frame) {
      apply_note(events[next_event++]);
    }
    std::size_t end = std::min(output.size(), frame + to_tick);
    if (next_event < events.size()) {
      end = std::min<std::size_t>(end, events[next_event].offset);
    }
    if (next_pedal < pedals.size()) {
      end = std::min<std::size_t>(end, pedals[next_pedal].offset);
    }
    kernel_->render(output.subspan(frame, end - frame));
    frame = end;
  }
  report.kernel = kernel_->status();
  publish_telemetry();
  observed_voices_.store(report.kernel.active_voices, std::memory_order_relaxed);
  observed_steals_.store(report.kernel.voice_steals, std::memory_order_relaxed);
  observed_deferred_.store(report.kernel.deferred_changes, std::memory_order_relaxed);
  return report;
}

void NativeSynthRuntime::publish_telemetry() noexcept {
  telemetry_->publish([this](std::span<float> out) { return kernel_->telemetry(out); });
}

bool NativeSynthRuntime::read_telemetry(std::vector<float>& out) { return telemetry_->read(out); }

} // namespace oscillate::native_dsp::synth
