#pragma once

// A test double for built-in synth panels: accepts every edit at once and
// records it.
#include "native_synth_panel_adapter.hpp"

namespace oscillate::quick {

// Accepts everything immediately and records what it received.
class InMemoryNativeSynthAdapter final : public NativeSynthPanelAdapter {
public:
  struct Submission {
    std::vector<double> values;
    NativeSynthGesture gesture;
  };

  explicit InMemoryNativeSynthAdapter(std::vector<double> values) : values_(std::move(values)) {}

  [[nodiscard]] std::vector<double> values() const override { return values_; }
  void submit(std::span<const double> values, NativeSynthGesture gesture) override {
    if (gesture == NativeSynthGesture::begin) {
      before_gesture_ = values_;
    }
    if (gesture == NativeSynthGesture::cancel) {
      values_ = before_gesture_;
    } else {
      values_.assign(values.begin(), values.end());
    }
    submissions_.push_back({values_, gesture});
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
  }
  [[nodiscard]] NativeSynthPanelStatus status() const override { return status_; }
  [[nodiscard]] project::NativeSynthDocument document() const override { return document_; }
  void submit_document(project::NativeSynthDocument document, NativeSynthGesture gesture) override {
    if (gesture == NativeSynthGesture::begin) {
      document_before_gesture_ = document_;
    }
    document_ =
        gesture == NativeSynthGesture::cancel ? document_before_gesture_ : std::move(document);
    document_submissions_.push_back({document_, gesture});
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
  }
  [[nodiscard]] bool telemetry(std::vector<float>& frame) override {
    frame = telemetry_;
    return !telemetry_.empty();
  }
  void submit_state(std::span<const double> values, project::NativeSynthDocument document,
                    NativeSynthGesture gesture) override {
    if (gesture == NativeSynthGesture::begin) {
      before_gesture_ = values_;
      document_before_gesture_ = document_;
    }
    if (gesture == NativeSynthGesture::cancel) {
      values_ = before_gesture_;
      document_ = document_before_gesture_;
    } else {
      values_.assign(values.begin(), values.end());
      document_ = std::move(document);
    }
    state_submissions_.push_back({values_, document_, gesture});
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
  }
  [[nodiscard]] NativeSynthMedia media(std::string_view slot) override {
    const auto found = media_.find(std::string(slot));
    return found == media_.end() ? NativeSynthMedia{} : found->second;
  }
  void set_media_listener(std::function<void()> listener) override {
    media_listener_ = std::move(listener);
  }
  // Pending until finish_media_import(); refused for an empty path or while
  // one is pending.
  bool begin_media_import(std::string_view slot, std::string_view path,
                          NativeSynthMediaPlanner plan, NativeSynthMediaDone done) override {
    if (path.empty() || pending_import_)
      return false;
    pending_import_ =
        PendingImport{std::string(slot), std::string(path), std::move(plan), std::move(done)};
    return true;
  }
  [[nodiscard]] bool media_import_busy() const override { return pending_import_.has_value(); }
  bool add_midi_clip(std::int64_t bar, timeline::TickPosition length,
                     std::vector<project::MidiNote> notes) override {
    clips_.push_back({bar, length, std::move(notes),
                      std::string(memory_clip_prefix) + std::to_string(clips_.size() + 1)});
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
    return true;
  }
  [[nodiscard]] std::vector<std::string> midi_clip_ids() const override {
    std::vector<std::string> ids;
    ids.reserve(clips_.size());
    for (const auto& clip : clips_)
      ids.push_back(clip.id);
    return ids;
  }
  // Clips here sit on a 4/4 grid with no meter map, at a set tempo and key.
  static constexpr double memory_beats_per_bar = 4.0;
  static constexpr double memory_default_bpm = 120.0;
  void set_tempo(double bpm) { bpm_ = bpm; }
  void set_project_key(std::optional<project::MusicalKey> key) { key_ = key; }
  [[nodiscard]] std::optional<NativeSynthClipWindow>
  first_clip_window(std::int64_t bars) const override {
    const auto first = std::ranges::min_element(clips_, {}, &Clip::bar);
    if (first == clips_.end() || bars <= 0)
      return std::nullopt;
    const auto quarter = static_cast<double>(timeline::ticks_per_quarter);
    NativeSynthClipWindow window;
    window.beats_per_bar = memory_beats_per_bar;
    window.start_beat = static_cast<double>(first->bar) * memory_beats_per_bar;
    window.bpm = bpm_;
    window.key = key_;
    const double end = static_cast<double>(
        std::min(first->length.value, static_cast<std::int64_t>(bars) *
                                          static_cast<std::int64_t>(memory_beats_per_bar) *
                                          timeline::ticks_per_quarter));
    for (const auto& note : first->notes) {
      if (static_cast<double>(note.start.value) >= end)
        continue;
      const double offset = static_cast<double>(note.start.value) / quarter;
      const double beat = window.start_beat + offset;
      const auto bar = static_cast<std::int64_t>(std::floor(beat / memory_beats_per_bar));
      const double until =
          std::min(static_cast<double>(note.end.value), static_cast<double>(first->length.value));
      window.notes.push_back({beat, beat - static_cast<double>(bar) * memory_beats_per_bar, bar,
                              (until - static_cast<double>(note.start.value)) / quarter, note.key,
                              note.velocity});
    }
    std::ranges::stable_sort(window.notes, {}, &NativeSynthClipNote::beat);
    return window;
  }
  // Replaces a clip add_midi_clip made; refused for any other id.
  bool replace_midi_clip(std::string_view clip, timeline::TickPosition length,
                         std::vector<project::MidiNote> notes) override {
    const auto found = std::ranges::find(clips_, clip, &Clip::id);
    if (found == clips_.end())
      return false;
    found->length = length;
    found->notes = std::move(notes);
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
    return true;
  }

  // Lands the pending import as if its worker decoded `audio` (with
  // `tempo`): the slot then holds it as managed media named by `identity`,
  // and the plan's regions, values and labels apply in one edit. Returns
  // what the import reported.
  bool finish_media_import(std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio,
                           std::string identity, std::optional<double> tempo = std::nullopt) {
    if (!pending_import_ || !audio)
      return false;
    auto pending = std::move(*pending_import_);
    pending_import_.reset();
    auto plan = pending.plan ? pending.plan(*audio, tempo)
                             : foundation::Result<NativeSynthMediaPlan>::success({});
    const bool imported = plan.has_value();
    if (imported) {
      auto& document = document_;
      project::ManagedAudioReference reference{
          project::SourceId::from_bits({0, 1}).value(),    identity, 0, audio->source_rate, 2,
          static_cast<std::uint64_t>(audio->frames.size())};
      const auto found =
          std::ranges::find(document.media, pending.slot, &project::NativeMediaSlot::id);
      if (found != document.media.end())
        found->media = reference;
      else
        document.media.push_back({pending.slot, reference, 0});
      if (plan.value().regions) {
        std::erase_if(document.regions, [&](const project::NativeRegion& region) {
          return region.media == pending.slot;
        });
        document.regions.insert(document.regions.end(), plan.value().regions->begin(),
                                plan.value().regions->end());
      }
      if (plan.value().values)
        values_ = *plan.value().values;
      if (plan.value().labels)
        document.labels = *plan.value().labels;
      media_[pending.slot] =
          NativeSynthMedia{std::move(audio), std::move(identity), false, false, tempo, {}};
      imports_.push_back({pending.slot, pending.path});
      ++status_.saved_revision;
      status_.audible_revision = status_.saved_revision;
    }
    if (pending.done)
      pending.done(imported);
    return imported;
  }
  // What media(slot) reports; announces it to the listener.
  void set_media(std::string slot, NativeSynthMedia media) {
    media_[std::move(slot)] = std::move(media);
    if (media_listener_)
      media_listener_();
  }

  void set_status(NativeSynthPanelStatus status) { status_ = std::move(status); }
  void set_telemetry(std::vector<float> frame) { telemetry_ = std::move(frame); }
  [[nodiscard]] const std::vector<Submission>& submissions() const { return submissions_; }
  struct DocumentSubmission {
    project::NativeSynthDocument document;
    NativeSynthGesture gesture;
  };
  [[nodiscard]] const std::vector<DocumentSubmission>& document_submissions() const {
    return document_submissions_;
  }
  struct StateSubmission {
    std::vector<double> values;
    project::NativeSynthDocument document;
    NativeSynthGesture gesture;
  };
  [[nodiscard]] const std::vector<StateSubmission>& state_submissions() const {
    return state_submissions_;
  }
  // Clips it holds are named this, then their number from 1.
  static constexpr std::string_view memory_clip_prefix = "memory-clip-";
  struct Clip {
    std::int64_t bar = 0;
    timeline::TickPosition length{0};
    std::vector<project::MidiNote> notes;
    std::string id;
  };
  [[nodiscard]] const std::vector<Clip>& clips() const { return clips_; }
  struct Import {
    std::string slot;
    std::string path;
  };
  [[nodiscard]] const std::vector<Import>& imports() const { return imports_; }

private:
  struct PendingImport {
    std::string slot;
    std::string path;
    NativeSynthMediaPlanner plan;
    NativeSynthMediaDone done;
  };

  std::vector<double> values_;
  std::vector<double> before_gesture_;
  std::vector<Submission> submissions_;
  project::NativeSynthDocument document_;
  project::NativeSynthDocument document_before_gesture_;
  std::vector<DocumentSubmission> document_submissions_;
  NativeSynthPanelStatus status_{};
  std::vector<float> telemetry_;
  std::vector<StateSubmission> state_submissions_;
  std::map<std::string, NativeSynthMedia, std::less<>> media_;
  std::function<void()> media_listener_;
  std::optional<PendingImport> pending_import_;
  std::vector<Clip> clips_;
  double bpm_ = memory_default_bpm;
  std::optional<project::MusicalKey> key_;
  std::vector<Import> imports_;
};

} // namespace oscillate::quick
