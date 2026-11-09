#include "project_bridge.hpp"

#include "device_catalog.hpp"
#include "device_strip_catalog.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "relay_panel_model.hpp"

#include <QTimer>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// Built-in effects in the app (ADR-0043): the browser catalogue, adding a
// slot, and the designed panel for one slot, on the same seam as the synths.
namespace oscillate::quick {
namespace {

[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// "master" or a track id naming an existing track.
[[nodiscard]] std::optional<project::SignalPathId> effect_path(const project::Snapshot& snapshot,
                                                               const QString& path_id) {
  if (path_id == QStringLiteral("master"))
    return project::MasterSignalPath{};
  const auto parsed = project::TrackId::parse(path_id.toStdString());
  if (!parsed || std::ranges::find(snapshot.tracks, parsed.value(), &project::Track::id) ==
                     snapshot.tracks.end())
    return std::nullopt;
  return project::SignalPathId{parsed.value()};
}

} // namespace

QVariantList ProjectBridge::native_effect_engines() const {
  QVariantList result;
  for (const auto* engine : native_dsp::effects::engines()) {
    const auto* about = device_look(engine->descriptor->id);
    result.push_back(QVariantMap{
        {QStringLiteral("id"), text(engine->descriptor->id)},
        {QStringLiteral("name"), text(engine->descriptor->name)},
        {QStringLiteral("utility"), about != nullptr && is_utility(*about)},
        {QStringLiteral("category"), about ? text(about->kind) : QStringLiteral("Effect")},
        {QStringLiteral("detail"),
         about ? text(about->detail) : QStringLiteral("Built-in audio effect")},
        {QStringLiteral("readsSidechain"), engine->sidechain},
        {QStringLiteral("generator"), engine->generator}});
  }
  return result;
}

QObject* ProjectBridge::native_effect_controller() const noexcept {
  return native_effect_controller_.get();
}

QObject* ProjectBridge::native_effect_automation() const noexcept {
  return native_effect_automation_.get();
}

QObject* ProjectBridge::native_effect_panel_model() const noexcept {
  return native_effect_panel_model_.get();
}

QVariantMap ProjectBridge::native_effect_context() const { return native_effect_context_; }

void ProjectBridge::add_effect_engine(const QString& path_id, int index, const QString& engine_id) {
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  // Adding a stage changes the chain's shape: while playing, the graph
  // switches at a held block boundary.
  if (!insert_chain_edit_enabled() && !live_chain_structure_edit_enabled(path_id)) {
    set_status(QStringLiteral("Stop playback and finish current work before editing inserts"));
    return;
  }
  const auto& snapshot = session_->snapshot();
  const auto path = effect_path(snapshot, path_id);
  if (!path || index < 0) {
    set_status(QStringLiteral("Select a valid path and insert position"));
    return;
  }
  const auto* engine = native_dsp::effects::find_engine(engine_id.toStdString());
  if (!engine) {
    set_status(QStringLiteral("Unknown built-in effect"));
    return;
  }
  const auto added = session_->effects().add_effect_engine(*path, static_cast<std::size_t>(index),
                                                           engine->descriptor->id);
  finish_insert_chain_edit(added ? foundation::Result<void>::success()
                                 : foundation::Result<void>::failure(added.error()),
                           QStringLiteral("%1 added").arg(text(engine->descriptor->name)));
}

void ProjectBridge::close_native_effect_editor() {
  if (effect_key_listen_) {
    if (live_run())
      (void)live_run()->set_effect_key_listen(effect_key_listen_->first, effect_key_listen_->second,
                                              false);
    effect_key_listen_.reset();
  }
  if (!native_effect_controller_)
    return;
  // QML unloads the panel before the objects it binds to are destroyed.
  auto controller = std::move(native_effect_controller_);
  auto panel_model = std::move(native_effect_panel_model_);
  auto automation = std::move(native_effect_automation_);
  auto adapter = std::move(native_effect_adapter_);
  native_effect_context_.clear();
  emit nativeEffectControllerChanged();
  emit nativeEffectContextChanged();
}

void ProjectBridge::open_native_effect_editor(const QString& path_id, const QString& slot_id) {
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto& snapshot = session_->snapshot();
  const auto path = effect_path(snapshot, path_id);
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!path || !slot) {
    set_status(QStringLiteral("Select a built-in effect slot"));
    return;
  }
  const auto* effects = &snapshot.master_effects;
  const project::Track* track = nullptr;
  if (const auto* track_id = std::get_if<project::TrackId>(&*path)) {
    track = &*std::ranges::find(snapshot.tracks, *track_id, &project::Track::id);
    effects = &track->effects;
  }
  const auto saved = std::ranges::find(*effects, slot.value(), &project::EffectInsert::id);
  const auto* patch = saved == effects->end()
                          ? nullptr
                          : std::get_if<project::NativeEffectPatch>(&saved->processor);
  if (!patch) {
    set_status(QStringLiteral("This slot holds no built-in effect"));
    return;
  }
  const auto* engine = native_dsp::effects::find_engine(patch->engine);
  if (!engine) {
    set_status(QStringLiteral("Built-in effect %1 is unavailable")
                   .arg(QString::fromStdString(patch->engine)));
    return;
  }
  if (native_effect_adapter_ && native_effect_adapter_->effect_slot() == slot.value()) {
    native_effect_controller_->refresh();
    return;
  }
  close_native_effect_editor();
  const std::optional<project::TrackId> owner =
      track ? std::optional(track->id) : std::optional<project::TrackId>{};
  native_effect_adapter_ = std::make_unique<SessionNativeSynthAdapter>(
      *session_, *path, slot.value(), *engine,
      [this](std::uint64_t) {
        refresh();
        submit_playback_edit();
      },
      [this]() -> std::optional<AudibleSynthState> {
        if (!live_run())
          return std::nullopt;
        return AudibleSynthState{live_run()->status().active_graph_revision, false};
      },
      [this, owner, insert = slot.value()](std::vector<float>& frame) {
        return live_run() && live_run()->effect_telemetry(owner, insert, frame);
      },
      [this]() { schedule_background_save(); },
      [this, owner, insert = slot.value(),
       descriptor = engine->descriptor](std::span<const double> values) {
        return live_run() && live_run()->set_live_effect_values(owner, insert, *descriptor, values);
      });
  native_effect_controller_ = std::make_unique<NativeSynthDeviceController>(
      *engine->descriptor, *native_effect_adapter_, this);
  share_macro_map(*native_effect_controller_);
  if (engine->descriptor->id == "relay") {
    auto relay = std::make_unique<RelayPanelModel>(*native_effect_controller_,
                                                   *native_effect_adapter_, this);
    double bpm = 0.0;
    if (snapshot.tempo.bpm_at_sample_rt({0}, snapshot.sample_rate, bpm))
      relay->set_tempo(bpm);
    native_effect_panel_model_ = std::move(relay);
  }
  if (track) {
    native_effect_automation_ = std::make_unique<NativeSynthAutomationController>(
        *session_, track->id, slot.value(), *engine,
        [this](std::uint64_t) {
          refresh();
          submit_playback_edit();
        },
        [this]() -> std::optional<application::RevisionStatus> {
          if (!live_run())
            return std::nullopt;
          return live_run()->status();
        },
        this, [this]() { schedule_background_save(); });
  }
  native_effect_context_ =
      QVariantMap{{QStringLiteral("pathId"), path_id}, {QStringLiteral("slotId"), slot_id}};
  refresh_native_effect_context();
  emit nativeEffectControllerChanged();
}

// Names a panel uses for its inputs: this path, and the track keying it.
void ProjectBridge::refresh_native_effect_context() {
  if (!session_ || native_effect_context_.isEmpty())
    return;
  const auto& snapshot = session_->snapshot();
  const auto path_id = native_effect_context_.value(QStringLiteral("pathId")).toString();
  const auto slot_id = native_effect_context_.value(QStringLiteral("slotId")).toString();
  const auto state = sidechain_state(path_id, slot_id);
  const bool keyed = state.value(QStringLiteral("keyed")).toBool();
  const auto track = project::TrackId::parse(path_id.toStdString());
  QVariantMap context{
      {QStringLiteral("trackName"),
       track ? track_display_name(track.value()) : QStringLiteral("The master")},
      {QStringLiteral("keyName"),
       keyed ? state.value(QStringLiteral("keyName")).toString() : QStringLiteral("the sidechain")},
      {QStringLiteral("keyed"), keyed},
      {QStringLiteral("sampleRate"), static_cast<double>(snapshot.sample_rate)},
      {QStringLiteral("pathId"), path_id},
      {QStringLiteral("slotId"), slot_id}};
  if (context != native_effect_context_) {
    native_effect_context_ = std::move(context);
    emit nativeEffectContextChanged();
  }
}

QVariantMap ProjectBridge::sidechain_state(const QString& track_id, const QString& slot_id) const {
  QVariantMap state{{QStringLiteral("available"), false}, {QStringLiteral("keyed"), false}};
  if (!session_)
    return state;
  const auto& snapshot = session_->snapshot();
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!track || !slot)
    return state;
  const auto owner = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  if (owner == snapshot.tracks.end())
    return state;
  const auto effect = std::ranges::find(owner->effects, slot.value(), &project::EffectInsert::id);
  if (effect == owner->effects.end())
    return state;
  // Built-in effects that read a detector, and VST3 effects (whose aux input
  // is verified when the graph is prepared).
  bool reads = std::holds_alternative<project::Vst3PluginInstance>(effect->processor);
  if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect->processor)) {
    const auto* engine = native_dsp::effects::find_engine(patch->engine);
    reads = engine != nullptr && engine->sidechain;
  }
  if (!reads)
    return state;
  QVariantList sources;
  for (const auto& other : snapshot.tracks) {
    if (other.id == owner->id)
      continue;
    sources.push_back(
        QVariantMap{{QStringLiteral("id"), QString::fromStdString(other.id.to_string())},
                    {QStringLiteral("name"), track_display_name(other.id)}});
    const auto send = std::ranges::find_if(other.sends, [&](const auto& route) {
      return route.purpose == project::SendPurpose::sidechain && route.destination == owner->id &&
             route.target_insert == slot.value();
    });
    if (send != other.sends.end() && !state.value(QStringLiteral("keyed")).toBool()) {
      state.insert(QStringLiteral("keyed"), true);
      state.insert(QStringLiteral("sourceId"), QString::fromStdString(other.id.to_string()));
      state.insert(QStringLiteral("sendId"), QString::fromStdString(send->id.to_string()));
      state.insert(QStringLiteral("gain"), send->gain);
      state.insert(QStringLiteral("keyName"), track_display_name(other.id));
    }
  }
  state.insert(QStringLiteral("available"), true);
  state.insert(QStringLiteral("sources"), sources);
  return state;
}

void ProjectBridge::set_effect_key_listen(const QString& track_id, const QString& slot_id,
                                          bool listen) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!track || !slot)
    return;
  effect_key_listen_ =
      listen ? std::optional(std::pair(track.value(), slot.value())) : std::nullopt;
  if (live_run())
    (void)live_run()->set_effect_key_listen(track.value(), slot.value(), listen);
}

double ProjectBridge::effect_key_level(const QString& track_id, const QString& slot_id) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!track || !slot || !live_run())
    return 0.0;
  // A rebuilt graph has fresh stages: keep a listen request applied.
  if (effect_key_listen_ && effect_key_listen_->first == track.value() &&
      effect_key_listen_->second == slot.value())
    (void)live_run()->set_effect_key_listen(track.value(), slot.value(), true);
  return live_run()->effect_key_level(track.value(), slot.value()).value_or(0.0F);
}

QVariantMap ProjectBridge::device_strip_info(const QString& engine_id) const {
  const auto id = engine_id.toStdString();
  const auto& style = device_strip_style(id);
  QVariantList parameters;
  const native_dsp::EngineDescriptor* table = nullptr;
  if (const auto* effect = native_dsp::effects::find_engine(id))
    table = effect->descriptor;
  else if (const auto* synth = native_dsp::synth::find_engine(id))
    table = synth->descriptor;
  if (table) {
    for (const auto& parameter : device_strip_parameters(*table))
      parameters.push_back(QString::fromStdString(parameter));
  }
  return QVariantMap{{QStringLiteral("kind"), text(style.kind)},
                     {QStringLiteral("accent"), text(style.accent)},
                     {QStringLiteral("parameters"), parameters}};
}

QObject* ProjectBridge::device_strip_controller(const QString& path_id, const QString& slot_id) {
  if (!session_)
    return nullptr;
  const auto& snapshot = session_->snapshot();
  const auto path = effect_path(snapshot, path_id);
  if (!path)
    return nullptr;
  const project::Track* track = nullptr;
  const auto* effects = &snapshot.master_effects;
  if (const auto* track_id = std::get_if<project::TrackId>(&*path)) {
    track = &*std::ranges::find(snapshot.tracks, *track_id, &project::Track::id);
    effects = &track->effects;
  }
  // Which engine the strip's slot holds now.
  const native_dsp::effects::EffectEngine* effect = nullptr;
  const native_dsp::synth::SynthEngine* synth = nullptr;
  std::optional<project::InsertSlotId> slot;
  if (slot_id == QStringLiteral("__source__")) {
    const auto* patch =
        track ? std::get_if<project::NativeInstrumentPatch>(&track->instrument_source) : nullptr;
    if (!patch || track->kind != project::TrackKind::instrument)
      return nullptr;
    synth = native_dsp::synth::find_engine(patch->engine);
  } else {
    const auto parsed = project::InsertSlotId::parse(slot_id.toStdString());
    if (!parsed)
      return nullptr;
    const auto saved = std::ranges::find(*effects, parsed.value(), &project::EffectInsert::id);
    const auto* patch = saved == effects->end()
                            ? nullptr
                            : std::get_if<project::NativeEffectPatch>(&saved->processor);
    if (!patch)
      return nullptr;
    effect = native_dsp::effects::find_engine(patch->engine);
    slot = parsed.value();
  }
  if (!effect && !synth)
    return nullptr;
  const std::string engine_id(effect ? effect->descriptor->id : synth->descriptor->id);
  const auto key = path_id + QLatin1Char('|') + slot_id;
  auto& entry = device_strips_[key];
  if (entry.controller && entry.engine == engine_id) {
    ++entry.users;
    entry.controller->refresh();
    return entry.controller.get();
  }
  // A different engine now sits in this slot: start afresh. The old
  // controller goes first; a strip still bound to it sees null.
  entry.controller.reset();
  entry.adapter.reset();
  entry.engine = engine_id;
  entry.users = 1;
  const auto after_edit = [this](std::uint64_t) {
    refresh();
    submit_playback_edit();
  };
  const auto save = [this]() { schedule_background_save(); };
  if (synth) {
    const auto owner = track->id;
    entry.adapter = std::make_unique<SessionNativeSynthAdapter>(
        *session_, owner, *synth, after_edit,
        [this, owner]() -> std::optional<AudibleSynthState> {
          if (!live_run())
            return std::nullopt;
          const auto deferred = live_run()->synth_deferred_changes(owner);
          return AudibleSynthState{live_run()->status().active_graph_revision,
                                   deferred.value_or(false)};
        },
        [this, owner](std::vector<float>& frame) {
          return live_run() && live_run()->synth_telemetry(owner, frame);
        },
        save,
        [this, owner, descriptor = synth->descriptor](std::span<const double> values) {
          return live_run() && live_run()->set_live_synth_values(owner, *descriptor, values);
        });
    entry.controller =
        std::make_unique<NativeSynthDeviceController>(*synth->descriptor, *entry.adapter, this);
  } else {
    const std::optional<project::TrackId> owner =
        track ? std::optional(track->id) : std::optional<project::TrackId>{};
    entry.adapter = std::make_unique<SessionNativeSynthAdapter>(
        *session_, *path, *slot, *effect, after_edit,
        [this]() -> std::optional<AudibleSynthState> {
          if (!live_run())
            return std::nullopt;
          return AudibleSynthState{live_run()->status().active_graph_revision, false};
        },
        [this, owner, insert = *slot](std::vector<float>& frame) {
          return live_run() && live_run()->effect_telemetry(owner, insert, frame);
        },
        save,
        [this, owner, insert = *slot,
         descriptor = effect->descriptor](std::span<const double> values) {
          return live_run() &&
                 live_run()->set_live_effect_values(owner, insert, *descriptor, values);
        });
    entry.controller =
        std::make_unique<NativeSynthDeviceController>(*effect->descriptor, *entry.adapter, this);
  }
  share_macro_map(*entry.controller);
  return entry.controller.get();
}

void ProjectBridge::release_device_strip(const QString& path_id, const QString& slot_id) {
  const auto key = path_id + QLatin1Char('|') + slot_id;
  const auto found = device_strips_.find(key);
  if (found == device_strips_.end() || --found->second.users > 0)
    return;
  // After the releasing strip has finished tearing down; a strip that took
  // the same slot meanwhile keeps it.
  QTimer::singleShot(0, this, [this, key] {
    const auto entry = device_strips_.find(key);
    if (entry != device_strips_.end() && entry->second.users <= 0)
      device_strips_.erase(entry);
  });
}

void ProjectBridge::clear_device_strips() { device_strips_.clear(); }

QString ProjectBridge::track_display_name(project::TrackId track) const {
  const auto id = QString::fromStdString(track.to_string());
  for (const auto& row : tracks_) {
    const auto map = row.toMap();
    if (map.value(QStringLiteral("id")).toString() == id)
      return map.value(QStringLiteral("name")).toString();
  }
  return QStringLiteral("The track");
}

} // namespace oscillate::quick
