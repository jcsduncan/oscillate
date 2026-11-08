#include "native_synth_automation_controller.hpp"

#include "oscillate/native_dsp/parameters.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {
QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

NativeSynthAutomationController::NativeSynthAutomationController(
    application::ProjectSession& session, project::TrackId track,
    const native_dsp::synth::SynthEngine& engine, Changed changed, AudibleRevision audible_revision,
    QObject* parent, SaveHook request_save)
    : QObject(parent), session_(session), track_(track), descriptor_(*engine.descriptor),
      after_edit_(std::move(changed)), audible_revision_(std::move(audible_revision)),
      request_save_(std::move(request_save)) {
  select_first();
}

NativeSynthAutomationController::NativeSynthAutomationController(
    application::ProjectSession& session, project::TrackId track, project::InsertSlotId slot,
    const native_dsp::effects::EffectEngine& effect, Changed changed,
    AudibleRevision audible_revision, QObject* parent, SaveHook request_save)
    : QObject(parent), session_(session), track_(track), slot_(slot),
      descriptor_(*effect.descriptor), after_edit_(std::move(changed)),
      audible_revision_(std::move(audible_revision)), request_save_(std::move(request_save)) {
  select_first();
}

void NativeSynthAutomationController::select_first() {
  for (const auto& parameter : descriptor_.parameters) {
    if (parameter.automatable) {
      selected_parameter_ = text(parameter.id);
      break;
    }
  }
}

bool NativeSynthAutomationController::targets(const project::AutomationTarget& target,
                                              std::string_view id) const {
  if (slot_) {
    const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&target);
    return effect && effect->slot == *slot_ && effect->parameter_id == id;
  }
  const auto* native = std::get_if<project::NativeParameterTarget>(&target);
  return native && native->track == track_ && native->parameter_id == id;
}

foundation::Result<std::vector<double>> NativeSynthAutomationController::saved_values() const {
  return slot_ ? session_.effects().native_effect_values(track_, *slot_)
               : session_.instruments().native_instrument_values(track_);
}

QString NativeSynthAutomationController::playback_status() const {
  const auto audible = audible_revision_ ? audible_revision_() : std::nullopt;
  if (!audible)
    return QStringLiteral("Ready for next playback");
  const auto saved = session_.snapshot().revision;
  if (audible->active_graph_revision >= session_.snapshot().audible_revision)
    return QStringLiteral("Active in playback · revision %1").arg(saved);
  if (audible->model_revision == saved &&
      audible->application == application::PlaybackApplication::pending)
    return QStringLiteral("Applying project edit %1 to playback…").arg(saved);
  if (audible->model_revision == saved &&
      audible->application == application::PlaybackApplication::failed)
    return QStringLiteral("Project edit %1 could not apply; playing revision %2.")
        .arg(saved)
        .arg(audible->active_graph_revision);
  return QStringLiteral("Project edit %1 is saved; playing revision %2.")
      .arg(saved)
      .arg(audible->active_graph_revision);
}

void NativeSynthAutomationController::refresh_playback_status() {
  const auto current = playback_status();
  if (current == last_playback_status_)
    return;
  last_playback_status_ = current;
  emit playbackStatusChanged();
}

QVariantList NativeSynthAutomationController::parameters() const {
  QVariantList result;
  for (const auto& parameter : descriptor_.parameters) {
    if (parameter.automatable)
      result.push_back(QVariantMap{{"id", text(parameter.id)},
                                   {"name", text(parameter.name)},
                                   {"group", text(parameter.group)}});
  }
  return result;
}

const native_dsp::ParameterDescriptor*
NativeSynthAutomationController::selected_descriptor() const {
  const auto id = selected_parameter_.toStdString();
  const auto index = native_dsp::find_parameter(descriptor_, id);
  if (!index)
    return nullptr;
  const auto& descriptor = descriptor_.parameters[*index];
  return descriptor.automatable ? &descriptor : nullptr;
}

const project::AutomationLane* NativeSynthAutomationController::selected_lane() const {
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track == tracks.end())
    return nullptr;
  const auto id = selected_parameter_.toStdString();
  const auto lane = std::ranges::find_if(
      track->automation, [&](const auto& candidate) { return targets(candidate.target, id); });
  return lane == track->automation.end() ? nullptr : &*lane;
}

QVariantMap NativeSynthAutomationController::lane() const {
  QVariantMap result{{"present", false}, {"points", QVariantList{}}, {"timebase", "Musical"}};
  const auto* descriptor = selected_descriptor();
  if (!descriptor)
    return result;
  result.insert("name", text(descriptor->name));
  result.insert("minimum", descriptor->minimum);
  result.insert("maximum", descriptor->maximum);
  result.insert("baseline", native_dsp::to_normalised(*descriptor, descriptor->default_value));
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track != tracks.end()) {
    // The slot (or the track's synth) still plays this engine.
    if (const auto values = saved_values();
        values && values.value().size() == descriptor_.parameters.size()) {
      const auto index = native_dsp::find_parameter(descriptor_, selected_parameter_.toStdString());
      if (index)
        result.insert("baseline", native_dsp::to_normalised(*descriptor, values.value()[*index]));
    } else {
      return result;
    }
  }
  const auto* saved = selected_lane();
  if (!saved)
    return result;
  result.insert("present", true);
  result.insert("laneId", text(saved->id.to_string()));
  result.insert("timebase", saved->timebase == project::AutomationTimebase::musical
                                ? QStringLiteral("Musical")
                                : QStringLiteral("Elapsed"));
  QVariantList points;
  for (const auto& point : saved->points) {
    const auto tick = saved->timebase == project::AutomationTimebase::musical
                          ? foundation::Result<timeline::TickPosition>::success({point.coordinate})
                          : session_.snapshot().tempo.sample_to_tick(
                                {point.coordinate}, session_.snapshot().sample_rate);
    QString position = saved->timebase == project::AutomationTimebase::musical
                           ? QStringLiteral("Tick %1").arg(point.coordinate)
                           : QStringLiteral("Sample %1").arg(point.coordinate);
    if (tick) {
      if (const auto beat = session_.snapshot().meter.locate(tick.value()); beat)
        position = QStringLiteral("Bar %1 · beat %2").arg(beat.value().bar).arg(beat.value().beat);
    }
    points.push_back(QVariantMap{{"id", text(point.id.to_string())},
                                 {"position", position},
                                 {"value", native_dsp::to_normalised(*descriptor, point.value)}});
  }
  result.insert("points", points);
  return result;
}

void NativeSynthAutomationController::select_parameter(const QString& id) {
  const auto index = native_dsp::find_parameter(descriptor_, id.toStdString());
  if (!index || !descriptor_.parameters[*index].automatable || selected_parameter_ == id)
    return;
  selected_parameter_ = id;
  emit changed();
}

bool NativeSynthAutomationController::commit(const foundation::Result<void>& result) {
  if (!result)
    return false;
  if (after_edit_)
    after_edit_(session_.snapshot().revision);
  if (request_save_) {
    request_save_();
    message_ = QStringLiteral("Edit queued for save");
  } else if (auto saved = session_.saving().save(); !saved) {
    message_ = QStringLiteral("Edit kept; save failed: %1")
                   .arg(QString::fromStdString(saved.error().message));
  } else {
    message_.clear();
  }
  emit changed();
  refresh_playback_status();
  return true;
}

bool NativeSynthAutomationController::set_point(int zero_based_bar, double normalized_value) {
  const auto* descriptor = selected_descriptor();
  if (!descriptor || zero_based_bar < 0 || zero_based_bar >= 65'536 ||
      !std::isfinite(normalized_value) || normalized_value < 0 || normalized_value > 1)
    return false;
  const auto value = native_dsp::from_normalised(*descriptor, normalized_value);
  const auto result = slot_ ? session_.parameter_automation().set_effect_automation_point(
                                  track_, *slot_, descriptor->id, zero_based_bar, value)
                            : session_.parameter_automation().set_native_automation_point(
                                  track_, descriptor->id, zero_based_bar, value);
  if (!result)
    return false;
  return commit(foundation::Result<void>::success());
}

bool NativeSynthAutomationController::set_value(const QString& point_id, double normalized_value) {
  const auto* descriptor = selected_descriptor();
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!descriptor || !point || !std::isfinite(normalized_value) || normalized_value < 0 ||
      normalized_value > 1)
    return false;
  const auto value = native_dsp::from_normalised(*descriptor, normalized_value);
  return commit(slot_ ? session_.parameter_automation().set_effect_automation_value(
                            track_, *slot_, descriptor->id, point.value(), value)
                      : session_.parameter_automation().set_native_automation_value(
                            track_, descriptor->id, point.value(), value));
}

bool NativeSynthAutomationController::remove_point(const QString& point_id) {
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!selected_descriptor() || !point)
    return false;
  const auto id = selected_parameter_.toStdString();
  return commit(slot_ ? session_.parameter_automation().remove_effect_automation_point(
                            track_, *slot_, id, point.value())
                      : session_.parameter_automation().remove_native_automation_point(
                            track_, id, point.value()));
}

bool NativeSynthAutomationController::remove_lane() {
  if (!selected_descriptor())
    return false;
  const auto id = selected_parameter_.toStdString();
  return commit(
      slot_ ? session_.parameter_automation().remove_effect_automation_lane(track_, *slot_, id)
            : session_.parameter_automation().remove_native_automation_lane(track_, id));
}

bool NativeSynthAutomationController::toggle_timebase() {
  const auto* saved = selected_lane();
  if (!saved)
    return false;
  return commit(session_.automation().toggle_automation_lane_timebase(saved->id));
}

} // namespace oscillate::quick
