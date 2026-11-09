#include "note_effects_controller.hpp"

#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

namespace notefx = native_dsp::note_effects;

[[nodiscard]] const project::Track* find_track(const project::Snapshot& snapshot,
                                               project::TrackId id) {
  const auto found = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  return found == snapshot.tracks.end() ? nullptr : &*found;
}

[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

NoteEffectAdapter::NoteEffectAdapter(application::ProjectSession& session, project::TrackId track,
                                     project::InsertSlotId slot,
                                     const notefx::NoteEffectEngine& engine, Hook after_edit,
                                     Hook live_edit, Hook save)
    : session_(session), track_(track), slot_(slot), engine_(engine),
      after_edit_(std::move(after_edit)), live_edit_(std::move(live_edit)), save_(std::move(save)) {
}

const project::NoteEffectInsert* NoteEffectAdapter::insert() const {
  const auto* track = find_track(session_.snapshot(), track_);
  if (track == nullptr)
    return nullptr;
  const auto found = std::ranges::find(track->note_effects, slot_, &project::NoteEffectInsert::id);
  return found == track->note_effects.end() || found->engine != engine_.descriptor->id ? nullptr
                                                                                       : &*found;
}

std::vector<double> NoteEffectAdapter::values() const {
  if (const auto* saved = insert()) {
    if (auto resolved = notefx::resolve_note_effect(*saved))
      return std::move(resolved.value().values);
  }
  return native_dsp::default_values(*engine_.descriptor);
}

bool NoteEffectAdapter::write(std::span<const double> values,
                              std::optional<std::uint64_t> gesture) {
  auto written = session_.track_devices().set_note_effect_parameters(
      track_, slot_, notefx::sparse_note_effect_parameters(engine_, values), gesture);
  if (!written) {
    message_ = written.error().message;
    return false;
  }
  message_.clear();
  return true;
}

void NoteEffectAdapter::finished() {
  if (after_edit_)
    after_edit_();
  if (save_)
    save_();
}

void NoteEffectAdapter::submit(std::span<const double> values, NativeSynthGesture gesture) {
  switch (gesture) {
  case NativeSynthGesture::begin:
    gesture_ = next_gesture_++;
    before_gesture_ = this->values();
    return;
  case NativeSynthGesture::update:
    // Each move is saved under the drag's gesture: one undo step, heard live.
    if (write(values, gesture_) && live_edit_)
      live_edit_();
    return;
  case NativeSynthGesture::commit:
    (void)write(values, gesture_);
    gesture_.reset();
    finished();
    return;
  case NativeSynthGesture::cancel:
    if (gesture_)
      (void)write(before_gesture_, gesture_);
    gesture_.reset();
    finished();
    return;
  case NativeSynthGesture::single:
    if (write(values, std::nullopt))
      finished();
    return;
  }
}

NativeSynthPanelStatus NoteEffectAdapter::status() const {
  NativeSynthPanelStatus status;
  status.saved_revision = session_.snapshot().revision;
  status.audible_revision = status.saved_revision;
  status.editable = insert() != nullptr;
  status.message = status.editable ? message_ : "This MIDI effect is no longer on the track";
  return status;
}

bool NoteEffectAdapter::submit_preset(std::size_t preset) {
  const auto& presets = engine_.descriptor->presets;
  if (preset >= presets.size())
    return false;
  auto values = native_dsp::preset_values(*engine_.descriptor, presets[preset]);
  if (!values) {
    message_ = values.error().message;
    return true;
  }
  if (write(values.value(), std::nullopt))
    finished();
  return true;
}

std::optional<std::uint32_t> NoteEffectAdapter::macro_of(std::string_view parameter_id) const {
  const auto* track = find_track(session_.snapshot(), track_);
  if (track == nullptr)
    return std::nullopt;
  const project::MacroDestination destination =
      project::NativeEffectParameterTarget{slot_, std::string(parameter_id)};
  for (std::uint32_t index = 0; index < track->macros.size(); ++index) {
    if (std::ranges::any_of(track->macros[index].mappings,
                            [&](const project::MacroMapping& mapping) {
                              return mapping.destination == destination;
                            }))
      return index;
  }
  return std::nullopt;
}

bool NoteEffectAdapter::map_to_macro(std::string_view parameter_id, std::uint32_t index,
                                     double minimum, double maximum) {
  const project::MacroDestination destination =
      project::NativeEffectParameterTarget{slot_, std::string(parameter_id)};
  // A parameter follows one macro: moving it is unmap, then map.
  if (const auto current = macro_of(parameter_id)) {
    if (*current == index)
      return true;
    if (auto removed = session_.track_devices().remove_macro_mapping(track_, *current, destination);
        !removed) {
      message_ = removed.error().message;
      return false;
    }
  }
  auto added =
      session_.track_devices().add_macro_mapping(track_, index, destination, minimum, maximum);
  if (!added)
    message_ = added.error().message;
  finished();
  return static_cast<bool>(added);
}

bool NoteEffectAdapter::unmap_macro(std::string_view parameter_id) {
  const auto current = macro_of(parameter_id);
  if (!current)
    return false;
  auto removed = session_.track_devices().remove_macro_mapping(
      track_, *current, project::NativeEffectParameterTarget{slot_, std::string(parameter_id)});
  if (!removed) {
    message_ = removed.error().message;
    return false;
  }
  finished();
  return true;
}

std::vector<std::pair<std::string, std::uint32_t>> NoteEffectAdapter::macro_bindings() const {
  std::vector<std::pair<std::string, std::uint32_t>> bindings;
  const auto* track = find_track(session_.snapshot(), track_);
  if (track == nullptr)
    return bindings;
  for (std::uint32_t index = 0; index < track->macros.size(); ++index) {
    for (const auto& mapping : track->macros[index].mappings) {
      const auto* target = std::get_if<project::NativeEffectParameterTarget>(&mapping.destination);
      if (target != nullptr && target->slot == slot_)
        bindings.emplace_back(target->parameter_id, index);
    }
  }
  return bindings;
}

NoteEffectsController::NoteEffectsController(Session session, CanEdit can_edit, Report report,
                                             Hook after_edit, Hook live_edit, Hook save,
                                             ControllerCreated created, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), after_edit_(std::move(after_edit)),
      live_edit_(std::move(live_edit)), save_(std::move(save)), created_(std::move(created)) {}

NoteEffectsController::~NoteEffectsController() = default;

QVariantList NoteEffectsController::engines() const {
  QVariantList list;
  for (const auto* engine : notefx::engines()) {
    QVariantList presets;
    for (std::size_t index = 0; index < engine->descriptor->presets.size(); ++index) {
      const auto& preset = engine->descriptor->presets[index];
      presets.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(index)},
                                    {QStringLiteral("name"), text(preset.name)},
                                    {QStringLiteral("category"), text(preset.category)}});
    }
    list.push_back(QVariantMap{{QStringLiteral("id"), text(engine->descriptor->id)},
                               {QStringLiteral("name"), text(engine->descriptor->name)},
                               {QStringLiteral("presets"), presets}});
  }
  return list;
}

bool NoteEffectsController::accepts(const QString& track_id) const {
  auto* session = session_();
  const auto id = project::TrackId::parse(track_id.toStdString());
  if (session == nullptr || !id)
    return false;
  const auto* track = find_track(session->snapshot(), id.value());
  return track != nullptr &&
         std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source);
}

QVariantList NoteEffectsController::chain(const QString& track_id) const {
  QVariantList list;
  auto* session = session_();
  const auto id = project::TrackId::parse(track_id.toStdString());
  if (session == nullptr || !id)
    return list;
  const auto* track = find_track(session->snapshot(), id.value());
  if (track == nullptr)
    return list;
  for (const auto& insert : track->note_effects) {
    const auto* engine = notefx::find_engine(insert.engine);
    list.push_back(
        QVariantMap{{QStringLiteral("slotId"), QString::fromStdString(insert.id.to_string())},
                    {QStringLiteral("engineId"), QString::fromStdString(insert.engine)},
                    {QStringLiteral("name"), engine ? text(engine->descriptor->name)
                                                    : QString::fromStdString(insert.engine)},
                    {QStringLiteral("enabled"), insert.enabled}});
  }
  return list;
}

application::ProjectSession* NoteEffectsController::editable_session(const QString& action) {
  auto* session = session_();
  if (session == nullptr || (can_edit_ && !can_edit_())) {
    report_(QStringLiteral("Finish the current project operation before %1").arg(action));
    return nullptr;
  }
  return session;
}

void NoteEffectsController::accepted(const QString& message) {
  if (after_edit_)
    after_edit_();
  if (save_)
    save_();
  report_(message);
  emit chainChanged();
}

void NoteEffectsController::failed(const QString& action, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(action, QString::fromStdString(error.message)));
}

QString NoteEffectsController::add(const QString& track_id, const QString& engine_id, int index,
                                   int preset) {
  auto* session = editable_session(QStringLiteral("adding a MIDI effect"));
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (session == nullptr || !track)
    return {};
  const auto* engine = notefx::find_engine(engine_id.toStdString());
  if (engine == nullptr) {
    report_(QStringLiteral("Unknown MIDI effect"));
    return {};
  }
  std::vector<project::NativeInstrumentParameter> parameters;
  if (preset >= 0) {
    const auto& presets = engine->descriptor->presets;
    if (static_cast<std::size_t>(preset) >= presets.size())
      return {};
    auto values =
        native_dsp::preset_values(*engine->descriptor, presets[static_cast<std::size_t>(preset)]);
    if (!values) {
      failed(QStringLiteral("MIDI effect preset"), values.error());
      return {};
    }
    parameters = notefx::sparse_note_effect_parameters(*engine, values.value());
  }
  const auto* owner = find_track(session->snapshot(), track.value());
  const auto count = owner ? owner->note_effects.size() : 0;
  const auto at = index < 0 ? count : std::min(static_cast<std::size_t>(index), count);
  auto slot =
      session->track_devices().add_note_effect(track.value(), at, engine->descriptor->id,
                                               std::move(parameters), engine->descriptor->version);
  if (!slot) {
    failed(QStringLiteral("Add MIDI effect"), slot.error());
    return {};
  }
  accepted(QStringLiteral("%1 plays into the synth · one undo step")
               .arg(text(engine->descriptor->name)));
  return QString::fromStdString(slot.value().to_string());
}

void NoteEffectsController::remove(const QString& track_id, const QString& slot_id) {
  auto* session = editable_session(QStringLiteral("removing a MIDI effect"));
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (session == nullptr || !track || !slot)
    return;
  if (auto removed = session->track_devices().remove_note_effect(track.value(), slot.value());
      !removed) {
    failed(QStringLiteral("Remove MIDI effect"), removed.error());
    return;
  }
  accepted(QStringLiteral("MIDI effect removed · one undo step"));
}

void NoteEffectsController::move(const QString& track_id, const QString& slot_id, int index) {
  auto* session = editable_session(QStringLiteral("moving a MIDI effect"));
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (session == nullptr || !track || !slot || index < 0)
    return;
  if (auto moved = session->track_devices().move_note_effect(track.value(), slot.value(),
                                                             static_cast<std::size_t>(index));
      !moved) {
    failed(QStringLiteral("Move MIDI effect"), moved.error());
    return;
  }
  accepted(QStringLiteral("MIDI effect moved · one undo step"));
}

void NoteEffectsController::set_enabled(const QString& track_id, const QString& slot_id,
                                        bool enabled) {
  auto* session = editable_session(QStringLiteral("switching a MIDI effect"));
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (session == nullptr || !track || !slot)
    return;
  if (auto switched =
          session->track_devices().set_note_effect_enabled(track.value(), slot.value(), enabled);
      !switched) {
    failed(QStringLiteral("Switch MIDI effect"), switched.error());
    return;
  }
  accepted(enabled ? QStringLiteral("MIDI effect on") : QStringLiteral("MIDI effect bypassed"));
}

QObject* NoteEffectsController::controller(const QString& track_id, const QString& slot_id) {
  auto* session = session_();
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (session == nullptr || !track || !slot)
    return nullptr;
  const auto key = slot_id.toStdString();
  if (auto found = held_.find(key); found != held_.end()) {
    ++found->second.users;
    return found->second.controller.get();
  }
  const auto* owner = find_track(session->snapshot(), track.value());
  if (owner == nullptr)
    return nullptr;
  const auto insert =
      std::ranges::find(owner->note_effects, slot.value(), &project::NoteEffectInsert::id);
  const auto* engine =
      insert == owner->note_effects.end() ? nullptr : notefx::find_engine(insert->engine);
  if (engine == nullptr)
    return nullptr;
  Held held;
  held.adapter = std::make_unique<NoteEffectAdapter>(*session, track.value(), slot.value(), *engine,
                                                     after_edit_, live_edit_, save_);
  held.controller =
      std::make_unique<NativeSynthDeviceController>(*engine->descriptor, *held.adapter, this);
  held.users = 1;
  if (created_)
    created_(*held.controller);
  auto* controller = held.controller.get();
  held_.emplace(key, std::move(held));
  return controller;
}

void NoteEffectsController::release(const QString& track_id, const QString& slot_id) {
  (void)track_id;
  const auto key = slot_id.toStdString();
  const auto found = held_.find(key);
  if (found == held_.end() || --found->second.users > 0)
    return;
  // After the releasing card has finished tearing down; a card that took
  // the same slot meanwhile keeps it.
  QTimer::singleShot(0, this, [this, key] {
    const auto entry = held_.find(key);
    if (entry != held_.end() && entry->second.users <= 0)
      held_.erase(entry);
  });
}

} // namespace oscillate::quick
