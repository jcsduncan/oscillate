#include "project_bridge.hpp"

#include "anvil_panel_model.hpp"
#include "drum_pads.hpp"
#include "jack_playback_output.hpp"
#include "knock_panel_model.hpp"
#include "knock_patterns_model.hpp"
#include "native_synth_audition.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/source_prep/managed_audio.hpp"
#include "oscillate/source_prep/synth_media.hpp"

#include <QTimer>

#include <algorithm>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::quick {

QObject* ProjectBridge::native_synth_controller() const noexcept {
  return native_synth_editor_open_ ? native_synth_controller_.get() : nullptr;
}

QObject* ProjectBridge::native_synth_automation() const noexcept {
  return native_synth_editor_open_ ? native_synth_automation_.get() : nullptr;
}

QObject* ProjectBridge::native_synth_panel_model() const noexcept {
  return native_synth_editor_open_ ? native_synth_panel_model_.get() : nullptr;
}

QString ProjectBridge::native_synth_track_id() const {
  return native_synth_editor_open_ && native_synth_adapter_
             ? QString::fromStdString(native_synth_adapter_->track().to_string())
             : QString{};
}

QVariantList ProjectBridge::native_synth_engines() const {
  QVariantList result;
  for (const auto* engine : native_dsp::synth::engines()) {
    result.push_back(
        QVariantMap{{"id", QString::fromStdString(std::string(engine->descriptor->id))},
                    {"name", QString::fromStdString(std::string(engine->descriptor->name))}});
  }
  return result;
}

QVariantList ProjectBridge::native_synth_note_lanes(const QString& engine_id,
                                                    const QString& track_id) const {
  namespace synth = native_dsp::synth;
  QVariantList lanes;
  const auto id = engine_id.toStdString();
  const auto layout = drum_pad_layout(id);
  if (!layout)
    return lanes;
  // Each pad's name, in key order.
  std::vector<QString> names;
  names.reserve(layout->pad_count);
  if (id == synth::pulse::engine().descriptor->id) {
    // Pulse's pads are its parameter groups (the kit's sounds), less the bus.
    std::vector<std::string_view> groups;
    for (const auto& parameter : synth::pulse::engine().descriptor->parameters) {
      if (parameter.group == "Bus" || parameter.group == "Output" ||
          std::ranges::find(groups, parameter.group) != groups.end())
        continue;
      groups.push_back(parameter.group);
    }
    if (groups.size() != layout->pad_count)
      return {};
    for (const auto group : groups)
      names.push_back(QString::fromUtf8(group.data(), static_cast<qsizetype>(group.size())));
  } else {
    // Knock's pads are named by their sound on the track, when there is one.
    std::vector<double> values;
    if (session_) {
      if (const auto track = project::TrackId::parse(track_id.toStdString())) {
        if (auto read = session_->instruments().native_instrument_values(track.value()))
          values = std::move(read).value();
      }
    }
    const auto& table = *synth::knock::engine().descriptor;
    for (std::size_t pad = 0; pad < layout->pad_count; ++pad) {
      const auto label = knock_pad_label(table, values, pad);
      names.push_back(label.empty() ? QStringLiteral("Pad %1").arg(static_cast<int>(pad + 1))
                                    : QString::fromStdString(label));
    }
  }
  for (std::size_t index = 0; index < names.size(); ++index) {
    lanes.push_back(
        QVariantMap{{QStringLiteral("key"), static_cast<int>(layout->first_key + index)},
                    {QStringLiteral("name"), names[index]}});
  }
  return lanes;
}

void ProjectBridge::close_native_synth_editor() {
  if (native_synth_audition_)
    native_synth_audition_->stop();
  if (!native_synth_controller_)
    return;
  native_synth_editor_open_ = false;
  emit nativeSynthControllerChanged();
  native_synth_automation_.reset();
  native_synth_knock_ = nullptr;
  native_synth_panel_model_.reset();
  native_synth_controller_.reset();
  native_synth_adapter_.reset();
}

void ProjectBridge::audition_native_synth_note(int key) {
  // A drum-pad engine's pads (Pulse, Knock), or Anvil's hit on any key.
  const auto engine_id =
      native_synth_adapter_ ? native_synth_adapter_->engine().descriptor->id : std::string_view{};
  const auto pads = drum_pad_layout(engine_id);
  const bool drum_pad = pads && pads->contains(key);
  const bool anvil_hit = engine_id == "anvil" && key >= 0 && key <= 127;
  if (!session_ || !native_synth_editor_open_ || (!drum_pad && !anvil_hit)) {
    set_status(QStringLiteral("Select a pad to preview"));
    return;
  }
  if (anvil_hit) {
    // Through the track's graph, so the hit has its curves, transient and
    // effects; the engine plays the whole hit whatever the note's length.
    const auto track = QString::fromStdString(native_synth_adapter_->track().to_string());
    play_note(track, key, 110);
    QTimer::singleShot(250, this, [this, key] { release_note(key); });
    return;
  }
  audition_native_synth(native_synth_adapter_->track(), native_synth_adapter_->engine(),
                        static_cast<std::uint8_t>(key), 100);
}

void ProjectBridge::audition_knock_pattern() {
  auto* patterns = native_synth_knock_ ? native_synth_knock_->pattern_model() : nullptr;
  if (!session_ || !native_synth_editor_open_ || !native_synth_adapter_ ||
      !native_synth_controller_ || patterns == nullptr) {
    set_status(QStringLiteral("Open a Knock track to audition its pattern"));
    return;
  }
  if (transport_running()) {
    set_status(QStringLiteral("Stop the song to audition the pattern"));
    return;
  }
  // The pattern's opening at the song's tempo, dry, as the track's sound.
  const auto& snapshot = session_->snapshot();
  double bpm = application::knock_patterns::fallback_preview_bpm;
  if (!snapshot.tempo.bpm_at_sample_rt({static_cast<std::int64_t>(playhead_sample_)},
                                       snapshot.sample_rate, bpm))
    bpm = application::knock_patterns::fallback_preview_bpm;
  auto notes =
      patterns->audition_notes(bpm, snapshot.sample_rate, NativeSynthAudition::max_phrase_notes);
  if (notes.empty()) {
    set_status(QStringLiteral("The pattern is empty"));
    return;
  }
  if (!ensure_native_synth_audition())
    return;
  application::PlaybackOutputFactory factory;
  std::optional<std::uint64_t> target_serial;
  if (!preview_output(factory, target_serial))
    return;
  if (!native_synth_audition_->phrase(native_synth_adapter_->engine(),
                                      native_synth_controller_->current_values(),
                                      snapshot.sample_rate, audio_output_->buffer_frames(),
                                      std::move(notes), target_serial, std::move(factory)))
    set_status(QStringLiteral("Instrument preview is busy; try again"));
}

void ProjectBridge::preview_note_velocity(const QString& clip_id, const QString& note_id,
                                          int velocity) {
  if (velocity < 1 || velocity > 127)
    return;
  audition_edited_note(clip_id, note_id, static_cast<std::uint8_t>(velocity));
}

void ProjectBridge::audition_edited_note(const QString& clip_id, const QString& note_id,
                                         std::uint8_t velocity) {
  if (!session_)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto note = project::NoteId::parse(note_id.toStdString());
  if (!clip || !note)
    return;
  for (const auto& track : session_->snapshot().tracks) {
    const auto midi = std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id);
    if (midi == track.midi_clips.end())
      continue;
    const auto found = std::ranges::find(midi->notes, note.value(), &project::MidiNote::id);
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (found != midi->notes.end() && patch) {
      if (const auto* engine = native_dsp::synth::find_engine(patch->engine))
        audition_native_synth(track.id, *engine, found->key, velocity, true);
    }
    return;
  }
}

void ProjectBridge::audition_native_synth(project::TrackId track,
                                          const native_dsp::synth::SynthEngine& engine,
                                          std::uint8_t key, std::uint8_t velocity,
                                          bool replace_previous) {
  auto values = session_->instruments().native_instrument_values(track);
  if (!values) {
    set_status(QStringLiteral("Cannot preview instrument: %1")
                   .arg(QString::fromStdString(values.error().message)));
    return;
  }
  audition_values(engine, std::move(values).value(), key, velocity, replace_previous);
}

bool ProjectBridge::ensure_native_synth_audition() {
  if (native_synth_audition_)
    return true;
  try {
    native_synth_audition_ = std::make_unique<NativeSynthAudition>([this](std::string error) {
      QMetaObject::invokeMethod(
          this, [this, message = QString::fromStdString(error)] { set_status(message); },
          Qt::QueuedConnection);
    });
  } catch (const std::exception&) {
    set_status(QStringLiteral("Cannot start the instrument preview worker"));
    return false;
  }
  return true;
}

bool ProjectBridge::preview_output(application::PlaybackOutputFactory& factory,
                                   std::optional<std::uint64_t>& target_serial) {
  const bool jack = audio_output_->jack();
  factory = jack ? make_jack_playback_output_factory() : application::PlaybackOutputFactory{};
  if (jack && !factory) {
    set_status(QStringLiteral("JACK preview is unavailable in this build"));
    return false;
  }
  target_serial = audio_output_->target_serial();
  return true;
}

void ProjectBridge::audition_values(const native_dsp::synth::SynthEngine& engine,
                                    std::vector<double> values, std::uint8_t key,
                                    std::uint8_t velocity, bool replace_previous) {
  if (!session_ || !ensure_native_synth_audition())
    return;
  application::PlaybackOutputFactory factory;
  std::optional<std::uint64_t> target_serial;
  if (!preview_output(factory, target_serial))
    return;
  if (!native_synth_audition_->trigger(engine, std::move(values), session_->snapshot().sample_rate,
                                       audio_output_->buffer_frames(), key, velocity, target_serial,
                                       std::move(factory), replace_previous)) {
    set_status(QStringLiteral("Instrument preview is busy; try the note again"));
  }
}

void ProjectBridge::play_note(const QString& track_id, int key, int velocity) {
  if (!session_ || key < 0 || key > 127)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track)
    return;
  const auto& snapshot = session_->snapshot();
  const auto found = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  if (found == snapshot.tracks.end())
    return;
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
  if (patch == nullptr || native_dsp::synth::find_engine(patch->engine) == nullptr) {
    set_status(QStringLiteral("Live playing needs a built-in synth on the track"));
    return;
  }
  live_note_tracks_.insert_or_assign(key, track.value());
  // Through the project's graph: the playing transport's, or a monitor run
  // started for it, so the track's effects and the master chain hear it.
  if (!transport_running() && !monitor_ && !monitor_completion_)
    start_monitor();
  if (live_run() || monitor_completion_) {
    send_live_note(track_id, key, velocity, true);
    return;
  }
  play_note_dry(track_id, key, velocity);
}

void ProjectBridge::play_note_dry(const QString& track_id, int key, int velocity) {
  if (!session_ || key < 0 || key > 127)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track)
    return;
  const auto& snapshot = session_->snapshot();
  const auto found = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  if (found == snapshot.tracks.end())
    return;
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
  const auto* engine = patch ? native_dsp::synth::find_engine(patch->engine) : nullptr;
  if (engine == nullptr)
    return;
  auto values = session_->instruments().native_instrument_values(track.value());
  auto document = session_->instruments().native_synth_document(track.value());
  if (!values || !document) {
    set_status(QStringLiteral("Cannot play the instrument: %1")
                   .arg(QString::fromStdString(!values ? values.error().message
                                                       : document.error().message)));
    return;
  }
  if (!ensure_native_synth_audition())
    return;
  application::PlaybackOutputFactory factory;
  std::optional<std::uint64_t> target_serial;
  if (!preview_output(factory, target_serial))
    return;
  // Managed media decode on the preview worker, from a snapshot taken now.
  NativeSynthAudition::Sound sound{engine, std::move(values).value(), document.value(), {}};
  if (std::ranges::any_of(document.value().media, [](const project::NativeMediaSlot& slot) {
        return std::holds_alternative<project::ManagedAudioReference>(slot.media);
      })) {
    sound.load_media =
        [root = waveform_root_, snapshot = snapshot,
         doc = document.value()]() -> foundation::Result<native_dsp::synth::SynthMedia> {
      auto store = persistence::ProjectStore::open_reader(root);
      if (!store)
        return foundation::Result<native_dsp::synth::SynthMedia>::failure(store.error());
      return source_prep::load_synth_media(store.value(), snapshot, doc);
    };
  }
  if (!native_synth_audition_->note_on(
          std::move(sound), snapshot.sample_rate, audio_output_->buffer_frames(),
          static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)),
          target_serial, std::move(factory)))
    set_status(QStringLiteral("Instrument preview is busy; try the note again"));
}

void ProjectBridge::release_note(int key) {
  if (key < 0 || key > 127)
    return;
  if (const auto found = live_note_tracks_.find(key); found != live_note_tracks_.end()) {
    const auto track = found->second;
    live_note_tracks_.erase(found);
    send_live_note(QString::fromStdString(track.to_string()), key, 0, false);
  }
  if (native_synth_audition_)
    (void)native_synth_audition_->note_off(static_cast<std::uint8_t>(key));
}

QString ProjectBridge::clip_track_id(const QString& clip_id) const {
  if (!session_)
    return {};
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip)
    return {};
  for (const auto& track : session_->snapshot().tracks) {
    if (std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id) !=
        track.midi_clips.end())
      return QString::fromStdString(track.id.to_string());
  }
  return {};
}

void ProjectBridge::open_native_synth_editor(const QString& track_id) {
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select a built-in synth track"));
    return;
  }
  const auto& tracks = session_->snapshot().tracks;
  const auto found = std::ranges::find(tracks, parsed.value(), &project::Track::id);
  const auto* patch = found == tracks.end()
                          ? nullptr
                          : std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
  if (!patch) {
    set_status(QStringLiteral("This track has no built-in synth"));
    return;
  }
  const auto* engine = native_dsp::synth::find_engine(patch->engine);
  if (!engine) {
    set_status(QStringLiteral("Built-in synth %1 is unavailable")
                   .arg(QString::fromStdString(patch->engine)));
    return;
  }
  if (native_synth_adapter_ && native_synth_adapter_->track() == parsed.value() &&
      native_synth_adapter_->engine().descriptor->id == engine->descriptor->id) {
    native_synth_controller_->refresh();
    return;
  }
  close_native_synth_editor();
  native_synth_adapter_ = std::make_unique<SessionNativeSynthAdapter>(
      *session_, parsed.value(), *engine,
      [this](std::uint64_t) {
        refresh();
        submit_playback_edit();
      },
      [this, track = parsed.value()]() -> std::optional<AudibleSynthState> {
        if (!live_run())
          return std::nullopt;
        const auto deferred = live_run()->synth_deferred_changes(track);
        return AudibleSynthState{live_run()->status().active_graph_revision,
                                 deferred.value_or(false)};
      },
      [this, track = parsed.value()](std::vector<float>& frame) {
        return live_run() && live_run()->synth_telemetry(track, frame);
      },
      [this]() { schedule_background_save(); },
      [this, track = parsed.value(),
       descriptor = engine->descriptor](std::span<const double> values) {
        return live_run() && live_run()->set_live_synth_values(track, *descriptor, values);
      },
      // Managed media decode on the adapter's worker (Crate's waveform and
      // chopping), from a reader of this project's store.
      [root = waveform_root_](const project::SourceRecord& record, const std::stop_token& stop)
          -> foundation::Result<source_prep::DecodedAudio> {
        auto store = persistence::ProjectStore::open_reader(root);
        if (!store)
          return foundation::Result<source_prep::DecodedAudio>::failure(store.error());
        return source_prep::load_managed_audio(store.value(), record,
                                               project::max_native_synth_media_frames, stop);
      });
  native_synth_controller_ = std::make_unique<NativeSynthDeviceController>(
      *engine->descriptor, *native_synth_adapter_, this);
  share_macro_map(*native_synth_controller_);
  if (engine->descriptor->id == "anvil") {
    // The preview hears the user's transient as the track does.
    auto anvil = std::make_unique<AnvilPanelModel>(
        *native_synth_controller_,
        [root = waveform_root_, session = session_.get()](const project::NativeSynthDocument& doc)
            -> std::shared_ptr<const native_dsp::synth::SynthMedia> {
          auto store = persistence::ProjectStore::open_reader(root);
          if (!store)
            return nullptr;
          auto media = source_prep::load_synth_media(store.value(), session->snapshot(), doc);
          if (!media)
            return nullptr;
          return std::make_shared<const native_dsp::synth::SynthMedia>(std::move(media).value());
        },
        session_->snapshot().sample_rate, this);
    // The beat strip follows the song's tempo (at bar 1) as it changes.
    const auto follow_tempo = [this, model = anvil.get()] {
      double bpm = 0.0;
      if (session_ &&
          session_->snapshot().tempo.bpm_at_sample_rt({0}, session_->snapshot().sample_rate, bpm))
        model->set_tempo(bpm);
    };
    follow_tempo();
    connect(this, &ProjectBridge::revisionChanged, anvil.get(), follow_tempo);
    native_synth_panel_model_ = std::move(anvil);
  }
  if (engine->descriptor->id == native_dsp::synth::knock::engine().descriptor->id) {
    // Pad labels and the ringing modes, designed at the project's rate.
    // Its patterns write the track's clips through the same adapter.
    auto knock = std::make_unique<KnockPanelModel>(*native_synth_controller_,
                                                   session_->snapshot().sample_rate,
                                                   native_synth_adapter_.get(), this);
    native_synth_knock_ = knock.get();
    native_synth_panel_model_ = std::move(knock);
  }
  native_synth_automation_ =
      std::make_unique<NativeSynthAutomationController>(*session_, parsed.value(), *engine, this);
  native_synth_editor_open_ = true;
  emit nativeSynthControllerChanged();
}

void ProjectBridge::select_native_instrument(const QString& track_id, const QString& engine_id) {
  if (!session_ || control_busy() || export_busy() || import_busy()) {
    set_status(QStringLiteral("Wait for the current project operation to finish"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an instrument track"));
    return;
  }
  const auto* engine = native_dsp::synth::find_engine(engine_id.toStdString());
  if (!engine) {
    set_status(QStringLiteral("Unknown built-in synth"));
    return;
  }
  const auto selected =
      session_->instruments().select_native_instrument(parsed.value(), engine->descriptor->id);
  if (!selected) {
    set_status(QString::fromStdString(selected.error().message));
    return;
  }
  close_native_synth_editor();
  refresh();
  submit_playback_edit();
  schedule_background_save();
  const auto name = QString::fromStdString(std::string(engine->descriptor->name));
  set_status(QStringLiteral("%1 selected · save queued · revision %2").arg(name).arg(revision_));
  open_native_synth_editor(track_id);
}

} // namespace oscillate::quick
