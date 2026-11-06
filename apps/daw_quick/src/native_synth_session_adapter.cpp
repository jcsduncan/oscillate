#include "native_synth_session_adapter.hpp"

#include "oscillate/analysis/tempo.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/persistence/effect_preset.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

using foundation::ErrorCode;
using native_dsp::synth::SynthMediaAsset;

// How often the UI thread looks for a finished decode or import.
constexpr std::chrono::milliseconds worker_poll_interval{50};
// Decoded samples kept by identity: the current one and a few undo steps back.
constexpr std::size_t max_remembered_media = 4;
// A new sound's own tempo analysis is trusted from this confidence.
constexpr double min_import_tempo_confidence = 0.5;

// A descriptor the worker owns and closes.
class OwnedFd {
public:
  explicit OwnedFd(int fd) noexcept : fd_(fd) {}
  OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  OwnedFd& operator=(OwnedFd&&) = delete;
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  ~OwnedFd() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

std::optional<double> reviewed_bpm(const project::SourceTempoReview& review) {
  const double bpm = review.corrected_bpm.value_or(review.detected_bpm);
  return bpm > 0.0 ? std::optional<double>(bpm) : std::nullopt;
}

// The new sound's own tempo, when the analysis is confident (worker only).
std::optional<double> analysed_tempo(int fd, const project::SourceRecord& record,
                                     const std::stop_token& stop) {
  auto reader = media::AudioReader::open_fd(fd, record);
  if (!reader)
    return std::nullopt;
  auto estimate = analysis::analyze_tempo(reader.value(), {record.sha256, 0}, {},
                                          [&stop] { return stop.stop_requested(); });
  if (!estimate || estimate.value().bpm <= 0.0 ||
      estimate.value().confidence < min_import_tempo_confidence)
    return std::nullopt;
  return estimate.value().bpm;
}

} // namespace

SessionNativeSynthAdapter::SessionNativeSynthAdapter(
    application::ProjectSession& session, project::TrackId track,
    const native_dsp::synth::SynthEngine& engine, EditHook after_edit, AudibleProbe audible,
    TelemetryProbe telemetry, SaveHook request_save, LiveHook live, MediaDecoder decode)
    : session_(session), track_(track), path_(track), engine_(&engine),
      descriptor_(*engine.descriptor), after_edit_(std::move(after_edit)),
      audible_(std::move(audible)), telemetry_(std::move(telemetry)),
      request_save_(std::move(request_save)), live_(std::move(live)), decode_(std::move(decode)) {
  poll_timer_.setInterval(worker_poll_interval);
  QObject::connect(&poll_timer_, &QTimer::timeout, &poll_timer_, [this] { poll(); });
}

SessionNativeSynthAdapter::SessionNativeSynthAdapter(
    application::ProjectSession& session, project::SignalPathId path, project::InsertSlotId slot,
    const native_dsp::effects::EffectEngine& effect, EditHook after_edit, AudibleProbe audible,
    TelemetryProbe telemetry, SaveHook request_save, LiveHook live)
    : session_(session), track_(path_track(path)), path_(std::move(path)), slot_(slot),
      effect_(&effect), descriptor_(*effect.descriptor), after_edit_(std::move(after_edit)),
      audible_(std::move(audible)), telemetry_(std::move(telemetry)),
      request_save_(std::move(request_save)), live_(std::move(live)) {}

SessionNativeSynthAdapter::~SessionNativeSynthAdapter() = default;

project::TrackId SessionNativeSynthAdapter::path_track(const project::SignalPathId& path) {
  if (const auto* track = std::get_if<project::TrackId>(&path))
    return *track;
  // The master path has no track; a reserved id that names none.
  return project::TrackId::from_bits({~0ULL, ~0ULL}).value();
}

foundation::Result<std::vector<double>> SessionNativeSynthAdapter::saved_values() const {
  return slot_ ? session_.effects().native_effect_values(path_, *slot_)
               : session_.instruments().native_instrument_values(track_);
}

foundation::Result<void>
SessionNativeSynthAdapter::write_values(std::span<const double> values,
                                        std::optional<std::uint64_t> gesture) {
  return slot_ ? session_.effects().set_native_effect_values(path_, *slot_, values, gesture)
               : session_.instruments().set_native_instrument_values(track_, values, gesture);
}

bool SessionNativeSynthAdapter::telemetry(std::vector<float>& frame) {
  return telemetry_ && unavailable_reason().empty() && telemetry_(frame);
}

std::string SessionNativeSynthAdapter::unavailable_reason() const {
  if (slot_) {
    const auto& snapshot = session_.snapshot();
    const std::vector<project::EffectInsert>* effects = &snapshot.master_effects;
    if (const auto* track = std::get_if<project::TrackId>(&path_)) {
      const auto found = std::ranges::find(snapshot.tracks, *track, &project::Track::id);
      if (found == snapshot.tracks.end())
        return "Track not found";
      effects = &found->effects;
    }
    const auto slot = std::ranges::find(*effects, *slot_, &project::EffectInsert::id);
    if (slot == effects->end())
      return "Effect slot was removed";
    const auto* patch = std::get_if<project::NativeEffectPatch>(&slot->processor);
    if (patch == nullptr || patch->engine != descriptor_.id)
      return "Slot no longer holds " + std::string(descriptor_.name);
    return {};
  }
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track == tracks.end()) {
    return "Track not found";
  }
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track->instrument_source);
  if (patch == nullptr) {
    return "Track has no built-in synth";
  }
  if (patch->engine != descriptor_.id) {
    return "Track plays '" + patch->engine + "', not " + std::string(descriptor_.name);
  }
  return {};
}

std::vector<double> SessionNativeSynthAdapter::values() const {
  if (live_values_)
    return *live_values_;
  if (unavailable_reason().empty()) {
    if (auto values = saved_values()) {
      return std::move(values).value();
    }
  }
  // Read-only placeholder in this panel's own table: status() says why.
  return native_dsp::synth::default_values(descriptor_);
}

void SessionNativeSynthAdapter::apply(std::span<const double> values,
                                      std::optional<std::uint64_t> gesture) {
  const auto before = session_.snapshot().revision;
  auto applied = write_values(values, gesture);
  if (!applied) {
    message_ = applied.error().message;
    return;
  }
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
}

void SessionNativeSynthAdapter::save() {
  if (request_save_) {
    request_save_();
    return;
  }
  if (auto saved = session_.saving().save(); !saved) {
    message_ = "Edit kept but not saved: " + saved.error().message;
  }
}

void SessionNativeSynthAdapter::submit(std::span<const double> values, NativeSynthGesture gesture) {
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return;
  }
  switch (gesture) {
  case NativeSynthGesture::single:
    gesture_.reset();
    apply(values, std::nullopt);
    if (message_.empty()) {
      save();
    }
    return;
  case NativeSynthGesture::begin:
    gesture_ = next_gesture_++;
    live_values_ = this->values();
    return;
  case NativeSynthGesture::update:
    // Heard at once while playing; nothing is saved until the release.
    if (!gesture_)
      gesture_ = next_gesture_++;
    live_values_.emplace(values.begin(), values.end());
    if (live_)
      (void)live_(values);
    return;
  case NativeSynthGesture::commit:
    live_values_.reset();
    apply(values, gesture_ ? gesture_ : std::optional<std::uint64_t>(next_gesture_++));
    gesture_.reset();
    if (message_.empty()) {
      save();
    }
    return;
  case NativeSynthGesture::cancel:
    // Nothing was saved: the running instance glides back to the saved values.
    if (live_values_) {
      live_values_.reset();
      if (const auto saved = saved_values(); saved && live_)
        (void)live_(saved.value());
    }
    gesture_.reset();
    return;
  }
}

project::NativeSynthDocument SessionNativeSynthAdapter::document() const {
  if (!unavailable_reason().empty())
    return {};
  auto document = slot_ ? session_.effects().native_effect_document(path_, *slot_)
                        : session_.instruments().native_synth_document(track_);
  return document ? std::move(document).value() : project::NativeSynthDocument{};
}

void SessionNativeSynthAdapter::apply_document(project::NativeSynthDocument document,
                                               std::optional<std::uint64_t> gesture) {
  const auto before = session_.snapshot().revision;
  auto applied =
      slot_
          ? session_.effects().set_native_effect_document(path_, *slot_, std::move(document),
                                                          gesture)
          : session_.instruments().set_native_synth_document(track_, std::move(document), gesture);
  if (!applied) {
    message_ = applied.error().message;
    return;
  }
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
}

// Documents have no unsaved live path: each step of a drag is an edit under
// one gesture, so the drag is heard as it moves and undoes as one step; the
// project file is written when it ends.
void SessionNativeSynthAdapter::submit_document(project::NativeSynthDocument document,
                                                NativeSynthGesture gesture) {
  if (slot_ && effect_->prepare_document == nullptr) {
    message_ = "This effect has no structured state";
    return;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return;
  }
  switch (gesture) {
  case NativeSynthGesture::single:
    gesture_.reset();
    apply_document(std::move(document), std::nullopt);
    if (message_.empty())
      save();
    return;
  case NativeSynthGesture::begin:
    gesture_ = next_gesture_++;
    document_before_gesture_ = this->document();
    return;
  case NativeSynthGesture::update:
    if (!gesture_)
      gesture_ = next_gesture_++;
    apply_document(std::move(document), gesture_);
    return;
  case NativeSynthGesture::commit:
    apply_document(std::move(document),
                   gesture_ ? gesture_ : std::optional<std::uint64_t>(next_gesture_++));
    gesture_.reset();
    document_before_gesture_.reset();
    if (message_.empty())
      save();
    return;
  case NativeSynthGesture::cancel:
    if (document_before_gesture_ && gesture_)
      apply_document(std::move(*document_before_gesture_), gesture_);
    document_before_gesture_.reset();
    gesture_.reset();
    return;
  }
}

bool SessionNativeSynthAdapter::import_media(std::string_view slot, std::string_view path) {
  if (slot_) {
    message_ = "Effects take no media";
    return false;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    message_ = "Cannot open " + std::string(path);
    return false;
  }
  gesture_.reset();
  const auto before = session_.snapshot().revision;
  auto imported = session_.imports().import_native_synth_media(track_, slot, fd);
  ::close(fd);
  if (!imported) {
    message_ = imported.error().message;
    return false;
  }
  // The session saved the new source with the edit.
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
  return true;
}

const project::NativeInstrumentPatch* SessionNativeSynthAdapter::saved_patch() const {
  if (slot_ || !unavailable_reason().empty())
    return nullptr;
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  return track == tracks.end()
             ? nullptr
             : std::get_if<project::NativeInstrumentPatch>(&track->instrument_source);
}

void SessionNativeSynthAdapter::apply_state(std::span<const double> values,
                                            project::NativeSynthDocument document,
                                            std::optional<std::uint64_t> gesture) {
  // The engine's canonical sparse values for these dense ones.
  auto patch = native_dsp::synth::make_native_patch(*engine_, values, std::move(document));
  if (!patch) {
    message_ = patch.error().message;
    return;
  }
  const auto before = session_.snapshot().revision;
  auto applied = session_.instruments().set_native_instrument_state(
      track_, std::move(patch.value().parameters), std::move(patch.value().document), gesture);
  if (!applied) {
    message_ = applied.error().message;
    return;
  }
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
}

// As submit_document: each step of a drag is an edit under one gesture.
void SessionNativeSynthAdapter::submit_state(std::span<const double> values,
                                             project::NativeSynthDocument document,
                                             NativeSynthGesture gesture) {
  if (slot_) {
    message_ = "Effects have no regions";
    return;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return;
  }
  switch (gesture) {
  case NativeSynthGesture::single:
    gesture_.reset();
    apply_state(values, std::move(document), std::nullopt);
    if (message_.empty())
      save();
    return;
  case NativeSynthGesture::begin:
    gesture_ = next_gesture_++;
    values_before_gesture_ = this->values();
    document_before_gesture_ = this->document();
    return;
  case NativeSynthGesture::update:
    if (!gesture_)
      gesture_ = next_gesture_++;
    apply_state(values, std::move(document), gesture_);
    return;
  case NativeSynthGesture::commit:
    apply_state(values, std::move(document),
                gesture_ ? gesture_ : std::optional<std::uint64_t>(next_gesture_++));
    gesture_.reset();
    values_before_gesture_.reset();
    document_before_gesture_.reset();
    if (message_.empty())
      save();
    return;
  case NativeSynthGesture::cancel:
    if (values_before_gesture_ && document_before_gesture_ && gesture_)
      apply_state(*values_before_gesture_, std::move(*document_before_gesture_), gesture_);
    values_before_gesture_.reset();
    document_before_gesture_.reset();
    gesture_.reset();
    return;
  }
}

void SessionNativeSynthAdapter::remember(std::string identity,
                                         std::shared_ptr<const SynthMediaAsset> audio) {
  std::erase_if(remembered_,
                [&identity](const RememberedMedia& entry) { return entry.identity == identity; });
  remembered_.push_back({std::move(identity), std::move(audio)});
  if (remembered_.size() > max_remembered_media)
    remembered_.erase(remembered_.begin());
}

std::shared_ptr<const SynthMediaAsset>
SessionNativeSynthAdapter::remembered(std::string_view identity) const {
  const auto found = std::ranges::find(remembered_, identity, &RememberedMedia::identity);
  return found == remembered_.end() ? nullptr : found->audio;
}

void SessionNativeSynthAdapter::set_media_listener(std::function<void()> listener) {
  media_listener_ = std::move(listener);
}

NativeSynthMedia SessionNativeSynthAdapter::media(std::string_view slot) {
  NativeSynthMedia result;
  const auto* patch = saved_patch();
  if (patch == nullptr)
    return result;
  const auto& media_slots = patch->document.media;
  const auto found = std::ranges::find(media_slots, slot, &project::NativeMediaSlot::id);
  if (found == media_slots.end())
    return result;
  if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&found->media)) {
    result.builtin = true;
    result.identity = builtin->name;
    return result;
  }
  const auto& reference = std::get<project::ManagedAudioReference>(found->media);
  result.identity = reference.sha256;
  const auto& snapshot = session_.snapshot();
  const auto review = std::ranges::find(snapshot.source_tempo_reviews, reference.source,
                                        &project::SourceTempoReview::source);
  if (review != snapshot.source_tempo_reviews.end())
    result.reviewed_bpm = reviewed_bpm(*review);
  if (auto audio = remembered(reference.sha256)) {
    result.audio = std::move(audio);
    return result;
  }
  // One decode at a time; the listener's refresh asks again when it lands.
  if (decode_job_ || import_) {
    result.decoding = true;
    return result;
  }
  if (decode_failure_identity_ == reference.sha256) {
    result.message = decode_failure_;
    return result;
  }
  const auto record =
      std::ranges::find(snapshot.sources, reference.source, &project::SourceRecord::id);
  if (record == snapshot.sources.end()) {
    result.message = "The sample's source is missing";
    return result;
  }
  if (!decode_) {
    result.message = "The sample cannot be decoded here";
    return result;
  }
  auto job = std::make_shared<Decode>();
  job->identity = reference.sha256;
  try {
    decode_worker_ =
        std::jthread([job, decode = decode_, source = *record](const std::stop_token& stop) {
          try {
            job->result.emplace(decode(source, stop));
          } catch (const std::exception& error) {
            job->result.emplace(foundation::Result<source_prep::DecodedAudio>::failure(
                {ErrorCode::internal, std::string("Sample decode failed: ") + error.what()}));
          }
          job->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception&) {
    result.message = "Cannot start the sample decode";
    return result;
  }
  decode_job_ = std::move(job);
  poll_timer_.start();
  result.decoding = true;
  return result;
}

bool SessionNativeSynthAdapter::begin_media_import(std::string_view slot, std::string_view path,
                                                   NativeSynthMediaPlanner plan,
                                                   NativeSynthMediaDone done) {
  if (slot_) {
    message_ = "Effects take no media";
    return false;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  if (import_) {
    message_ = "Finish the current import first";
    return false;
  }
  const std::string file(path);
  // Two descriptors: the session stages one; the worker decodes the other
  // (its own offset) so the plan sees the audio before the import lands.
  const OwnedFd staged(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
  OwnedFd decoded(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
  if (staged.get() < 0 || decoded.get() < 0) {
    message_ = "Cannot open " + file;
    return false;
  }
  gesture_.reset();
  auto task = session_.imports().begin_native_synth_media_import(track_, slot, staged.get());
  if (!task) {
    message_ = task.error().message;
    return false;
  }
  // Reviewed tempos by content, so a sound the project already knows keeps
  // the tempo the user confirmed.
  std::vector<std::pair<std::string, double>> reviewed;
  const auto& snapshot = session_.snapshot();
  for (const auto& review : snapshot.source_tempo_reviews) {
    const auto source =
        std::ranges::find(snapshot.sources, review.source, &project::SourceRecord::id);
    if (const auto bpm = reviewed_bpm(review); bpm && source != snapshot.sources.end())
      reviewed.emplace_back(source->sha256, *bpm);
  }
  auto job = std::make_shared<Import>();
  job->done_callback = std::move(done);
  try {
    import_worker_ = std::jthread([job, task = std::move(task).value(),
                                   decoded = std::move(decoded), reviewed = std::move(reviewed),
                                   plan = std::move(plan)](const std::stop_token& stop) mutable {
      using Prepared = foundation::Result<application::PreparedNativeSynthMediaImport>;
      try {
        auto prepared = std::move(task).run(stop);
        if (prepared) {
          const auto& record = prepared.value().source();
          auto audio = source_prep::decode_audio_fd(decoded.get(), record,
                                                    project::max_native_synth_media_frames, stop);
          if (!audio) {
            prepared = Prepared::failure(audio.error());
          } else {
            auto asset = std::make_shared<SynthMediaAsset>();
            asset->source_rate = audio.value().source_rate;
            asset->frames = std::move(audio.value().frames);
            const auto known =
                std::ranges::find(reviewed, record.sha256, &std::pair<std::string, double>::first);
            const auto tempo = known != reviewed.end()
                                   ? std::optional<double>(known->second)
                                   : analysed_tempo(decoded.get(), record, stop);
            job->plan.emplace(plan ? plan(*asset, tempo)
                                   : foundation::Result<NativeSynthMediaPlan>::success({}));
            job->audio = std::move(asset);
          }
        }
        job->prepared.emplace(std::move(prepared));
      } catch (const std::exception& error) {
        job->prepared.emplace(Prepared::failure(
            {ErrorCode::internal, std::string("Sound import failed: ") + error.what()}));
      }
      job->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception&) {
    message_ = "Cannot start the sound import";
    return false;
  }
  import_ = std::move(job);
  message_.clear();
  poll_timer_.start();
  return true;
}

void SessionNativeSynthAdapter::poll() {
  if (decode_job_ && decode_job_->done.load(std::memory_order_acquire))
    finish_decode();
  if (import_ && import_->done.load(std::memory_order_acquire))
    finish_import();
  if (!decode_job_ && !import_)
    poll_timer_.stop();
}

void SessionNativeSynthAdapter::finish_decode() {
  if (decode_worker_.joinable())
    decode_worker_.join();
  const auto job = std::move(decode_job_);
  if (job->result && *job->result) {
    auto asset = std::make_shared<SynthMediaAsset>();
    asset->source_rate = job->result->value().source_rate;
    asset->frames = std::move(job->result->value().frames);
    remember(job->identity, std::move(asset));
  } else {
    decode_failure_identity_ = job->identity;
    decode_failure_ = job->result ? job->result->error().message : "Sample decode failed";
  }
  if (media_listener_)
    media_listener_();
}

void SessionNativeSynthAdapter::finish_import() {
  if (import_worker_.joinable())
    import_worker_.join();
  const auto job = std::move(import_);
  bool imported = false;
  if (!job->prepared || !*job->prepared) {
    message_ = job->prepared ? job->prepared->error().message : "Sound import failed";
  } else if (!job->plan || !*job->plan) {
    message_ = job->plan ? job->plan->error().message : "Sound import failed";
  } else if (const auto* patch = saved_patch(); patch == nullptr) {
    message_ = unavailable_reason();
  } else {
    auto& plan = job->plan->value();
    std::optional<application::NativeSynthMediaState> state;
    bool valid = true;
    if (plan.values || plan.labels) {
      application::NativeSynthMediaState next{patch->parameters, patch->document.labels};
      if (plan.values) {
        auto sparse = native_dsp::synth::make_native_patch(*engine_, *plan.values, patch->document);
        if (sparse) {
          next.values = std::move(sparse.value().parameters);
        } else {
          message_ = sparse.error().message;
          valid = false;
        }
      }
      if (plan.labels)
        next.labels = std::move(*plan.labels);
      state = std::move(next);
    }
    if (valid) {
      auto identity = job->prepared->value().source().sha256;
      const auto before = session_.snapshot().revision;
      auto finished = session_.imports().finish_native_synth_media_import(
          std::move(job->prepared->value()), std::move(plan.regions), std::move(state));
      // The session saved the new source with the edit.
      if (finished) {
        message_.clear();
        remember(std::move(identity), job->audio);
        imported = true;
      } else {
        message_ = finished.error().message;
      }
      const auto after = session_.snapshot().revision;
      if (after != before && after_edit_)
        after_edit_(after);
    }
  }
  if (job->done_callback)
    job->done_callback(imported);
  if (media_listener_)
    media_listener_();
}

bool SessionNativeSynthAdapter::add_midi_clip(std::int64_t bar, timeline::TickPosition length,
                                              std::vector<project::MidiNote> notes) {
  if (slot_) {
    message_ = "Effects have no clips";
    return false;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  if (bar < 0 || bar == std::numeric_limits<std::int64_t>::max()) {
    message_ = "Choose a bar in the song";
    return false;
  }
  // Zero-based bars on the project's meter map, as automation places them.
  const auto start = session_.snapshot().meter.tick_at({bar + 1, 1, 0});
  if (!start) {
    message_ = start.error().message;
    return false;
  }
  gesture_.reset();
  const auto before = session_.snapshot().revision;
  auto added = session_.midi_clips().add_midi_clip_with_notes(track_, start.value(), length,
                                                              std::move(notes));
  if (!added) {
    message_ = added.error().message;
    return false;
  }
  accepted(before);
  return true;
}

std::vector<std::string> SessionNativeSynthAdapter::midi_clip_ids() const {
  std::vector<std::string> ids;
  if (slot_)
    return ids;
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track == tracks.end())
    return ids;
  ids.reserve(track->midi_clips.size());
  for (const auto& clip : track->midi_clips)
    ids.push_back(clip.id.to_string());
  return ids;
}

std::optional<NativeSynthClipWindow>
SessionNativeSynthAdapter::first_clip_window(std::int64_t bars) const {
  // A looped pattern repeats at most this often in the window, however short.
  constexpr std::int64_t max_window_repeats = 1'024;
  if (slot_ || bars <= 0)
    return std::nullopt;
  const auto& snapshot = session_.snapshot();
  const auto track = std::ranges::find(snapshot.tracks, track_, &project::Track::id);
  if (track == snapshot.tracks.end())
    return std::nullopt;
  const project::MidiClip* first = nullptr;
  const project::MusicalInterval* placed = nullptr;
  for (const auto& clip : track->midi_clips) {
    const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement);
    if (musical != nullptr && (placed == nullptr || musical->start < placed->start)) {
      first = &clip;
      placed = musical;
    }
  }
  if (first == nullptr)
    return std::nullopt;
  // Bars as the synth player finds them (native_dsp::synth::BarGrid).
  const native_dsp::synth::BarGrid grid(snapshot.meter);
  const auto quarter = static_cast<double>(timeline::ticks_per_quarter);
  double at = 0.0;
  double beats_per_bar = 0.0;
  grid.locate(placed->start.value, at, beats_per_bar);
  const auto first_bar = static_cast<std::int64_t>(std::floor(at));
  const auto from = snapshot.meter.tick_at({first_bar + 1, 1, 0});
  const auto to = snapshot.meter.tick_at({first_bar + 1 + bars, 1, 0});
  if (!from || !to)
    return std::nullopt;
  NativeSynthClipWindow window;
  window.start_beat = static_cast<double>(from.value().value) / quarter;
  window.beats_per_bar = beats_per_bar;
  window.key = snapshot.key;
  if (const auto sample = snapshot.tempo.tick_to_sample(from.value(), snapshot.sample_rate)) {
    double bpm = 0.0;
    if (snapshot.tempo.bpm_at_sample_rt(sample.value(), snapshot.sample_rate, bpm))
      window.bpm = bpm;
  }
  const std::int64_t clip_end = placed->end.value;
  const std::int64_t limit = std::min(clip_end, to.value().value);
  const std::int64_t pattern = first->pattern_length.value;
  const bool repeats = first->loop && pattern > 0;
  for (std::int64_t repeat = 0; repeat < max_window_repeats; ++repeat) {
    const std::int64_t base = placed->start.value + repeat * (repeats ? pattern : 0);
    if (base >= limit)
      break;
    for (const auto& note : first->notes) {
      if (repeats && note.start.value >= pattern)
        continue;
      const std::int64_t start = base + note.start.value;
      if (start >= limit)
        continue;
      const std::int64_t end = std::min(base + note.end.value, clip_end);
      double bar_position = 0.0;
      double note_beats_per_bar = 0.0;
      grid.locate(start, bar_position, note_beats_per_bar);
      const double bar = std::floor(bar_position);
      window.notes.push_back({static_cast<double>(start) / quarter,
                              (bar_position - bar) * note_beats_per_bar,
                              static_cast<std::int64_t>(bar),
                              static_cast<double>(std::max<std::int64_t>(0, end - start)) / quarter,
                              note.key, note.velocity});
    }
    if (!repeats)
      break;
  }
  std::ranges::stable_sort(window.notes, {}, &NativeSynthClipNote::beat);
  return window;
}

bool SessionNativeSynthAdapter::replace_midi_clip(std::string_view clip,
                                                  timeline::TickPosition length,
                                                  std::vector<project::MidiNote> notes) {
  if (slot_) {
    message_ = "Effects have no clips";
    return false;
  }
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  const auto ids = midi_clip_ids();
  const auto id = project::ClipId::parse(std::string(clip));
  if (!id || std::ranges::find(ids, clip) == ids.end()) {
    message_ = "Select a MIDI clip on this track";
    return false;
  }
  gesture_.reset();
  const auto before = session_.snapshot().revision;
  auto replaced =
      session_.midi_clips().replace_midi_clip_notes(id.value(), length, std::move(notes));
  if (!replaced) {
    message_ = replaced.error().message;
    return false;
  }
  accepted(before);
  return true;
}

bool SessionNativeSynthAdapter::submit_preset(std::size_t preset) {
  // Effects without documents load presets as values on the panel side.
  if (slot_ && effect_->preset_document == nullptr)
    return false;
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return true;
  }
  gesture_.reset();
  const auto before = session_.snapshot().revision;
  auto applied = slot_ ? session_.effects().set_native_effect_preset(path_, *slot_, preset)
                       : session_.instruments().select_native_instrument(
                             track_, engine_->descriptor->id, preset);
  if (!applied) {
    message_ = applied.error().message;
    return true;
  }
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
  save();
  return true;
}

void SessionNativeSynthAdapter::accepted(std::uint64_t before) {
  message_.clear();
  const auto after = session_.snapshot().revision;
  if (after != before && after_edit_)
    after_edit_(after);
  save();
}

// A synth's user preset is written as a built-in effect's (ADR-0058): the
// file holds an engine, its sparse values and its document, which is a
// native patch whichever kind of device it is; the engine id tells them
// apart.
std::optional<std::string> SessionNativeSynthAdapter::preset_text() const {
  const auto values = saved_values();
  if (!values)
    return std::nullopt;
  if (!slot_) {
    auto patch = native_dsp::synth::make_native_patch(*engine_, values.value(), document());
    if (!patch)
      return std::nullopt;
    project::NativeEffectPatch preset;
    preset.engine = patch.value().engine;
    preset.engine_version = patch.value().engine_version;
    preset.parameters = patch.value().parameters;
    preset.document = patch.value().document;
    return persistence::encode_native_effect_preset(preset);
  }
  auto patch = native_inserts::make_native_effect_patch(*effect_, values.value(), document());
  if (!patch)
    return std::nullopt;
  return persistence::encode_native_effect_preset(patch.value());
}

bool SessionNativeSynthAdapter::submit_preset_text(std::string_view text) {
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  auto patch = persistence::decode_native_effect_preset(text);
  if (!patch || patch.value().engine != descriptor_.id) {
    message_ =
        patch ? (slot_ ? "That preset is for another effect" : "That preset is for another synth")
              : patch.error().message;
    return false;
  }
  if (!slot_) {
    project::NativeInstrumentPatch instrument{patch.value().engine, patch.value().engine_version,
                                              patch.value().parameters, patch.value().document};
    // Checked whole before anything changes.
    if (auto resolved = native_dsp::synth::resolve_native_instrument(instrument); !resolved) {
      message_ = resolved.error().message;
      return false;
    }
    gesture_.reset();
    const auto before = session_.snapshot().revision;
    auto applied = session_.instruments().set_native_instrument_state(
        track_, std::move(instrument.parameters), std::move(instrument.document));
    if (!applied) {
      message_ = applied.error().message;
      return false;
    }
    accepted(before);
    return true;
  }
  auto resolved = native_inserts::resolve_native_effect(patch.value());
  if (!resolved) {
    message_ = resolved.error().message;
    return false;
  }
  gesture_.reset();
  const auto before = session_.snapshot().revision;
  auto applied = session_.effects().set_native_effect_state(path_, *slot_, resolved.value().values,
                                                            std::move(patch).value().document);
  if (!applied) {
    message_ = applied.error().message;
    return false;
  }
  accepted(before);
  return true;
}

std::size_t SessionNativeSynthAdapter::linked_count() const {
  if (!slot_)
    return 1;
  return std::max<std::size_t>(1, session_.effects().native_effect_link_count(*slot_));
}

std::optional<project::MacroDestination>
SessionNativeSynthAdapter::macro_destination(std::string_view parameter_id) const {
  if (!slot_)
    return project::NativeParameterTarget{track_, std::string(parameter_id)};
  if (!std::holds_alternative<project::TrackId>(path_))
    return std::nullopt;
  return project::NativeEffectParameterTarget{*slot_, std::string(parameter_id)};
}

std::optional<std::uint32_t>
SessionNativeSynthAdapter::macro_of(std::string_view parameter_id) const {
  const auto destination = macro_destination(parameter_id);
  if (!destination)
    return std::nullopt;
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track == tracks.end())
    return std::nullopt;
  for (std::uint32_t index = 0; index < track->macros.size(); ++index) {
    if (std::ranges::any_of(track->macros[index].mappings,
                            [&](const project::MacroMapping& mapping) {
                              return mapping.destination == *destination;
                            }))
      return index;
  }
  return std::nullopt;
}

bool SessionNativeSynthAdapter::map_to_macro(std::string_view parameter_id, std::uint32_t index,
                                             double minimum, double maximum) {
  if (const auto reason = unavailable_reason(); !reason.empty()) {
    message_ = reason;
    return false;
  }
  const auto destination = macro_destination(parameter_id);
  if (!destination) {
    message_ = "Macros belong to a track; the master's effects have none";
    return false;
  }
  const auto before = session_.snapshot().revision;
  // A parameter follows one macro: moving it is unmap, then map.
  if (const auto current = macro_of(parameter_id)) {
    if (*current == index) {
      message_.clear();
      return true;
    }
    if (auto removed =
            session_.track_devices().remove_macro_mapping(track_, *current, *destination);
        !removed) {
      message_ = removed.error().message;
      return false;
    }
  }
  if (auto added =
          session_.track_devices().add_macro_mapping(track_, index, *destination, minimum, maximum);
      !added) {
    message_ = added.error().message;
    if (session_.snapshot().revision != before)
      accepted(before);
    return false;
  }
  accepted(before);
  return true;
}

bool SessionNativeSynthAdapter::unmap_macro(std::string_view parameter_id) {
  const auto destination = macro_destination(parameter_id);
  const auto current = macro_of(parameter_id);
  if (!destination || !current)
    return false;
  const auto before = session_.snapshot().revision;
  if (auto removed = session_.track_devices().remove_macro_mapping(track_, *current, *destination);
      !removed) {
    message_ = removed.error().message;
    return false;
  }
  accepted(before);
  return true;
}

std::vector<std::pair<std::string, std::uint32_t>>
SessionNativeSynthAdapter::macro_bindings() const {
  std::vector<std::pair<std::string, std::uint32_t>> bindings;
  // Any parameter id stands in for the device's own destinations.
  const auto probe = macro_destination({});
  if (!probe)
    return bindings;
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track == tracks.end())
    return bindings;
  for (std::uint32_t index = 0; index < track->macros.size(); ++index) {
    for (const auto& mapping : track->macros[index].mappings) {
      if (const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
        if (!slot_ && native->track == track_)
          bindings.emplace_back(native->parameter_id, index);
      } else if (slot_) {
        const auto& effect = std::get<project::NativeEffectParameterTarget>(mapping.destination);
        if (effect.slot == *slot_)
          bindings.emplace_back(effect.parameter_id, index);
      }
    }
  }
  return bindings;
}

void SessionNativeSynthAdapter::show_macro_values(const std::vector<double>* values) {
  if (gesture_)
    return;
  if (values != nullptr && values->size() == descriptor_.parameters.size())
    live_values_ = *values;
  else
    live_values_.reset();
}

void SessionNativeSynthAdapter::unlink() {
  if (!slot_ || !unavailable_reason().empty())
    return;
  const auto before = session_.snapshot().revision;
  if (auto applied = session_.effects().unlink_native_effect(path_, *slot_); !applied) {
    message_ = applied.error().message;
    return;
  }
  accepted(before);
}

NativeSynthPanelStatus SessionNativeSynthAdapter::status() const {
  NativeSynthPanelStatus status;
  status.saved_revision = session_.snapshot().revision;
  status.persistence_pending = session_.saving().has_unsaved_changes();
  auto reason = unavailable_reason();
  if (reason.empty()) {
    if (const auto values = saved_values(); !values) {
      reason = values.error().message;
    }
  }
  status.editable = reason.empty();
  status.message = reason.empty() ? message_ : reason;
  const auto audible = audible_ ? audible_() : std::nullopt;
  status.audible_revision = audible ? audible->revision : status.saved_revision;
  status.deferred_changes = audible && audible->deferred_changes;
  return status;
}

} // namespace oscillate::quick
