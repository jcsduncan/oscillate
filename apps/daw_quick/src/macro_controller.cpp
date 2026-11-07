#include "macro_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

namespace synth = native_dsp::synth;

constexpr int macro_count = static_cast<int>(project::max_macros_per_track);

// What a mapping drives, for labels and knob positions. The parameter is
// null when its engine is unknown; the domain then stands in.
struct Described {
  QString device;
  QString parameter;
  const synth::ParameterDescriptor* descriptor = nullptr;
};

[[nodiscard]] const synth::ParameterDescriptor*
find_descriptor(const synth::EngineDescriptor* engine, std::string_view id) {
  if (engine == nullptr)
    return nullptr;
  const auto index = synth::find_parameter(*engine, id);
  return index ? &engine->parameters[*index] : nullptr;
}

[[nodiscard]] Described describe(const project::Track& track,
                                 const project::MacroDestination& destination) {
  Described described;
  std::string_view id;
  const synth::EngineDescriptor* engine = nullptr;
  if (const auto* native = std::get_if<project::NativeParameterTarget>(&destination)) {
    id = native->parameter_id;
    if (const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source)) {
      if (const auto* found = synth::find_engine(patch->engine))
        engine = found->descriptor;
    }
  } else {
    const auto& target = std::get<project::NativeEffectParameterTarget>(destination);
    id = target.parameter_id;
    const auto effect = std::ranges::find(track.effects, target.slot, &project::EffectInsert::id);
    if (effect != track.effects.end()) {
      if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect->processor)) {
        if (const auto* found = native_dsp::effects::find_engine(patch->engine))
          engine = found->descriptor;
      }
    } else {
      const auto note =
          std::ranges::find(track.note_effects, target.slot, &project::NoteEffectInsert::id);
      // Note effects are named by their engine id; their domain gives the
      // knob travel.
      if (note != track.note_effects.end()) {
        described.device = QString::fromStdString(note->engine);
        if (!described.device.isEmpty())
          described.device[0] = described.device[0].toUpper();
      }
    }
  }
  if (engine != nullptr)
    described.device =
        QString::fromUtf8(engine->name.data(), static_cast<qsizetype>(engine->name.size()));
  described.descriptor = find_descriptor(engine, id);
  if (described.descriptor != nullptr) {
    // "Sub Level", not "Level": the group names which of several it is.
    const auto name = QString::fromUtf8(described.descriptor->name.data(),
                                        static_cast<qsizetype>(described.descriptor->name.size()));
    const auto group =
        QString::fromUtf8(described.descriptor->group.data(),
                          static_cast<qsizetype>(described.descriptor->group.size()));
    described.parameter = group.isEmpty() || name.startsWith(group, Qt::CaseInsensitive)
                              ? name
                              : group + QLatin1Char(' ') + name;
  } else {
    described.parameter = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
  }
  return described;
}

// Knob position (0..1) of a plain value, and back.
[[nodiscard]] double position_of(const Described& described, const project::MacroMapping& mapping,
                                 double value) {
  if (described.descriptor != nullptr)
    return synth::to_normalised(*described.descriptor, value);
  const auto span = mapping.domain.maximum - mapping.domain.minimum;
  return span > 0.0 ? std::clamp((value - mapping.domain.minimum) / span, 0.0, 1.0) : 0.0;
}

[[nodiscard]] double value_at(const Described& described, const project::MacroMapping& mapping,
                              double position) {
  position = std::clamp(position, 0.0, 1.0);
  if (described.descriptor != nullptr)
    return synth::sanitise(*described.descriptor,
                           synth::from_normalised(*described.descriptor, position));
  return mapping.domain.minimum + position * (mapping.domain.maximum - mapping.domain.minimum);
}

[[nodiscard]] QString text_of(const Described& described, const project::MacroMapping& mapping,
                              double value) {
  if (described.descriptor != nullptr)
    return QString::fromStdString(synth::format_value(*described.descriptor, value));
  auto text = QString::number(value, 'g', 3);
  if (!mapping.domain.unit.empty() && mapping.domain.unit != "normalized")
    text += QLatin1Char(' ') + QString::fromStdString(mapping.domain.unit);
  return text;
}

} // namespace

MacroController::MacroController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), map_state_(this) {
  connect(&map_state_, &MacroMapState::armed_changed, this, &MacroController::armed_changed);
  rebuild();
}

void MacroController::set_track_id(const QString& id) {
  if (id == track_id_)
    return;
  if (turn_)
    cancel_turn(turn_->index);
  track_id_ = id;
  map_state_.set_armed(-1);
  rebuild();
  emit changed();
}

std::optional<project::TrackId> MacroController::track_key() const {
  const auto parsed = project::TrackId::parse(track_id_.toStdString());
  if (!parsed)
    return std::nullopt;
  return parsed.value();
}

const project::Track* MacroController::track() const {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto key = track_key();
  if (session == nullptr || !key)
    return nullptr;
  const auto& tracks = session->snapshot().tracks;
  const auto found = std::ranges::find(tracks, *key, &project::Track::id);
  return found == tracks.end() ? nullptr : &*found;
}

bool MacroController::available() const { return track() != nullptr; }

void MacroController::rebuild() {
  QVariantList list;
  const auto* owner = track();
  for (int index = 0; index < macro_count; ++index) {
    const auto* macro = owner != nullptr && static_cast<std::size_t>(index) < owner->macros.size()
                            ? &owner->macros[static_cast<std::size_t>(index)]
                            : nullptr;
    const double value = turn_ && turn_->index == index ? turn_->value
                         : macro != nullptr             ? macro->value
                                                        : 0.0;
    const auto name = macro != nullptr ? QString::fromStdString(macro->name) : QString();
    QVariantList mappings;
    if (macro != nullptr) {
      for (const auto& mapping : macro->mappings) {
        const auto described = describe(*owner, mapping.destination);
        const auto key =
            std::visit([](const auto& target) { return target.parameter_id; }, mapping.destination);
        mappings.append(QVariantMap{
            {QStringLiteral("label"),
             described.device + QStringLiteral(" · ") + described.parameter},
            {QStringLiteral("device"), described.device},
            {QStringLiteral("parameter"), described.parameter},
            {QStringLiteral("parameter_id"), QString::fromStdString(key)},
            {QStringLiteral("minimum"), position_of(described, mapping, mapping.minimum)},
            {QStringLiteral("maximum"), position_of(described, mapping, mapping.maximum)},
            {QStringLiteral("minimum_text"), text_of(described, mapping, mapping.minimum)},
            {QStringLiteral("maximum_text"), text_of(described, mapping, mapping.maximum)},
            {QStringLiteral("value_text"),
             text_of(described, mapping,
                     automation_project::macro_parameter_value(mapping, value))},
        });
      }
    }
    list.append(QVariantMap{
        {QStringLiteral("index"), index},
        {QStringLiteral("name"), name},
        {QStringLiteral("label"),
         name.isEmpty() ? QStringLiteral("Macro %1").arg(index + 1) : name},
        {QStringLiteral("value"), value},
        {QStringLiteral("value_text"), QStringLiteral("%1%").arg(std::lround(value * 100.0))},
        {QStringLiteral("mappings"), mappings},
    });
  }
  macros_ = std::move(list);
}

void MacroController::refresh() {
  rebuild();
  emit changed();
}

void MacroController::begin_turn(int index) {
  if (turn_)
    cancel_turn(turn_->index);
  const auto* owner = track();
  if (owner == nullptr || index < 0 || index >= macro_count ||
      (hooks_.can_edit && !hooks_.can_edit()))
    return;
  Turn turn;
  turn.index = index;
  if (static_cast<std::size_t>(index) < owner->macros.size()) {
    const auto& macro = owner->macros[static_cast<std::size_t>(index)];
    turn.value = macro.value;
    for (const auto& mapping : macro.mappings) {
      std::optional<project::InsertSlotId> slot;
      const synth::EngineDescriptor* descriptor = nullptr;
      std::vector<double> base;
      std::string_view id;
      if (const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
        const auto* patch = std::get_if<project::NativeInstrumentPatch>(&owner->instrument_source);
        auto resolved = patch ? synth::resolve_native_instrument(*patch)
                              : foundation::Result<synth::ResolvedNativeInstrument>::failure(
                                    {foundation::ErrorCode::not_found, "no synth"});
        if (!resolved)
          continue;
        descriptor = resolved.value().engine->descriptor;
        base = std::move(resolved).value().values;
        id = native->parameter_id;
      } else {
        // Note effects have no live mailbox: they follow on release.
        const auto& target = std::get<project::NativeEffectParameterTarget>(mapping.destination);
        const auto effect =
            std::ranges::find(owner->effects, target.slot, &project::EffectInsert::id);
        const auto* patch = effect == owner->effects.end()
                                ? nullptr
                                : std::get_if<project::NativeEffectPatch>(&effect->processor);
        if (patch == nullptr)
          continue;
        auto resolved = native_inserts::resolve_native_effect(*patch);
        if (!resolved)
          continue;
        descriptor = resolved.value().engine->descriptor;
        base = std::move(resolved).value().values;
        slot = target.slot;
        id = target.parameter_id;
      }
      const auto parameter = synth::find_parameter(*descriptor, id);
      if (!parameter)
        continue;
      auto device = std::ranges::find(turn.devices, slot, &TurnDevice::slot);
      if (device == turn.devices.end()) {
        turn.devices.push_back({slot, descriptor, std::move(base), {}});
        device = std::prev(turn.devices.end());
      }
      device->mapped.emplace_back(*parameter, mapping);
    }
  }
  turn_ = std::move(turn);
  rebuild();
  emit changed();
}

void MacroController::offer(const Turn& turn, double value) {
  const auto key = track_key();
  if (!key)
    return;
  for (const auto& device : turn.devices) {
    auto values = device.base;
    for (const auto& [parameter, mapping] : device.mapped)
      values[parameter] = automation_project::macro_parameter_value(mapping, value);
    if (device.slot) {
      if (hooks_.live_effect)
        (void)hooks_.live_effect(*key, *device.slot, *device.descriptor, values);
    } else if (hooks_.live_synth) {
      (void)hooks_.live_synth(*key, *device.descriptor, values);
    }
    if (hooks_.show)
      hooks_.show(*key, device.slot, &values);
  }
}

void MacroController::hand_back(const Turn& turn) {
  const auto key = track_key();
  if (!key || !hooks_.show)
    return;
  for (const auto& device : turn.devices)
    hooks_.show(*key, device.slot, nullptr);
}

void MacroController::turn(int index, double value) {
  if (!turn_ || turn_->index != index || !std::isfinite(value))
    return;
  value = std::clamp(value, 0.0, 1.0);
  if (value == turn_->value)
    return;
  turn_->value = value;
  offer(*turn_, value);
  rebuild();
  emit changed();
}

void MacroController::end_turn(int index) {
  if (!turn_ || turn_->index != index)
    return;
  auto turn = std::move(*turn_);
  turn_.reset();
  hand_back(turn);
  // A refused edit leaves the saved sound, which plays again.
  if (!commit_value(index, turn.value))
    restore(turn);
  rebuild();
  emit changed();
}

void MacroController::restore(const Turn& turn) {
  const auto key = track_key();
  if (!key)
    return;
  for (const auto& device : turn.devices) {
    if (device.slot) {
      if (hooks_.live_effect)
        (void)hooks_.live_effect(*key, *device.slot, *device.descriptor, device.base);
    } else if (hooks_.live_synth) {
      (void)hooks_.live_synth(*key, *device.descriptor, device.base);
    }
  }
}

void MacroController::cancel_turn(int index) {
  if (!turn_ || turn_->index != index)
    return;
  auto turn = std::move(*turn_);
  turn_.reset();
  restore(turn);
  hand_back(turn);
  rebuild();
  emit changed();
}

bool MacroController::commit_value(int index, double value) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto key = track_key();
  if (session == nullptr || !key || index < 0 || index >= macro_count)
    return false;
  if (hooks_.can_edit && !hooks_.can_edit()) {
    if (hooks_.report)
      hooks_.report(QStringLiteral("Finish the current project operation before turning a macro"));
    return false;
  }
  const auto before = session->snapshot().revision;
  auto applied = session->track_devices().set_macro_value(*key, static_cast<std::uint32_t>(index),
                                                          std::clamp(value, 0.0, 1.0));
  if (!applied) {
    if (hooks_.report)
      hooks_.report(
          QStringLiteral("Macro: %1").arg(QString::fromStdString(applied.error().message)));
    return false;
  }
  if (session->snapshot().revision != before)
    accepted();
  return true;
}

void MacroController::accepted() {
  if (hooks_.after_edit)
    hooks_.after_edit();
  if (hooks_.save)
    hooks_.save();
}

void MacroController::set_value(int index, double value) {
  if (turn_)
    cancel_turn(turn_->index);
  if (!std::isfinite(value))
    return;
  (void)commit_value(index, value);
  rebuild();
  emit changed();
}

void MacroController::step(int index, int direction, bool fine) {
  const auto* owner = track();
  if (owner == nullptr || index < 0 || index >= macro_count || direction == 0)
    return;
  const double current = static_cast<std::size_t>(index) < owner->macros.size()
                             ? owner->macros[static_cast<std::size_t>(index)].value
                             : 0.0;
  set_value(index,
            std::clamp(current + (direction > 0 ? 1.0 : -1.0) * (fine ? 0.001 : 0.01), 0.0, 1.0));
}

void MacroController::rename(int index, const QString& name) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto key = track_key();
  if (session == nullptr || !key || index < 0 || index >= macro_count)
    return;
  const auto before = session->snapshot().revision;
  auto applied = session->track_devices().set_macro_name(*key, static_cast<std::uint32_t>(index),
                                                         name.trimmed().left(48).toStdString());
  if (!applied) {
    if (hooks_.report)
      hooks_.report(
          QStringLiteral("Macro: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  if (session->snapshot().revision != before)
    accepted();
  refresh();
}

void MacroController::set_mapping_range(int index, int mapping, double minimum, double maximum) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto* owner = track();
  if (session == nullptr || owner == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= owner->macros.size() || mapping < 0 ||
      !std::isfinite(minimum) || !std::isfinite(maximum))
    return;
  const auto& macro = owner->macros[static_cast<std::size_t>(index)];
  if (static_cast<std::size_t>(mapping) >= macro.mappings.size())
    return;
  const auto chosen = macro.mappings[static_cast<std::size_t>(mapping)];
  const auto described = describe(*owner, chosen.destination);
  const auto before = session->snapshot().revision;
  auto applied = session->track_devices().set_macro_mapping_range(
      owner->id, static_cast<std::uint32_t>(index), chosen.destination,
      value_at(described, chosen, minimum), value_at(described, chosen, maximum));
  if (!applied) {
    if (hooks_.report)
      hooks_.report(
          QStringLiteral("Macro: %1").arg(QString::fromStdString(applied.error().message)));
  } else if (session->snapshot().revision != before) {
    accepted();
  }
  refresh();
}

void MacroController::remove_mapping(int index, int mapping) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto* owner = track();
  if (session == nullptr || owner == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= owner->macros.size() || mapping < 0)
    return;
  const auto& macro = owner->macros[static_cast<std::size_t>(index)];
  if (static_cast<std::size_t>(mapping) >= macro.mappings.size())
    return;
  const auto destination = macro.mappings[static_cast<std::size_t>(mapping)].destination;
  auto applied = session->track_devices().remove_macro_mapping(
      owner->id, static_cast<std::uint32_t>(index), destination);
  if (!applied) {
    if (hooks_.report)
      hooks_.report(
          QStringLiteral("Macro: %1").arg(QString::fromStdString(applied.error().message)));
  } else {
    accepted();
  }
  refresh();
}

QString MacroController::position_text(int index, int mapping, double position) const {
  const auto* owner = track();
  if (owner == nullptr || index < 0 || static_cast<std::size_t>(index) >= owner->macros.size() ||
      mapping < 0)
    return {};
  const auto& macro = owner->macros[static_cast<std::size_t>(index)];
  if (static_cast<std::size_t>(mapping) >= macro.mappings.size())
    return {};
  const auto& chosen = macro.mappings[static_cast<std::size_t>(mapping)];
  const auto described = describe(*owner, chosen.destination);
  return text_of(described, chosen, value_at(described, chosen, position));
}

} // namespace oscillate::quick
