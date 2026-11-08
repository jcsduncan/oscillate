#include "routed_playback.hpp"
#include "saved_processing.hpp"

#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/project/frozen_view.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/application/vst3/stage_provider.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

namespace oscillate::quick {
namespace {
// OSCILLATE_AUDIO_WORKERS overrides the machine default; 0 renders on the
// device callback alone (ADR-0048).
std::size_t audio_worker_count() noexcept {
  const char* text = std::getenv("OSCILLATE_AUDIO_WORKERS");
  std::size_t value = 0;
  if (text != nullptr) {
    const auto* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec == std::errc{} && parsed.ptr == end)
      return std::min(value, engine::AudioWorkerPool::max_workers);
  }
  return engine::AudioWorkerPool::default_worker_count();
}

class PipeWirePlaybackOutput final : public application::PlaybackOutput {
public:
  PipeWirePlaybackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor)
      : output_(source, cursor) {}

  [[nodiscard]] foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t latency,
        std::optional<std::uint64_t> target_serial) override {
    return output_.start(rate, latency, target_serial);
  }
  void stop() noexcept override { output_.stop(); }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override { return output_.status(); }

private:
  audio_io::PipeWireOutput output_;
};

[[nodiscard]] foundation::Result<std::unique_ptr<RoutedPlayback>> fail(foundation::Error error) {
  return foundation::Result<std::unique_ptr<RoutedPlayback>>::failure(std::move(error));
}
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
[[nodiscard]] std::filesystem::path vst3_helper_path() {
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error)
    return {};
  return executable.parent_path() / "oscillate_vst3_scan_helper";
}

[[nodiscard]] std::vector<plugins::Vst3QualifiedManualBlock>
qualified_manual_blocks(const Vst3LiveQualification& qualification,
                        const project::Vst3PluginInstance& saved, std::uint32_t sample_rate) {
  std::vector<plugins::Vst3QualifiedManualBlock> blocks;
  for (const auto& item : qualification.parameters) {
    if (item.binary_sha256 != saved.binary_sha256 || item.class_id != saved.class_id ||
        item.sample_rate != sample_rate || item.block_frames == 0)
      continue;
    auto found = std::ranges::find(blocks, item.block_frames,
                                   &plugins::Vst3QualifiedManualBlock::block_frames);
    if (found == blocks.end()) {
      blocks.push_back({item.block_frames, {item.parameter_id}});
    } else {
      found->parameter_ids.push_back(item.parameter_id);
    }
  }
  return blocks;
}

// Every saved plugin a snapshot runs: VST3 instruments and enabled effects,
// on tracks and the master.
[[nodiscard]] std::vector<std::pair<project::Vst3PluginInstance, bool>>
running_vst3(const project::Snapshot& snapshot) {
  std::vector<std::pair<project::Vst3PluginInstance, bool>> result;
  const auto effects = [&](std::span<const project::EffectInsert> slots) {
    for (const auto& slot : slots) {
      if (const auto* saved = detail::vst3_effect(slot); saved && slot.enabled)
        result.emplace_back(*saved, false);
    }
  };
  for (const auto& track : snapshot.tracks) {
    if (const auto* source = detail::vst3_instrument_source(track))
      result.emplace_back(*source, true);
    effects(track.effects);
  }
  effects(snapshot.master_effects);
  return result;
}

// Scans one saved plugin out of process. Any thread.
[[nodiscard]] foundation::Result<plugins::ScanResult>
scan_vst3(const project::Vst3PluginInstance& saved) {
  using Result = foundation::Result<plugins::ScanResult>;
  plugins::Vst3Scanner scanner(vst3_helper_path());
  auto scanned = scanner.scan(saved.bundle_path);
  if (!scanned.ok())
    return Result::failure(
        {foundation::ErrorCode::unavailable, "Saved VST3 plugin " + saved.id.to_string() +
                                                 " could not be scanned: " + scanned.diagnostic});
  if (scanned.binary.sha256 != saved.binary_sha256)
    return Result::failure(
        {foundation::ErrorCode::unavailable,
         "Saved VST3 binary changed since the project was saved: " + saved.id.to_string()});
  return Result::success(std::move(scanned));
}
#endif
} // namespace

bool has_saved_vst3_insert(const project::Snapshot& snapshot) noexcept {
  for (const auto& track : snapshot.tracks) {
    if (detail::has_saved_vst3(track))
      return true;
  }
  for (const auto& slot : snapshot.master_effects) {
    if (detail::vst3_effect(slot))
      return true;
  }
  return false;
}

bool has_saved_track_effect(const project::Snapshot& snapshot) noexcept {
  return std::ranges::any_of(snapshot.tracks,
                             [](const project::Track& track) { return !track.effects.empty(); });
}

class RoutedPlayback::StoppedGate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
// Adopts a saved manual revision into this run's mirror and control. Every
// call runs on the control sequencer and refuses once the run has retired.
class RoutedPlayback::ManualAdoption final : public application::Vst3ManualLiveAdoption {
public:
  explicit ManualAdoption(RoutedPlayback& run) : run_(run) {}
  [[nodiscard]] foundation::Result<void>
  preflight(project::PluginInstanceId plugin, plugins::Vst3ActiveGraphIdentity expected) override {
    if (!run_.control_ || !run_.model_ || run_.paused() || run_.edit_pending() ||
        run_.queued_revision_ || run_.source_capture_)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::conflict, "routed playback has an unsettled edit"});
    run_.control_->observe();
    return run_.control_->preflight_vst3_manual_edit(plugin, expected);
  }
  [[nodiscard]] foundation::Result<void> begin(const project::Snapshot& saved,
                                               project::PluginInstanceId plugin,
                                               plugins::Vst3ActiveGraphIdentity expected) override {
    if (!run_.control_ || !run_.model_)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "routed playback has stopped"});
    auto adopted = run_.model_->adopt_authoritative_snapshot(saved);
    if (!adopted)
      return foundation::Result<void>::failure(adopted.error());
    return run_.control_->begin_vst3_manual_edit(adopted.value(), "Quick VST3 parameter", plugin,
                                                 expected);
  }
  [[nodiscard]] foundation::Result<void> settle(plugins::Vst3ManualEventReceipt receipt) override {
    if (!run_.control_)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "routed playback has stopped"});
    return run_.control_->settle_vst3_manual_edit(receipt);
  }

private:
  RoutedPlayback& run_;
};

const char* vst3_manual_refusal(plugins::Vst3ManualEventStatus status) noexcept {
  switch (status) {
  case plugins::Vst3ManualEventStatus::busy:
    return "the previous live value has no callback receipt yet";
  case plugins::Vst3ManualEventStatus::stale_instance:
    return "the active plugin instance or graph changed";
  case plugins::Vst3ManualEventStatus::invalid_revision:
    return "the project revision is not newer than the last live value";
  case plugins::Vst3ManualEventStatus::invalid_value:
    return "the normalized value is invalid";
  case plugins::Vst3ManualEventStatus::automated:
    return "an automation lane owns this parameter";
  case plugins::Vst3ManualEventStatus::unavailable:
    return "this parameter has no verified live control";
  case plugins::Vst3ManualEventStatus::latency_unverified:
    return "the plugin has no fixed-latency qualification for live editing";
  case plugins::Vst3ManualEventStatus::accepted:
    break;
  }
  return "the live control is unavailable";
}
#endif

RoutedPlayback::RoutedPlayback(application::ControlTransportGate::PlaybackToken token,
                               std::unique_ptr<StoppedGate> gate,
                               std::uint64_t first_sample) noexcept
    : token_(std::move(token)), gate_(std::move(gate)), cursor_(first_sample) {}

RoutedPlayback::~RoutedPlayback() { stop_and_retire(); }

std::optional<bool> RoutedPlayback::synth_deferred_changes(project::TrackId track) const noexcept {
  if (!source_ || !control_)
    return std::nullopt;
  const auto* runtime = source_->synth_runtime(track);
  if (!runtime)
    return std::nullopt;
  return runtime->observed_status().deferred_changes;
}

bool RoutedPlayback::synth_telemetry(project::TrackId track, std::vector<float>& frame) {
  if (!source_ || !control_)
    return false;
  auto* runtime = source_->synth_runtime(track);
  return runtime && runtime->read_telemetry(frame);
}

foundation::Result<std::unique_ptr<RoutedPlayback>> RoutedPlayback::prepare_and_start(
    const std::filesystem::path& root, project::Snapshot snapshot,
    application::ControlTransportGate::PlaybackToken token,
    application::PlaybackOutputFactory output_factory, std::optional<std::uint64_t> target_serial,
    std::uint32_t requested_buffer_frames, std::uint64_t first_sample,
    Vst3LiveQualification qualification, std::shared_ptr<audio_io::Metronome> metronome,
    std::shared_ptr<RoutedSourceFill> source_fill) {
  auto prepared = prepare_run(root, std::move(snapshot), std::move(token), requested_buffer_frames,
                              first_sample, std::move(qualification));
  if (!prepared)
    return prepared;
  auto run = std::move(prepared).value();
  run->target_serial_ = target_serial;
  // Device playback clicks; export never attaches one.
  run->metronome_ = std::move(metronome);
  run->cursor_.set_metronome(run->metronome_.get());
  if (source_fill)
    run->source_fill_ = std::move(source_fill);
  if (!output_factory) {
    output_factory = [](audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor) {
      return std::make_unique<PipeWirePlaybackOutput>(source, cursor);
    };
  }
  run->output_ = output_factory(run->render_source(), run->cursor_);
  if (!run->output_)
    return fail({foundation::ErrorCode::internal, "Routed output factory returned no device"});
  auto started = run->output_->start(run->sample_rate(), requested_buffer_frames, target_serial);
  if (!started)
    return fail(started.error());
  return foundation::Result<std::unique_ptr<RoutedPlayback>>::success(std::move(run));
}

foundation::Result<std::unique_ptr<RoutedPlayback>>
RoutedPlayback::prepare_for_export(const std::filesystem::path& root, project::Snapshot snapshot,
                                   application::ControlTransportGate::PlaybackToken token,
                                   std::uint32_t requested_buffer_frames,
                                   Vst3LiveQualification qualification) {
  return prepare_run(root, std::move(snapshot), std::move(token), requested_buffer_frames, 0,
                     std::move(qualification));
}

foundation::Result<std::unique_ptr<RoutedPlayback>>
RoutedPlayback::prepare_run(const std::filesystem::path& root, project::Snapshot snapshot,
                            application::ControlTransportGate::PlaybackToken token,
                            std::uint32_t requested_buffer_frames, std::uint64_t first_sample,
                            Vst3LiveQualification qualification) {
  struct ReservationGuard {
    application::ControlTransportGate::PlaybackToken token;
    ~ReservationGuard() { token.finish_after_join(); }
  } reservation{std::move(token)};
  if (!application::valid_playback_buffer_frames(requested_buffer_frames) ||
      first_sample == std::numeric_limits<std::uint64_t>::max())
    return fail({foundation::ErrorCode::invalid_argument,
                 "Requested audio buffer or first sample is invalid"});
  auto gate = std::make_unique<StoppedGate>();
  auto run = std::unique_ptr<RoutedPlayback>(
      new RoutedPlayback(std::move(reservation.token), std::move(gate), first_sample));
  run->requested_buffer_frames_ = requested_buffer_frames;
  run->project_root_ = root;
  run->workers_ = std::make_unique<engine::AudioWorkerPool>(audio_worker_count());
  // Frozen tracks play their renders, never their devices (ADR-0059).
  auto model =
      project::ProjectModel::open(project::audible_snapshot(std::move(snapshot)), *run->gate_);
  if (!model)
    return fail(model.error());
  auto& opened = run->model_.emplace(std::move(model).value());
  if (project::has_convolution_insert(*opened.snapshot()))
    return fail({foundation::ErrorCode::unsupported,
                 "Saved convolution insert remains in the project, but live playback "
                 "has no qualified device headroom and lifecycle evidence. Use offline "
                 "export or remove the insert from a copy of the project to play"});
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Both providers exist for the whole run, so an edit may add a plugin of
  // either kind; each saved plugin is scanned and admitted before its first
  // preparation.
  run->qualification_ = std::move(qualification);
  run->inserts_ = std::make_unique<plugins::Vst3TrackInsertProvider>();
  run->instruments_ = std::make_unique<plugins::Vst3InstrumentTrackProvider>();
  run->vst3_ = std::make_unique<plugins::Vst3StageProvider>(*run->instruments_, *run->inserts_);
  for (auto& [saved, instrument] : running_vst3(*opened.snapshot())) {
    auto scanned = scan_vst3(saved);
    if (!scanned)
      return fail(scanned.error());
    run->admit_vst3({std::move(saved), instrument, std::move(scanned)});
  }
#else
  (void)qualification;
  for (const auto& track : opened.snapshot()->tracks) {
    if (detail::vst3_instrument_source(track) ||
        std::ranges::any_of(track.effects, [](const auto& slot) {
          return slot.enabled && detail::vst3_effect(slot);
        })) {
      return fail({foundation::ErrorCode::unsupported,
                   "This Quick build has VST3 playback disabled; saved insert is not active"});
    }
  }
#endif
  // The graph keeps the backend's larger-block tolerance; VST3 stages run in
  // chunks of their host's block size (application::vst3_host_block_frames).
  const std::size_t max_block = 8192;
  constexpr std::uint64_t graph_budget = 64ULL * 1024 * 1024;
  constexpr std::uint64_t history_budget = 64ULL * 1024 * 1024;
  const engine::RoutedMixerLimits limits{.latency = {1, 8192},
                                         .max_frames = max_block,
                                         .max_buffer_bytes = graph_budget,
                                         .max_history_bytes = history_budget,
                                         .max_source_history_bytes = 0,
                                         .gain_ramp_frames = 128,
                                         // A fresh chain with lookahead primes from its path's
                                         // recent input when it switches in live (ADR-0049).
                                         .max_processor_input_history_frames = 8192,
                                         .node_meters = true};
  run->control_ = std::make_unique<application::RoutedPlaybackControl>(opened, limits,
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
                                                                       run->vst3_.get()
#else
                                                                       nullptr
#endif
  );
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  run->manual_adoption_ = std::make_unique<ManualAdoption>(*run);
#endif
  auto graph = run->control_->submit_current();
  if (!graph)
    return fail(graph.error());
  constexpr std::size_t stream_frame_budget = std::size_t{512} * 1024;
  auto prepared = application::RoutedAudioSource::prepare(
      root, *opened.snapshot(), *run->control_, stream_frame_budget, nullptr, {}, first_sample);
  if (!prepared)
    return fail(prepared.error());
  run->source_ = std::move(prepared).value();
  run->control_->bind_live_source(*run->source_);
  run->source_selection_ = std::make_unique<application::RoutedSourceSelection>(
      application::RoutedSourceBinding{opened.snapshot()->revision, run->source_});
  return foundation::Result<std::unique_ptr<RoutedPlayback>>::success(std::move(run));
}

bool RoutedSourceFill::fill(application::RoutedAudioSource& source, bool exact,
                            engine::BlockContext block, std::size_t frames,
                            std::span<const std::span<const engine::StereoFrame>>& selected,
                            engine::AudioWorkerPool* workers) noexcept {
  return exact ? source.fill_source_bank_exact(block, frames, selected, workers)
               : source.fill_source_bank(block, frames, selected, workers);
}

audio_io::StereoRenderSource RoutedPlayback::render_source() noexcept {
  return {this, &RoutedPlayback::render, &RoutedPlayback::freeze_cursor, workers_.get()};
}

bool RoutedPlayback::render(void* context, std::span<engine::StereoFrame> output,
                            engine::BlockContext block) noexcept {
  auto& run = *static_cast<RoutedPlayback*>(context);
  if (!run.control_ || !run.source_selection_)
    return false;
  auto lease = run.source_selection_->enter_callback();
  struct FillContext {
    application::RoutedSourceSelection::CallbackLease& lease;
    engine::AudioWorkerPool* workers;
    application::RoutedAudioSource* used = nullptr;
    RoutedSourceFill& fill;
    std::uint64_t exact_revision = 0;
  } fill_context{lease, run.workers_.get(), nullptr, *run.source_fill_,
                 run.source_exact_revision_.load(std::memory_order_acquire)};
  const std::span<const std::span<const engine::StereoFrame>> unused{};
  const auto result = run.control_->render_block(
      unused, output, block,
      {&fill_context, nullptr,
       [](void* context, std::uint64_t revision, engine::BlockContext fill_block,
          std::size_t frames,
          std::span<const std::span<const engine::StereoFrame>>& selected) noexcept {
         auto& state = *static_cast<FillContext*>(context);
         const bool exact = revision == state.exact_revision;
         auto* source = static_cast<application::RoutedAudioSource*>(
             exact ? state.lease.source_for(revision)
                   : state.lease.source_for_graph_revision(revision));
         if (source == nullptr)
           return false;
         state.used = source;
         return state.fill.fill(*source, exact, fill_block, frames, selected, state.workers);
       }},
      run.workers_.get());
  run.callback_frozen_ = result != engine::RoutedRenderStatus::rendered;
  if (result == engine::RoutedRenderStatus::rendered && fill_context.used != nullptr)
    fill_context.used->note_rendered_frames(output.size());
  return result == engine::RoutedRenderStatus::rendered ||
         (result == engine::RoutedRenderStatus::invalid_block && fill_context.used != nullptr &&
          fill_context.used->streaming_hold());
}

bool RoutedPlayback::freeze_cursor(void* context) noexcept {
  return static_cast<RoutedPlayback*>(context)->callback_frozen_;
}

void RoutedPlayback::begin_graceful_stop() noexcept {
  if (!source_ || !control_)
    return;
  const auto drain = control_->prepared_limiter_drain_frames();
  if (!drain)
    return;
  graceful_stop_requested_ = true;
  requested_drain_frames_ = *drain;
  if (requested_drain_frames_ != 0)
    source_->request_silent_drain();
}

foundation::Result<void> RoutedPlayback::pause() {
  if (!output_ || !source_ || !control_)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Routed playback is unavailable"});
  if (paused_)
    return foundation::Result<void>::success();
  if (graph_handoff_in_progress())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Wait for the active graph handoff before pausing"});
  output_->stop(); // Joins the callback at its last completed block.
  paused_ = true;
  // A pure pause leaves project time and processor history continuous. Only a
  // paused seek starts a new transport generation and discards that history.
  return foundation::Result<void>::success();
}

bool RoutedPlayback::graph_handoff_in_progress() const noexcept {
  return control_ && (control_->graph_handoff_in_progress() || edit_awaiting_activation_);
}

foundation::Result<void> RoutedPlayback::seek_paused(std::uint64_t first_sample) {
  if (!paused_)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Pause playback before seeking"});
  return control_->seek_convolution_after_quiesce(cursor_, first_sample);
}

foundation::Result<void> RoutedPlayback::resume() {
  if (!paused_ || !output_ || !model_)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Routed playback is not paused"});
  if (const auto failure = source_->worker_failure())
    return foundation::Result<void>::failure(*failure);
  resumed_boundary_ = cursor_.completed_snapshot();
  auto started =
      output_->start(model_->snapshot()->sample_rate, requested_buffer_frames_, target_serial_);
  if (!started)
    return started;
  paused_ = false;
  return foundation::Result<void>::success();
}

RoutedPlayback::StopDrainOutcome RoutedPlayback::stop_and_retire() noexcept {
  auto outcome =
      graceful_stop_requested_ ? StopDrainOutcome::completed : StopDrainOutcome::not_requested;
  if (output_ && source_ && requested_drain_frames_ != 0) {
    using namespace std::chrono_literals;
    constexpr auto max_drain_wait = 250ms;
    const auto deadline = std::chrono::steady_clock::now() + max_drain_wait;
    while (source_->completed_silent_drain_frames() < requested_drain_frames_) {
      const auto state = output_->status().state;
      if (state == audio_io::OutputState::stopped || state == audio_io::OutputState::disconnected ||
          state == audio_io::OutputState::error || source_->worker_failure() ||
          std::chrono::steady_clock::now() >= deadline) {
        outcome = StopDrainOutcome::truncated;
        break;
      }
      std::this_thread::sleep_for(1ms); // Worker only; never the audio callback or UI thread.
    }
  }
  if (output_) {
    output_->stop(); // Joins the callback before source/transport retirement.
    output_.reset();
  }
  if (control_)
    control_->unbind_live_source();
  if (source_preparation_.valid())
    source_preparation_.wait(); // Worker-side Stop; control remains alive until it exits.
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (vst3_scans_.valid())
    vst3_scans_.wait(); // The scan worker touches no run state, but joins before Stop ends.
  scanning_revision_.reset();
#endif
  for (auto& abandoned : abandoned_source_preparations_)
    abandoned.wait(); // Stop owns the remaining candidate workers after callback join.
  abandoned_source_preparations_.clear();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (instruments_ && !instruments_->stop_after_join())
    outcome = StopDrainOutcome::instrument_release_failed;
#endif
  source_selection_.reset();
  source_candidate_.reset();
  source_.reset(); // Joins the decoder worker after the callback has stopped.
  source_retirements_.clear();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  observe_manual_edit(); // A retired slot reports any pending value as failed.
  manual_effect_.reset();
  manual_instrument_.reset();
#endif
  if (control_)
    control_->observe();
  control_.reset();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  vst3_.reset();
  inserts_.reset();
  instruments_.reset();
#endif
  model_.reset();
  gate_.reset();
  if (token_) {
    token_->finish_after_join();
    token_.reset();
  }
  graceful_stop_requested_ = false;
  requested_drain_frames_ = 0;
  paused_ = false;
  return outcome;
}

application::RevisionStatus RoutedPlayback::status() {
  OSCILLATE_TRACE_SPAN("playback.status");
  if (!control_)
    return {};
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  observe_manual_edit();
#endif
  control_->observe();
  collect_source_retirements();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (scanning_revision_ && vst3_scans_.valid() &&
      vst3_scans_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto applied = std::move(*scanning_revision_);
    scanning_revision_.reset();
    std::optional<foundation::Error> failed;
    for (auto& scan : vst3_scans_.get()) {
      if (!scan.scanned && !failed)
        failed = scan.scanned.error();
      admit_vst3(std::move(scan));
    }
    // A failed scan fails the edit with its diagnostic; the old graph keeps
    // playing.
    const auto begun = failed ? control_->reject_edit(applied, "Quick project edit", *failed)
                              : control_->begin_edit(applied, "Quick project edit");
    if (!begun)
      edit_pending_ = false;
  }
#endif
  if (source_cancel_pending_ && source_capture_ && source_selection_ &&
      source_selection_->cancel_candidate(source_capture_->snapshot->revision)) {
    source_cancel_pending_ = false;
    source_staged_ = false;
    retire_source(std::move(source_candidate_));
    source_capture_.reset();
  }
  if (source_capture_ && source_preparation_.valid() &&
      source_preparation_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto prepared = source_preparation_.get();
    if (!prepared) {
      fail_source_handoff(prepared.error());
    } else {
      source_candidate_ = std::move(prepared).value();
    }
  }
  if (source_capture_ && synth_switch_policy_ && !source_staged_ && !edit_awaiting_activation_ &&
      std::chrono::steady_clock::now() >= synth_switch_policy_->deadline)
    fail_source_handoff({foundation::ErrorCode::unavailable,
                         "Native synth switch found no note-free release boundary within ten "
                         "seconds; saved choice is not audible"});
  if (source_capture_ && source_candidate_ && !source_staged_ && !edit_awaiting_activation_ &&
      !source_cancel_pending_) {
    const auto cursor = cursor_.completed_snapshot();
    // The old mixer must render once in a new transport generation before its
    // history can be handed over.
    if ((!resumed_boundary_ || cursor.generation != resumed_boundary_->generation ||
         cursor.next_sample != resumed_boundary_->next_sample) &&
        (!synth_switch_policy_ ||
         synth_switch_quiet_over(cursor.next_sample,
                                 std::uint64_t{requested_buffer_frames_} * synth_switch_blocks))) {
      resumed_boundary_.reset();
      const auto ready = source_candidate_->prime_handoff_window(
          cursor.generation, cursor.next_sample, requested_buffer_frames_);
      if (ready == media::HandoffWindowStatus::ready) {
        // The callback fills the staged source exactly: a block it cannot
        // fill completely plays the old source and graph instead.
        if (source_selection_->stage_candidate(
                {source_capture_->snapshot->revision, source_candidate_})) {
          source_staged_ = true;
          source_exact_revision_.store(source_capture_->snapshot->revision,
                                       std::memory_order_release);
          auto offered = source_capture_->source_overlay_eligible && !loop_
                             ? control_->publish_captured_source_overlay(*source_capture_)
                             : control_->publish_captured_source(*source_capture_);
          if (!offered) {
            source_exact_revision_.store(0, std::memory_order_release);
            fail_source_handoff(offered.error());
          } else {
            edit_awaiting_activation_ = true;
          }
        }
      } else if (ready == media::HandoffWindowStatus::missed ||
                 ready == media::HandoffWindowStatus::invalid) {
        if (source_retry_count_ < max_source_primes) {
          ++source_retry_count_;
          retire_source(std::move(source_candidate_));
          auto restarted = start_source_preparation();
          if (!restarted)
            fail_source_handoff(restarted.error());
        } else {
          fail_source_handoff({foundation::ErrorCode::unavailable,
                               "Prepared routed source missed its playback handoff window"});
        }
      }
    }
  }
  // A published source switch whose source never filled its block was
  // withdrawn, and the old graph played on: prime a fresh source at a later
  // block, a bounded number of times.
  if (source_capture_ && edit_awaiting_activation_ && source_staged_ &&
      control_->status().model_revision == source_capture_->snapshot->revision &&
      control_->status().application == application::PlaybackApplication::failed &&
      source_selection_->cancel_candidate(source_capture_->snapshot->revision)) {
    source_exact_revision_.store(0, std::memory_order_release);
    source_staged_ = false;
    edit_awaiting_activation_ = false;
    retire_source(std::move(source_candidate_));
    const bool again = source_retry_count_ < max_source_primes &&
                       control_->restart_captured_source(*source_capture_).has_value();
    if (again) {
      ++source_retry_count_;
      if (auto restarted = start_source_preparation(); !restarted)
        fail_source_handoff(restarted.error());
    } else {
      edit_pending_ = false;
      synth_switch_policy_.reset();
      source_capture_.reset();
    }
  }
  control_->observe();
  collect_source_retirements();
  if (source_capture_ && edit_awaiting_activation_ && source_candidate_ &&
      control_->status().active_graph_revision == source_capture_->snapshot->revision &&
      source_selection_->promote_candidate(source_capture_->snapshot->revision)) {
    source_ = std::move(source_candidate_);
    control_->bind_live_source(*source_);
    source_exact_revision_.store(0, std::memory_order_release);
    source_capture_.reset();
    synth_switch_policy_.reset();
    source_staged_ = false;
    // The promoted source keeps playing the armed loop. A wrap queued for
    // the retired source's priming is withdrawn and rescheduled once this
    // source primes its own next pass.
    withdraw_loop_wrap();
    if (loop_)
      (void)source_->set_loop(loop_->start, loop_->end);
  }
  if (edit_pending_ && model_ &&
      control_->status().active_graph_revision == model_->snapshot()->revision &&
      !source_capture_) {
    edit_pending_ = false;
    edit_awaiting_activation_ = false;
  }
  if (!edit_pending() && !source_cancel_pending_ && queued_revision_) {
    auto next = std::move(*queued_revision_);
    queued_revision_.reset();
    (void)begin_revision(std::move(next));
  }
  auto status = control_->status();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // An edit waiting for its plugins' scans is saved, not yet audible.
  if (scanning_revision_) {
    status.model_revision = scanning_revision_->snapshot->revision;
    status.application = application::PlaybackApplication::pending;
    status.failure.reset();
  }
#endif
  return status;
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void RoutedPlayback::admit_vst3(Vst3Scan scan) {
  if (!scan.scanned || !model_)
    return;
  const auto& saved = scan.saved;
  const auto sample_rate = model_->snapshot()->sample_rate;
  // Whole-binary admission is only for synthetic fixtures. Production
  // records are selected for each saved plugin and each exact control.
  const bool full_binary =
      std::ranges::find(qualification_.fixed_latency_binaries, saved.binary_sha256) !=
      qualification_.fixed_latency_binaries.end();
  auto blocks = qualified_manual_blocks(qualification_, saved, sample_rate);
  const bool verified_control = !blocks.empty();
  if (scan.instrument) {
    if (full_binary)
      instruments_->set_manual_qualification(saved.id, true, {});
    else
      instruments_->set_size_scoped_manual_qualification(saved.id, verified_control,
                                                         std::move(blocks));
    for (const auto& [id, record] : qualification_.fixed_instrument_tails) {
      if (id == saved.id)
        instruments_->set_tail_qualification(id, record);
    }
    instruments_->set_scan_receipt(saved.id, std::move(scan.scanned).value());
  } else {
    if (full_binary)
      inserts_->set_manual_qualification(saved.id, true, {});
    else
      inserts_->set_size_scoped_manual_qualification(saved.id, verified_control, std::move(blocks));
    for (const auto& [id, record] : qualification_.fixed_effect_tails) {
      if (id == saved.id)
        inserts_->set_fixed_tail_qualification(id, record);
    }
    inserts_->set_scan_receipt(saved.id, std::move(scan.scanned).value());
  }
  admitted_vst3_.push_back(saved.id);
}
#endif

foundation::Result<void> RoutedPlayback::begin_revision(project::Snapshot saved) {
  if (!model_ || !control_)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Routed playback is unavailable"});
  // Frozen tracks play their renders: a freeze or unfreeze is a source edit.
  auto snapshot = project::audible_snapshot(std::move(saved));
  const bool source_edit = control_->can_capture_source_edit(snapshot);
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Plugins new to this run are scanned on a worker first.
  std::vector<std::pair<project::Vst3PluginInstance, bool>> unscanned;
  for (auto& plugin : running_vst3(snapshot)) {
    if (std::ranges::find(admitted_vst3_, plugin.first.id) == admitted_vst3_.end())
      unscanned.push_back(std::move(plugin));
  }
#endif
  const auto previous_source_snapshot = model_->snapshot();
  auto adopted = model_->adopt_authoritative_snapshot(std::move(snapshot));
  if (!adopted)
    return foundation::Result<void>::failure(adopted.error());
  if (source_edit) {
    auto captured = control_->capture_source_edit(adopted.value(), "Quick source edit");
    if (!captured)
      return foundation::Result<void>::failure(captured.error());
    source_capture_ = std::move(captured).value();
    if (source_capture_->native_synth_switch) {
      if (loop_) {
        const foundation::Error error{foundation::ErrorCode::unsupported,
                                      "Native synth switching during a loop is saved but not "
                                      "audible; clear the loop and select the engine again"};
        (void)control_->fail_captured_source_edit(*source_capture_, error);
        source_capture_.reset();
        return foundation::Result<void>::failure(error);
      }
      auto compiled = engine::compile_midi_events(*previous_source_snapshot);
      if (!compiled) {
        auto error = compiled.error();
        (void)control_->fail_captured_source_edit(*source_capture_, error);
        source_capture_.reset();
        return foundation::Result<void>::failure(std::move(error));
      }
      std::vector<std::pair<project::TrackId, std::uint64_t>> tails;
      for (const auto& track : previous_source_snapshot->tracks) {
        const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
        if (!patch)
          continue;
        const auto native_lane = [](const auto& lane) {
          return std::holds_alternative<project::NativeParameterTarget>(lane.target);
        };
        const bool automated = std::ranges::any_of(track.automation, native_lane) ||
                               std::ranges::any_of(track.midi_clips, [&](const auto& clip) {
                                 return std::ranges::any_of(clip.automation, native_lane);
                               });
        const auto tail = source_->synth_tail_frames(track.id);
        const auto resolved = native_dsp::synth::resolve_native_instrument(*patch);
        if (!tail || !resolved || automated) {
          const foundation::Error error{
              foundation::ErrorCode::conflict,
              "Native synth switch has no fixed release bound; saved choice is not audible"};
          (void)control_->fail_captured_source_edit(*source_capture_, error);
          source_capture_.reset();
          return foundation::Result<void>::failure(error);
        }
        const auto current_tail = resolved.value().engine->tail_frames(
            resolved.value().values, previous_source_snapshot->sample_rate);
        tails.emplace_back(track.id, std::max(*tail, current_tail));
      }
      synth_switch_policy_.emplace(
          SynthSwitchPolicy{std::move(compiled).value(), std::move(tails),
                            std::chrono::steady_clock::now() + std::chrono::seconds(10)});
    }
    source_retry_count_ = 0;
    auto started = start_source_preparation();
    if (!started) {
      auto error = started.error();
      if (source_capture_)
        (void)control_->fail_captured_source_edit(*source_capture_, error);
      source_capture_.reset();
      return foundation::Result<void>::failure(std::move(error));
    }
    edit_pending_ = true;
    edit_awaiting_activation_ = false;
    return foundation::Result<void>::success();
  }
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!unscanned.empty()) {
    try {
      vst3_scans_ = std::async(std::launch::async, [plugins = std::move(unscanned)]() mutable {
        std::vector<Vst3Scan> scans;
        scans.reserve(plugins.size());
        for (auto& [saved, instrument] : plugins) {
          auto scanned = scan_vst3(saved);
          scans.push_back({std::move(saved), instrument, std::move(scanned)});
        }
        return scans;
      });
    } catch (const std::system_error&) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::resource_exhausted, "Cannot start the VST3 scan worker"});
    }
    scanning_revision_.emplace(std::move(adopted).value());
    edit_pending_ = true;
    edit_awaiting_activation_ = false;
    return foundation::Result<void>::success();
  }
#endif
  auto begun = control_->begin_edit(adopted.value(), "Quick project edit");
  if (!begun)
    return begun;
  edit_pending_ = true;
  edit_awaiting_activation_ = false;
  return foundation::Result<void>::success();
}

foundation::Result<void> RoutedPlayback::start_source_preparation() {
  if (!source_capture_ || !control_)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Routed source capture is unavailable"});
  const auto cursor = cursor_.completed_snapshot();
  try {
    source_preparation_ =
        std::async(std::launch::async, [root = project_root_, capture = *source_capture_,
                                        control = control_.get(), live_cursor = &cursor_,
                                        first_sample = cursor.next_sample,
                                        generation = cursor.generation]() mutable {
          constexpr std::size_t stream_frame_budget = std::size_t{512} * 1024;
          return application::RoutedAudioSource::prepare(root, capture, *control,
                                                         stream_frame_budget, nullptr, {},
                                                         first_sample, generation, live_cursor);
        });
  } catch (const std::system_error&) {
    return foundation::Result<void>::failure({foundation::ErrorCode::resource_exhausted,
                                              "Cannot start routed source preparation worker"});
  }
  return foundation::Result<void>::success();
}

void RoutedPlayback::retire_source(std::shared_ptr<application::RoutedAudioSource> source) {
  if (source)
    source_retirements_.push_back(std::async(std::launch::async, [source = std::move(source)] {}));
}

void RoutedPlayback::collect_source_retirements() {
  std::erase_if(abandoned_source_preparations_, [this](auto& preparation) {
    if (preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return false;
    auto prepared = preparation.get();
    if (prepared)
      retire_source(std::move(prepared).value());
    return true;
  });
  if (source_selection_) {
    if (auto retired = source_selection_->take_retired_if_quiescent()) {
      source_retirements_.push_back(
          std::async(std::launch::async, [owner = std::move(*retired)] {}));
    }
  }
  std::erase_if(source_retirements_, [](std::future<void>& future) {
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
}

void RoutedPlayback::fail_source_handoff(const foundation::Error& error) {
  if (!source_staged_ && source_capture_)
    (void)control_->fail_captured_source_edit(*source_capture_, error);
  edit_pending_ = false;
  edit_awaiting_activation_ = false;
  synth_switch_policy_.reset();
  if (source_preparation_.valid())
    abandoned_source_preparations_.push_back(std::move(source_preparation_));
  if (source_staged_) {
    source_cancel_pending_ = true;
    return;
  }
  retire_source(std::move(source_candidate_));
  source_capture_.reset();
}

bool RoutedPlayback::synth_switch_quiet_over(std::uint64_t sample,
                                             std::uint64_t frames) const noexcept {
  if (!synth_switch_quiet_at(sample))
    return false;
  if (!synth_switch_policy_)
    return true;
  // No note may start before the switch can have taken over either.
  const auto end =
      frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - sample
          ? std::numeric_limits<std::int64_t>::max()
          : static_cast<std::int64_t>(sample + frames);
  for (const auto& [track, tail] : synth_switch_policy_->release_tails) {
    (void)tail;
    const auto found = std::ranges::find(synth_switch_policy_->old_notes.instruments(), track,
                                         &engine::InstrumentEvents::track);
    if (found == synth_switch_policy_->old_notes.instruments().end())
      return false;
    if (std::ranges::any_of(found->events(), [&](const engine::ScheduledMidiEvent& event) {
          return event.kind == engine::MidiEventKind::note_on &&
                 event.sample.value >= static_cast<std::int64_t>(sample) &&
                 event.sample.value < end;
        }))
      return false;
  }
  return true;
}

bool RoutedPlayback::synth_switch_quiet_at(std::uint64_t sample) const noexcept {
  if (!synth_switch_policy_ ||
      sample > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return !synth_switch_policy_;
  for (const auto& [track, tail] : synth_switch_policy_->release_tails) {
    const auto found = std::ranges::find(synth_switch_policy_->old_notes.instruments(), track,
                                         &engine::InstrumentEvents::track);
    if (found == synth_switch_policy_->old_notes.instruments().end())
      return false;
    std::size_t held = 0;
    std::uint64_t release_end = 0;
    for (const auto& event : found->events()) {
      if (event.sample.value >= static_cast<std::int64_t>(sample))
        break;
      if (event.kind == engine::MidiEventKind::note_on) {
        ++held;
      } else {
        if (held == 0)
          return false;
        --held;
        const auto off = static_cast<std::uint64_t>(event.sample.value);
        release_end = std::max(release_end, tail > std::numeric_limits<std::uint64_t>::max() - off
                                                ? std::numeric_limits<std::uint64_t>::max()
                                                : off + tail);
      }
    }
    if (held != 0 || sample < release_end)
      return false;
  }
  return true;
}

foundation::Result<void> RoutedPlayback::submit_revision(project::Snapshot snapshot) {
  OSCILLATE_TRACE_SPAN("edit.submit_revision");
  if (paused())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Resume playback to apply this edit"});
  // A manual VST3 gesture adopts its saved revision directly; its status
  // reports whether that revision became audible.
  if (model_ && snapshot.revision == model_->snapshot()->revision &&
      manual_adopted(snapshot.revision))
    return foundation::Result<void>::success();
  if (!model_ || snapshot.revision <= model_->snapshot()->revision)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Routed playback edit is stale"});
  if (edit_pending()) {
    queued_revision_ = std::move(snapshot);
    return foundation::Result<void>::success();
  }
  return begin_revision(std::move(snapshot));
}

audio_io::OutputStatus RoutedPlayback::output_status() const noexcept {
  return output_ ? output_->status() : audio_io::OutputStatus{};
}

application::RoutedSourceStatus RoutedPlayback::source_status() const noexcept {
  return source_ ? source_->status() : application::RoutedSourceStatus{};
}

std::optional<media::UnderrunEvent> RoutedPlayback::pop_source_underrun() noexcept {
  return source_ ? source_->pop_source_underrun() : std::nullopt;
}

std::optional<foundation::Error> RoutedPlayback::source_failure() const {
  return source_ ? source_->worker_failure() : std::nullopt;
}

std::optional<foundation::Error> RoutedPlayback::processor_fault() const {
  return control_ ? control_->processor_fault() : std::nullopt;
}

bool RoutedPlayback::set_live_channel(std::optional<project::TrackId> track, double level,
                                      double pan) noexcept {
  return control_ && !paused() && control_->set_live_channel(track, level, pan);
}

bool RoutedPlayback::set_live_send(project::TrackId track, project::SendId send,
                                   double gain) noexcept {
  return control_ && !paused() && control_->set_live_send(track, send, gain);
}

bool RoutedPlayback::set_live_synth_values(project::TrackId track,
                                           const native_dsp::synth::EngineDescriptor& engine,
                                           std::span<const double> values) noexcept {
  return control_ && !paused() && control_->set_live_synth_values(track, engine, values);
}

bool RoutedPlayback::live_note(project::TrackId track, std::uint8_t key, std::uint8_t velocity,
                               bool on) noexcept {
  return control_ && !paused() && control_->live_note(track, key, velocity, on);
}

bool RoutedPlayback::set_live_effect_values(std::optional<project::TrackId> track,
                                            project::InsertSlotId slot,
                                            const native_dsp::synth::EngineDescriptor& engine,
                                            std::span<const double> values) noexcept {
  return control_ && !paused() && control_->set_live_effect_values(track, slot, engine, values);
}

std::optional<engine::RoutedNodeMeterReading>
RoutedPlayback::track_meter(project::TrackId track) const noexcept {
  return control_ ? control_->active_track_meter(track) : std::nullopt;
}

bool RoutedPlayback::effect_telemetry(std::optional<project::TrackId> track,
                                      project::InsertSlotId insert, std::vector<float>& frame) {
  return control_ && control_->active_effect_telemetry(track, insert, frame);
}

bool RoutedPlayback::set_effect_key_listen(project::TrackId track, project::InsertSlotId insert,
                                           bool listen) noexcept {
  return control_ && control_->set_active_effect_key_listen(track, insert, listen);
}

std::optional<float> RoutedPlayback::effect_key_level(project::TrackId track,
                                                      project::InsertSlotId insert) const noexcept {
  return control_ ? control_->active_effect_key_peak(track, insert) : std::nullopt;
}

std::optional<std::uint64_t> RoutedPlayback::settled_graph_revision() const {
  if (!control_ || !model_ || paused() || edit_pending() || queued_revision_ || source_capture_ ||
      control_->graph_handoff_in_progress())
    return std::nullopt;
  // Audible state must equal the accepted model state. The returned topology revision can
  // be older after acknowledged manual parameter adoption.
  const auto audible = control_->status().active_graph_revision;
  const auto topology = control_->active_topology_revision();
  if (audible == 0 || audible != model_->snapshot()->revision || topology == 0)
    return std::nullopt;
  return topology;
}

bool RoutedPlayback::manual_adopted(std::uint64_t revision) const {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  return manual_status_ && manual_status_->status.saved_revision == revision && control_ &&
         control_->status().model_revision == revision;
#else
  (void)revision;
  return false;
#endif
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void RoutedPlayback::observe_manual_edit() {
  if (!manual_status_ || manual_status_->status.phase != application::Vst3ManualEditPhase::pending)
    return;
  auto& pending = *manual_status_;
  if (manual_effect_)
    pending.status = manual_effect_->observe();
  else if (manual_instrument_)
    pending.status = manual_instrument_->observe();
}

foundation::Result<void>
RoutedPlayback::preflight_vst3_manual_edit(std::uint64_t saved_revision, project::TrackId track,
                                           std::optional<project::InsertSlotId> insert,
                                           project::PluginInstanceId plugin,
                                           plugins::Vst3ParameterEvent parameter) {
  const auto refuse = [](foundation::ErrorCode code, std::string message) {
    return foundation::Result<void>::failure({code, std::move(message)});
  };
  (void)status();
  if (!manual_adoption_)
    return refuse(foundation::ErrorCode::unavailable, "routed playback has stopped");
  if (manual_status_ && manual_status_->status.phase == application::Vst3ManualEditPhase::pending)
    return refuse(foundation::ErrorCode::conflict,
                  vst3_manual_refusal(plugins::Vst3ManualEventStatus::busy));
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
  plugins::Vst3ActiveGraphIdentity identity;
  if (insert) {
    if (auto binding = active_vst3_effect_manual_events(track, *insert, plugin)) {
      slot = std::move(binding->slot);
      identity = binding->identity;
    }
  } else if (auto binding = active_vst3_instrument_manual_events(track, plugin)) {
    slot = std::move(binding->slot);
    identity = binding->identity;
  }
  if (!slot)
    return refuse(foundation::ErrorCode::unavailable,
                  "no qualified live control is active for this plugin's saved state");
  if (!model_ || model_->snapshot()->revision != saved_revision)
    return refuse(foundation::ErrorCode::conflict,
                  "the audible graph is not the saved project revision");
  const auto admission = slot->admission(identity, saved_revision + 1, parameter);
  if (admission != plugins::Vst3ManualEventStatus::accepted)
    return refuse(foundation::ErrorCode::unsupported, vst3_manual_refusal(admission));
  return manual_adoption_->preflight(plugin, identity);
}

foundation::Result<application::Vst3ManualEditStatus>
RoutedPlayback::commit_vst3_manual_edit(application::ProjectSession& session,
                                        Vst3ManualGesture gesture,
                                        application::EditorSavePolicy save_policy) {
  using Result = foundation::Result<application::Vst3ManualEditStatus>;
  auto ready = preflight_vst3_manual_edit(gesture.opened_revision, gesture.track, gesture.insert,
                                          gesture.original.id, gesture.parameter);
  if (!ready)
    return Result::failure(ready.error());
  const auto topology = settled_graph_revision();
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
  plugins::Vst3ActiveGraphIdentity identity;
  if (gesture.insert) {
    if (auto binding =
            active_vst3_effect_manual_events(gesture.track, *gesture.insert, gesture.original.id)) {
      slot = std::move(binding->slot);
      identity = binding->identity;
    }
  } else if (auto binding =
                 active_vst3_instrument_manual_events(gesture.track, gesture.original.id)) {
    slot = std::move(binding->slot);
    identity = binding->identity;
  }
  if (!topology || !slot)
    return Result::failure({foundation::ErrorCode::conflict, "live VST3 slot changed"});
  manual_effect_.reset();
  manual_instrument_.reset();
  manual_status_.reset();
  Result committed = Result::failure({foundation::ErrorCode::internal, "no VST3 transaction"});
  if (gesture.insert) {
    manual_effect_.emplace(session, std::move(slot), manual_adoption_.get());
    committed = manual_effect_->commit(gesture.opened_revision, gesture.track, *gesture.insert,
                                       gesture.original, std::move(gesture.component_state),
                                       std::move(gesture.references), gesture.parameter, identity,
                                       gesture.gesture_sequence, save_policy);
    manual_status_ = Vst3ManualGestureStatus{gesture.original.id, manual_effect_->observe()};
  } else {
    manual_instrument_.emplace(session, std::move(slot), manual_adoption_.get());
    committed = manual_instrument_->commit(gesture.opened_revision, gesture.track, gesture.original,
                                           std::move(gesture.component_state),
                                           std::move(gesture.references), gesture.parameter,
                                           identity, gesture.gesture_sequence, save_policy);
    manual_status_ = Vst3ManualGestureStatus{gesture.original.id, manual_instrument_->observe()};
  }
  if (committed)
    manual_status_->status = committed.value();
  return committed;
}

std::optional<Vst3ManualGestureStatus> RoutedPlayback::vst3_manual_edit_status() const {
  return manual_status_;
}
#endif

std::optional<application::ActiveVst3EffectManualEvents>
RoutedPlayback::active_vst3_effect_manual_events(project::TrackId track,
                                                 project::InsertSlotId insert,
                                                 project::PluginInstanceId plugin) const {
  const auto revision = settled_graph_revision();
  if (!revision)
    return std::nullopt;
  return control_->active_vst3_effect_manual_events(track, insert, plugin, *revision);
}

std::optional<application::ActiveVst3InstrumentManualEvents>
RoutedPlayback::active_vst3_instrument_manual_events(project::TrackId track,
                                                     project::PluginInstanceId plugin) const {
  const auto revision = settled_graph_revision();
  if (!revision)
    return std::nullopt;
  return control_->active_vst3_instrument_manual_events(track, plugin, *revision);
}

std::optional<application::PendingVst3ManualPreviewEvents>
RoutedPlayback::pending_vst3_manual_preview_events(project::TrackId track,
                                                   std::optional<project::InsertSlotId> insert,
                                                   project::PluginInstanceId plugin) const {
  // The pending VST3 manual receipt is the sole edit admitted by this path.
  // Other graph edits still block preview access.
  if (!control_ || !model_ || paused() || edit_pending_ || queued_revision_ || source_capture_)
    return std::nullopt;
  return control_->pending_vst3_manual_preview_events(track, insert, plugin);
}

std::vector<application::RoutedProcessedClipStatus> RoutedPlayback::processed_clip_status() const {
  return source_ ? source_->processed_clip_status()
                 : std::vector<application::RoutedProcessedClipStatus>{};
}

void RoutedPlayback::withdraw_loop_wrap() noexcept {
  if (loop_scheduled_generation_ != 0)
    (void)cursor_.cancel_loop_wrap();
  loop_scheduled_generation_ = 0;
}

bool RoutedPlayback::set_loop(std::uint64_t start, std::uint64_t end) {
  if (!source_ || end <= start || synth_switch_policy_)
    return false;
  if (loop_ && loop_->start == start && loop_->end == end)
    return true;
  withdraw_loop_wrap();
  if (!source_->set_loop(start, end))
    return false;
  loop_ = LoopRange{start, end};
  return true;
}

void RoutedPlayback::clear_loop() {
  withdraw_loop_wrap();
  loop_.reset();
  if (source_)
    source_->clear_loop();
}

application::RoutedAudioSource::LoopStatus RoutedPlayback::loop_status() const {
  return source_ && loop_ ? source_->loop_status() : application::RoutedAudioSource::LoopStatus{};
}

RoutedPlayback::LoopProgress RoutedPlayback::service_loop() {
  LoopProgress progress;
  progress.wraps = cursor_.loop_wrap_counts();
  if (!source_ || paused())
    return progress;
  const auto position = cursor_.completed_snapshot();
  if (loop_restart_generation_ != 0) {
    if (position.generation <= loop_restart_generation_) {
      progress.restarting = true;
      return progress;
    }
    loop_restart_generation_ = 0;
  }
  const auto status = source_->loop_status();
  if (status.missed) {
    // Frames are queued for a pass that no longer applies. Restart in place
    // to discard them, unless the playhead is already past an armed loop
    // (the source would miss again; playback just continues).
    const bool past_loop = loop_ && position.next_sample >= loop_->end;
    if (!past_loop && !graph_handoff_in_progress() && cursor_.request_restart_in_place()) {
      loop_scheduled_generation_ = 0;
      loop_restart_generation_ = position.generation;
      progress.restarting = true;
      return progress;
    }
    progress.missed = loop_.has_value();
    return progress;
  }
  if (!loop_)
    return progress;
  const auto next = position.generation + 1;
  if (loop_scheduled_generation_ == next) {
    progress.scheduled = true;
    return progress;
  }
  if (status.ready_generation != next || position.next_sample > loop_->end)
    return progress;
  if (cursor_.request_loop_wrap_at(loop_->end, loop_->start)) {
    loop_scheduled_generation_ = next;
    progress.scheduled = true;
  }
  return progress;
}

std::uint64_t RoutedPlayback::completed_sample() const noexcept {
  return cursor_.completed_snapshot().next_sample;
}

std::uint32_t RoutedPlayback::sample_rate() const noexcept {
  return model_ ? model_->snapshot()->sample_rate : 0;
}

std::size_t RoutedPlayback::prepared_block_limit() const noexcept {
  return control_ ? control_->max_frames() : 0;
}

} // namespace oscillate::quick
