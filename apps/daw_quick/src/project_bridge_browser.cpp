#include "project_bridge.hpp"

#include "browser_preview_controller.hpp"
#include "device_catalog.hpp"
#include "drum_pads.hpp"
#include "key_match_controller.hpp"
#include "waveform_controller.hpp"

#include "native_synth_audition.hpp"
#include "oscillate/application/browser_preview.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <QSettings>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The browser (ADR-0046): one card per built-in instrument and effect, each
// with a one-line description and its presets attached for the full
// browser; the bundled starter sounds; favourites and recents; one-step
// creation of a track with a chosen sound, and a dry preview: a short
// phrase that suits each preset, and the user's samples in time with the
// song (ADR-0059).
namespace oscillate::quick {
namespace {

// The engine's preset named Init, if it has one.
std::optional<std::size_t> init_preset(const native_dsp::synth::EngineDescriptor& table) {
  for (std::size_t i = 0; i < table.presets.size(); ++i) {
    if (table.presets[i].name == "Init")
      return i;
  }
  return std::nullopt;
}

[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// A bundled starter sound is a Crate preset (ADR-0063, spec 09 §2.5): the
// engine's own one-shot as one chop, found by the preset's name.
constexpr std::string_view crate_engine = native_dsp::synth::crate::engine_id;
struct Starter {
  std::string_view file;
  std::string_view name;
  std::string_view tag;
};
constexpr std::array starters{Starter{"kick", "Kick", "Drums"}, Starter{"snare", "Snare", "Drums"},
                              Starter{"hat", "Hat", "Drums"}, Starter{"pluck", "Pluck", "Melodic"}};

[[nodiscard]] std::optional<std::size_t> starter_preset(const Starter& starter) {
  const auto* engine = native_dsp::synth::find_engine(crate_engine);
  if (engine == nullptr || engine->descriptor == nullptr)
    return std::nullopt;
  const auto& presets = engine->descriptor->presets;
  const auto found =
      std::ranges::find(presets, starter.name, &native_dsp::synth::SynthPreset::name);
  if (found == presets.end())
    return std::nullopt;
  return static_cast<std::size_t>(std::distance(presets.begin(), found));
}

constexpr auto favourites_key = "browser/favourites";
constexpr auto recents_key = "browser/recents";
constexpr qsizetype max_recents = 24;

[[nodiscard]] QVariantMap card(const QString& key, const QString& engine_id,
                               const QString& engine_name, int preset_index,
                               const QString& preset_name, const QString& tag,
                               const DeviceLook* about, const QString& fallback_category,
                               const QString& fallback_kind, const QString& fallback_about) {
  const auto category = about ? text(about->category) : fallback_category;
  return QVariantMap{
      {QStringLiteral("key"), key},
      {QStringLiteral("engineId"), engine_id},
      {QStringLiteral("engineName"), engine_name},
      {QStringLiteral("presetIndex"), preset_index},
      {QStringLiteral("presetName"), preset_name},
      {QStringLiteral("tag"), tag},
      {QStringLiteral("category"), category},
      {QStringLiteral("kind"), about ? text(about->kind) : fallback_kind},
      {QStringLiteral("color"), about ? text(about->color) : QStringLiteral("#9caab8")},
      {QStringLiteral("blurb"),
       about && !about->blurb.empty() ? text(about->blurb) : fallback_about},
      {QStringLiteral("about"),
       about && !about->about.empty() ? text(about->about) : fallback_about},
      // What it is and where it is listed (device_catalog.hpp).
      {QStringLiteral("section"), about ? text(section_name(about->section)) : category},
      {QStringLiteral("sectionRank"), about ? static_cast<int>(about->section) : -1},
      {QStringLiteral("rank"), about ? static_cast<int>(device_rank(about->id).value_or(0)) : -1},
      {QStringLiteral("abbreviation"), about ? text(about->abbreviation) : QString()},
      {QStringLiteral("keywords"), about ? text(about->keywords) : QString()},
      {QStringLiteral("instrument"),
       category == QStringLiteral("Instruments") || category == QStringLiteral("Drums")},
      {QStringLiteral("source"), QStringLiteral("native")}};
}

} // namespace

// The catalogue is fixed for the life of the bridge (CONSTANT), so it is
// built once: QML re-reads a list property on every element access.
QVariantList ProjectBridge::browser_catalogue() const {
  if (!browser_catalogue_cache_)
    browser_catalogue_cache_ = build_browser_catalogue();
  return *browser_catalogue_cache_;
}

QVariantList ProjectBridge::build_browser_catalogue() const {
  QVariantList cards;
  // One card per engine: double-click makes its default sound; its presets
  // ride along for the full browser's "start from a preset".
  const auto engine_card = [](const auto& table, const QString& fallback_category,
                              const QString& fallback_kind, const QString& fallback_about) {
    const auto* about = device_look(table.id);
    const auto id = text(table.id);
    const auto name = text(table.name);
    auto entry = card(id, id, name, -1, name, about ? text(about->kind) : fallback_kind, about,
                      fallback_category, fallback_kind, fallback_about);
    QVariantList presets;
    for (std::size_t index = 0; index < table.presets.size(); ++index) {
      presets.push_back(
          QVariantMap{{QStringLiteral("index"), static_cast<int>(index)},
                      {QStringLiteral("name"), text(table.presets[index].name)},
                      {QStringLiteral("category"), text(table.presets[index].category)}});
    }
    entry.insert(QStringLiteral("presets"), presets);
    return entry;
  };
  for (const auto* engine : native_dsp::synth::engines()) {
    cards.push_back(engine_card(*engine->descriptor, QStringLiteral("Instruments"),
                                QStringLiteral("Synth"), QStringLiteral("A built-in instrument.")));
  }
  for (const auto* engine : native_dsp::effects::engines()) {
    auto entry = engine_card(*engine->descriptor, QStringLiteral("Effects"),
                             QStringLiteral("Effect"), QStringLiteral("A built-in audio effect."));
    entry.insert(QStringLiteral("readsSidechain"), engine->sidechain);
    cards.push_back(std::move(entry));
  }
  for (std::size_t index = 0; index < starters.size(); ++index) {
    const auto& starter = starters[index];
    auto entry =
        card(QStringLiteral("starter:") + text(starter.file), text(crate_engine),
             QStringLiteral("Crate"), static_cast<int>(index), text(starter.name),
             text(starter.tag), nullptr, QStringLiteral("Samples"), QStringLiteral("One-shot"),
             QStringLiteral("A bundled one-shot played by Crate."));
    entry.insert(QStringLiteral("instrument"), true);
    entry.insert(QStringLiteral("color"), QStringLiteral("#d6c38f"));
    entry.insert(QStringLiteral("source"), QStringLiteral("starter"));
    cards.push_back(std::move(entry));
  }
  return cards;
}

QStringList ProjectBridge::browser_favourites() const {
  return QSettings{}.value(QString::fromLatin1(favourites_key)).toStringList();
}

QStringList ProjectBridge::browser_recents() const {
  return QSettings{}.value(QString::fromLatin1(recents_key)).toStringList();
}

void ProjectBridge::toggle_browser_favourite(const QString& key) {
  if (key.isEmpty())
    return;
  auto favourites = browser_favourites();
  if (!favourites.removeOne(key))
    favourites.prepend(key);
  QSettings{}.setValue(QString::fromLatin1(favourites_key), favourites);
  emit browserCollectionsChanged();
}

void ProjectBridge::note_browser_use(const QString& key) {
  if (key.isEmpty())
    return;
  auto recents = browser_recents();
  recents.removeAll(key);
  recents.prepend(key);
  while (recents.size() > max_recents)
    recents.removeLast();
  QSettings{}.setValue(QString::fromLatin1(recents_key), recents);
  emit browserCollectionsChanged();
}

void ProjectBridge::create_native_instrument_track(const QString& engine_id, int preset_index) {
  if (!session_ || control_busy() || export_busy() || import_busy()) {
    set_status(QStringLiteral("Wait for the current project operation to finish"));
    return;
  }
  const auto* engine = native_dsp::synth::find_engine(engine_id.toStdString());
  if (!engine) {
    set_status(QStringLiteral("Unknown built-in instrument"));
    return;
  }
  const auto& table = *engine->descriptor;
  if (preset_index < -1 || preset_index >= static_cast<int>(table.presets.size())) {
    set_status(QStringLiteral("Unknown preset for %1").arg(text(table.name)));
    return;
  }
  // The track arrives with its sound: one undo step removes it. With no
  // preset chosen it starts from the engine's Init, as a new instance of a
  // synth does, so the panel names it.
  const auto preset = preset_index < 0
                          ? init_preset(table)
                          : std::optional<std::size_t>{static_cast<std::size_t>(preset_index)};
  const auto track = session_->instruments().add_native_instrument_track(table.id, preset);
  if (!track) {
    set_status(QString::fromStdString(track.error().message));
    return;
  }
  refresh();
  submit_playback_edit();
  schedule_background_save();
  emit trackCreated(static_cast<int>(session_->tracks().track_index(track.value()).value_or(0)));
  const auto sound = preset ? text(table.presets[*preset].name) : text(table.name);
  note_browser_use(engine_id);
  set_status(QStringLiteral("New track with %1 · %2").arg(text(table.name), sound));
}

void ProjectBridge::replace_native_instrument(const QString& track_id, const QString& engine_id,
                                              int preset_index) {
  if (!session_ || control_busy() || export_busy() || import_busy()) {
    set_status(QStringLiteral("Wait for the current project operation to finish"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  const auto* engine = native_dsp::synth::find_engine(engine_id.toStdString());
  if (!parsed || !engine) {
    set_status(QStringLiteral("Select an instrument track and a built-in instrument"));
    return;
  }
  const auto& table = *engine->descriptor;
  if (preset_index < -1 || preset_index >= static_cast<int>(table.presets.size())) {
    set_status(QStringLiteral("Unknown preset for %1").arg(text(table.name)));
    return;
  }
  const auto preset = preset_index < 0
                          ? std::optional<std::size_t>{}
                          : std::optional<std::size_t>{static_cast<std::size_t>(preset_index)};
  const auto selected =
      session_->instruments().select_native_instrument(parsed.value(), table.id, preset);
  if (!selected) {
    set_status(QString::fromStdString(selected.error().message));
    return;
  }
  close_native_synth_editor();
  refresh();
  submit_playback_edit();
  schedule_background_save();
  const auto sound = preset ? text(table.presets[*preset].name) : text(table.name);
  note_browser_use(engine_id);
  set_status(QStringLiteral("%1 · %2 on the selected track").arg(text(table.name), sound));
}

void ProjectBridge::add_effect_preset(const QString& path_id, int index, const QString& engine_id,
                                      int preset_index) {
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  if (!insert_chain_edit_enabled() && !live_chain_structure_edit_enabled(path_id)) {
    set_status(QStringLiteral("Stop playback and finish current work before editing inserts"));
    return;
  }
  const auto* engine = native_dsp::effects::find_engine(engine_id.toStdString());
  if (!engine) {
    set_status(QStringLiteral("Unknown built-in effect"));
    return;
  }
  const auto& table = *engine->descriptor;
  if (index < 0 || preset_index < -1 || preset_index >= static_cast<int>(table.presets.size())) {
    set_status(QStringLiteral("Select a valid insert position and preset"));
    return;
  }
  std::optional<project::SignalPathId> path;
  if (path_id == QStringLiteral("master")) {
    path = project::MasterSignalPath{};
  } else if (const auto parsed = project::TrackId::parse(path_id.toStdString());
             parsed &&
             std::ranges::find(session_->snapshot().tracks, parsed.value(), &project::Track::id) !=
                 session_->snapshot().tracks.end()) {
    path = project::SignalPathId{parsed.value()};
  }
  if (!path) {
    set_status(QStringLiteral("Select a track or the master"));
    return;
  }
  const auto preset = preset_index < 0
                          ? std::optional<std::size_t>{}
                          : std::optional<std::size_t>{static_cast<std::size_t>(preset_index)};
  const auto added = session_->effects().add_effect_engine(*path, static_cast<std::size_t>(index),
                                                           table.id, preset);
  const auto sound = preset ? text(table.presets[*preset].name) : text(table.name);
  if (added)
    note_browser_use(engine_id);
  finish_insert_chain_edit(added ? foundation::Result<void>::success()
                                 : foundation::Result<void>::failure(added.error()),
                           QStringLiteral("%1 · %2 added").arg(text(table.name), sound));
}

void ProjectBridge::create_starter_sound_track(int sound_index) {
  if (sound_index < 0 || sound_index >= static_cast<int>(starters.size())) {
    set_status(QStringLiteral("Choose a bundled sound"));
    return;
  }
  if (!session_ || control_busy() || export_busy() || import_busy()) {
    set_status(QStringLiteral("Wait for the current project operation to finish"));
    return;
  }
  const auto& starter = starters[static_cast<std::size_t>(sound_index)];
  const auto preset = starter_preset(starter);
  if (!preset) {
    set_status(QStringLiteral("Bundled starter sound unavailable"));
    return;
  }
  const auto track = session_->instruments().add_native_instrument_track(crate_engine, preset);
  if (!track) {
    set_status(QString::fromStdString(track.error().message));
    return;
  }
  const auto created = track.value();
  refresh();
  emit trackCreated(static_cast<int>(session_->tracks().track_index(created).value_or(0)));
  note_browser_use(QStringLiteral("starter:") + text(starter.file));
  submit_playback_edit();
  set_status(QStringLiteral("%1 on Crate · revision %2").arg(text(starter.name)).arg(revision_));
}

void ProjectBridge::bind_starter_sound(const QString& track_id, int sound_index) {
  if (control_busy()) {
    set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                    : QStringLiteral("Wait for the audio edit to finish"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || sound_index < 0 || sound_index >= static_cast<int>(starters.size())) {
    set_status(QStringLiteral("Select an instrument track and a bundled sound"));
    return;
  }
  const auto& starter = starters[static_cast<std::size_t>(sound_index)];
  const auto preset = starter_preset(starter);
  if (!preset) {
    set_status(QStringLiteral("Bundled starter sound unavailable"));
    return;
  }
  const auto& tracks = session_->snapshot().tracks;
  const auto found = std::ranges::find(tracks, parsed.value(), &project::Track::id);
  const auto* native = found == tracks.end()
                           ? nullptr
                           : std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
  const bool other_engine = native == nullptr || native->engine != crate_engine;
  // One undo step: Crate at the starter's preset, its sample as one chop.
  const auto result =
      session_->instruments().select_native_instrument(parsed.value(), crate_engine, preset);
  if (result && other_engine)
    close_native_synth_editor();
  refresh();
  submit_playback_edit();
  set_status(
      result ? QStringLiteral("%1 on Crate · revision %2").arg(text(starter.name)).arg(revision_)
             : QStringLiteral("Starter sound: %1")
                   .arg(QString::fromStdString(result.error().message)));
}

void ProjectBridge::create_browser_previews() {
  browser_preview_ = std::make_unique<BrowserPreviewController>(
      [this]() -> std::optional<BrowserPreviewController::Song> {
        if (!session_)
          return std::nullopt;
        const auto& snapshot = session_->snapshot();
        BrowserPreviewController::Song song;
        song.sample_rate = snapshot.sample_rate;
        const auto playhead = static_cast<std::int64_t>(playhead_sample_);
        if (!snapshot.tempo.bpm_at_sample_rt({playhead}, snapshot.sample_rate, song.bpm))
          song.bpm = 120.0;
        if (transport_running())
          song.clock = SamplePreview::BeatClock{snapshot.tempo, snapshot.sample_rate,
                                                playhead_sample_, std::chrono::steady_clock::now()};
        return song;
      },
      [this](application::PlaybackOutputFactory& factory,
             std::optional<std::uint64_t>& target_serial) {
        return preview_output(factory, target_serial);
      },
      [this] { return audio_output_->buffer_frames(); },
      [this](QString message) { set_status(std::move(message)); }, OSCILLATE_HAVE_CODEC != 0, this);
  key_match_ = std::make_unique<KeyMatchController>(
      [this] { return session_.get(); }, [this] { return waveform_->root(); },
      [this] {
        return !control_busy() && !playback_completion_ && !export_busy() && !import_busy();
      },
      [this](QString message) { set_status(std::move(message)); },
      [this] {
        refresh();
        submit_audio_clip_edit();
      },
      KeyMatchController::Analyzer{}, this);
}

QObject* ProjectBridge::browser_preview() const noexcept { return browser_preview_.get(); }
QObject* ProjectBridge::key_match() const noexcept { return key_match_.get(); }

void ProjectBridge::preview_browser_preset(const QString& engine_id, int preset_index) {
  if (!session_) {
    set_status(QStringLiteral("Open a project to preview sounds"));
    return;
  }
  const auto* engine = native_dsp::synth::find_engine(engine_id.toStdString());
  if (!engine) {
    set_status(QStringLiteral("Previews play built-in instruments"));
    return;
  }
  const auto& table = *engine->descriptor;
  const auto* preset = preset_index >= 0 && preset_index < static_cast<int>(table.presets.size())
                           ? &table.presets[static_cast<std::size_t>(preset_index)]
                           : nullptr;
  auto values = preset ? native_dsp::synth::preset_values(table, *preset)
                       : foundation::Result<std::vector<double>>::success(
                             native_dsp::synth::default_values(table));
  if (!values) {
    set_status(QString::fromStdString(values.error().message));
    return;
  }
  if (browser_preview_)
    browser_preview_->stop();
  // A phrase that shows the sound off (a beat, a riff, a line or chords),
  // at the song's tempo; while the song plays it starts on the next beat.
  const auto& snapshot = session_->snapshot();
  const auto playhead = static_cast<std::int64_t>(playhead_sample_);
  double bpm = 120.0;
  if (!snapshot.tempo.bpm_at_sample_rt({playhead}, snapshot.sample_rate, bpm))
    bpm = 120.0;
  const auto style =
      application::preview_style(table.id, preset ? preset->category : std::string_view{},
                                 preset ? preset->name : std::string_view{});
  // A drum-pad engine's beat lands on its own pads.
  const auto pads = drum_pad_layout(table.id);
  auto notes = pads ? application::preview_phrase(style, bpm, snapshot.sample_rate, pads->first_key)
                    : application::preview_phrase(style, bpm, snapshot.sample_rate);
  if (transport_running()) {
    const auto beat = application::next_beat_sample(snapshot.tempo, playhead, snapshot.sample_rate);
    if (beat && *beat > playhead)
      for (auto& note : notes)
        note.start_frame += static_cast<std::uint64_t>(*beat - playhead);
  }
  if (!ensure_native_synth_audition())
    return;
  application::PlaybackOutputFactory factory;
  std::optional<std::uint64_t> target_serial;
  if (!preview_output(factory, target_serial))
    return;
  if (!native_synth_audition_->phrase(*engine, std::move(values).value(), snapshot.sample_rate,
                                      audio_output_->buffer_frames(), std::move(notes),
                                      target_serial, std::move(factory)))
    set_status(QStringLiteral("Instrument preview is busy; try again"));
}

void ProjectBridge::stop_browser_preview() {
  if (native_synth_audition_)
    native_synth_audition_->stop();
  if (browser_preview_)
    browser_preview_->stop();
}

} // namespace oscillate::quick
