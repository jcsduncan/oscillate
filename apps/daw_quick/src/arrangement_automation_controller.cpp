#include "arrangement_automation_controller.hpp"

#include "bar_ticks.hpp"

#include "oscillate/automation/curve.hpp"
#include "oscillate/foundation/value_text.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::quick {
namespace {
using Key = project::TrackControlTarget::Key;

// How a target's values sit on a lane's 0..1 height and read as text.
struct Feel {
  enum class Kind : std::uint8_t {
    gain,
    balance,
    mute,
    parameter,
    normalised
  } kind = Kind::normalised;
  const native_dsp::ParameterDescriptor* parameter = nullptr;
  QString name;
  QString group;
};

// Gains (level, sends: linear 0..4) follow a fader: -70 dB at the foot,
// +12 dB at the top, silence below.
constexpr double fader_floor_db = automation::decibel_floor;
const double fader_top_db = 20.0 * std::log10(4.0);

[[nodiscard]] double to_lane(const Feel& feel, double value) {
  switch (feel.kind) {
  case Feel::Kind::gain:
    if (value <= 0.0)
      return 0.0;
    return std::clamp((20.0 * std::log10(value) - fader_floor_db) / (fader_top_db - fader_floor_db),
                      0.0, 1.0);
  case Feel::Kind::balance:
    return (value + 1.0) / 2.0;
  case Feel::Kind::parameter:
    return native_dsp::to_normalised(*feel.parameter, value);
  case Feel::Kind::mute:
  case Feel::Kind::normalised:
    break;
  }
  return value;
}

[[nodiscard]] double from_lane(const Feel& feel, double lane) {
  lane = std::clamp(lane, 0.0, 1.0);
  switch (feel.kind) {
  case Feel::Kind::gain:
    if (lane <= 0.0)
      return 0.0;
    return std::min(
        4.0, std::pow(10.0, (fader_floor_db + lane * (fader_top_db - fader_floor_db)) / 20.0));
  case Feel::Kind::balance:
    return lane * 2.0 - 1.0;
  case Feel::Kind::mute:
    return lane >= 0.5 ? 1.0 : 0.0;
  case Feel::Kind::parameter:
    return native_dsp::from_normalised(*feel.parameter, lane);
  case Feel::Kind::normalised:
    break;
  }
  return lane;
}

[[nodiscard]] QString text_of(const Feel& feel, double value) {
  switch (feel.kind) {
  case Feel::Kind::gain: {
    if (value <= 0.0)
      return QStringLiteral("−inf dB");
    const auto db = 20.0 * std::log10(value);
    const auto rounded = std::round(db * 10.0) / 10.0;
    return (rounded > 0.0   ? QStringLiteral("+")
            : rounded < 0.0 ? QStringLiteral("−")
                            : QString()) +
           QString::number(std::abs(rounded), 'f', 1) + QStringLiteral(" dB");
  }
  case Feel::Kind::balance: {
    const auto percent = static_cast<int>(std::lround(std::abs(value) * 100.0));
    if (percent == 0)
      return QStringLiteral("C");
    return (value < 0.0 ? QStringLiteral("L ") : QStringLiteral("R ")) + QString::number(percent);
  }
  case Feel::Kind::mute:
    return value >= 0.5 ? QStringLiteral("Muted") : QStringLiteral("Playing");
  case Feel::Kind::parameter:
    return QString::fromStdString(native_dsp::format_value(*feel.parameter, value));
  case Feel::Kind::normalised:
    break;
  }
  return QString::number(std::lround(value * 100.0)) + QStringLiteral("%");
}

[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// Unit prefixes a typed value may carry beyond the parameter's own unit.
constexpr double unit_thousand = 1'000.0;
constexpr double percent_scale = 100.0;
constexpr double balance_percent = 100.0;
// Amplitude decibels: 20 per decade of gain.
constexpr double gain_decibels_per_decade = 20.0;

// The leading number of `typed` (a true minus sign or "-", "inf" or "∞"
// for an infinite magnitude) and the text after it, trimmed.
struct TypedNumber {
  double value = 0.0;
  QString rest;
};
[[nodiscard]] std::optional<TypedNumber> leading_number(QString typed) {
  typed = typed.trimmed();
  typed.replace(QChar(0x2212), QLatin1Char('-'));
  typed.replace(QStringLiteral("∞"), QStringLiteral("inf"));
  bool negative = false;
  if (typed.startsWith(QLatin1Char('-')) || typed.startsWith(QLatin1Char('+'))) {
    negative = typed.startsWith(QLatin1Char('-'));
    typed = typed.mid(1).trimmed();
  }
  const auto infinity_word = QStringLiteral("inf");
  if (typed.startsWith(infinity_word, Qt::CaseInsensitive)) {
    const auto infinite = std::numeric_limits<double>::infinity();
    return TypedNumber{negative ? -infinite : infinite, typed.mid(infinity_word.size()).trimmed()};
  }
  qsizetype end = 0;
  while (end < typed.size() &&
         (typed[end].isDigit() || typed[end] == QLatin1Char('.') || typed[end] == QLatin1Char(',')))
    ++end;
  bool parsed = false;
  auto number = typed.left(end).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&parsed);
  if (!parsed)
    return std::nullopt;
  return TypedNumber{negative ? -number : number, typed.mid(end).trimmed()};
}

// A typed value in a feel's own units, as the lane's 0..1.
[[nodiscard]] std::optional<double> lane_from_text(const Feel& feel, const QString& typed) {
  switch (feel.kind) {
  case Feel::Kind::mute: {
    const auto word = typed.trimmed().toLower();
    if (word == QStringLiteral("muted") || word == QStringLiteral("on") ||
        word == QStringLiteral("1"))
      return 1.0;
    if (word == QStringLiteral("playing") || word == QStringLiteral("off") ||
        word == QStringLiteral("0"))
      return 0.0;
    return std::nullopt;
  }
  case Feel::Kind::balance: {
    auto word = typed.trimmed().toUpper();
    if (word == QStringLiteral("C"))
      return to_lane(feel, 0.0);
    double side = 1.0;
    if (word.startsWith(QLatin1Char('L')) || word.startsWith(QLatin1Char('R'))) {
      side = word.startsWith(QLatin1Char('L')) ? -1.0 : 1.0;
      word = word.mid(1);
    }
    const auto number = leading_number(word);
    if (!number || !std::isfinite(number->value) || !number->rest.isEmpty() ||
        std::abs(number->value) > balance_percent)
      return std::nullopt;
    return to_lane(feel, side * number->value / balance_percent);
  }
  case Feel::Kind::gain: {
    const auto number = leading_number(typed);
    if (!number || (!number->rest.isEmpty() &&
                    number->rest.compare(QStringLiteral("dB"), Qt::CaseInsensitive) != 0))
      return std::nullopt;
    if (number->value == -std::numeric_limits<double>::infinity())
      return 0.0;
    if (!std::isfinite(number->value) || number->value > fader_top_db)
      return std::nullopt;
    return to_lane(feel, std::pow(10.0, number->value / gain_decibels_per_decade));
  }
  case Feel::Kind::parameter: {
    const auto& parameter = *feel.parameter;
    if (parameter.scale == native_dsp::ParameterScale::choice) {
      for (std::size_t index = 0; index < parameter.choices.size(); ++index) {
        if (text(parameter.choices[index]).compare(typed.trimmed(), Qt::CaseInsensitive) == 0)
          return to_lane(feel, static_cast<double>(index));
      }
      return std::nullopt;
    }
    const auto number = leading_number(typed);
    if (!number)
      return std::nullopt;
    const auto unit = text(parameter.unit);
    auto value = number->value;
    const auto& rest = number->rest;
    const bool larger_unit =
        (unit == text(foundation::hertz_unit) &&
         rest.compare(text(foundation::kilohertz_unit), Qt::CaseInsensitive) == 0) ||
        (unit == text(foundation::milliseconds_unit) && rest == text(foundation::seconds_unit));
    if (larger_unit)
      value *= unit_thousand;
    else if (unit == text(foundation::seconds_unit) && rest == text(foundation::milliseconds_unit))
      value /= unit_thousand;
    else if (!rest.isEmpty() && rest.compare(unit, Qt::CaseInsensitive) != 0)
      return std::nullopt;
    if (unit == text(foundation::percent_unit))
      value /= percent_scale;
    if (value == -std::numeric_limits<double>::infinity())
      value = parameter.minimum;
    if (!std::isfinite(value) || value < parameter.minimum || value > parameter.maximum)
      return std::nullopt;
    return to_lane(feel, value);
  }
  case Feel::Kind::normalised: {
    const auto number = leading_number(typed);
    if (!number || !std::isfinite(number->value) ||
        (!number->rest.isEmpty() && number->rest != text(foundation::percent_unit)))
      return std::nullopt;
    const auto value = number->value / percent_scale;
    if (value < 0.0 || value > 1.0)
      return std::nullopt;
    return value;
  }
  }
  return std::nullopt;
}

[[nodiscard]] QString track_name(const project::Snapshot& snapshot, project::TrackId id) {
  const auto track = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  if (track == snapshot.tracks.end())
    return QStringLiteral("a missing track");
  if (!track->name.empty())
    return QString::fromStdString(track->name);
  return QStringLiteral("Track %1").arg(track - snapshot.tracks.begin() + 1);
}

// A target as the short key the arrangement passes back.
[[nodiscard]] QString key_of(const project::AutomationTarget& target) {
  return std::visit(
      [](const auto& item) -> QString {
        using Target = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Target, project::TrackControlTarget>) {
          return item.key == Key::channel_pan    ? QStringLiteral("pan")
                 : item.key == Key::channel_mute ? QStringLiteral("mute")
                                                 : QStringLiteral("level");
        } else if constexpr (std::is_same_v<Target, project::SendGainTarget>) {
          return QStringLiteral("send:") + QString::fromStdString(item.send.to_string());
        } else if constexpr (std::is_same_v<Target, project::NativeEffectParameterTarget>) {
          return QStringLiteral("fx:") + QString::fromStdString(item.slot.to_string()) +
                 QStringLiteral(":") + QString::fromStdString(item.parameter_id);
        } else if constexpr (std::is_same_v<Target, project::NativeParameterTarget>) {
          return QStringLiteral("synth:") + QString::fromStdString(item.parameter_id);
        } else if constexpr (std::is_same_v<Target, project::MacroTarget>) {
          return QStringLiteral("macro:") + QString::number(item.index);
        } else {
          return QStringLiteral("vst3:") + QString::fromStdString(item.plugin.to_string()) +
                 QStringLiteral(":") + QString::number(item.parameter_id);
        }
      },
      target);
}

[[nodiscard]] std::optional<project::AutomationTarget> target_of(project::TrackId track,
                                                                 const QString& key) {
  if (key == QStringLiteral("level"))
    return project::TrackControlTarget{track, Key::channel_level};
  if (key == QStringLiteral("pan"))
    return project::TrackControlTarget{track, Key::channel_pan};
  if (key == QStringLiteral("mute"))
    return project::TrackControlTarget{track, Key::channel_mute};
  const auto parts = key.split(QLatin1Char(':'));
  if (parts.size() == 2 && parts[0] == QStringLiteral("send")) {
    if (auto send = project::SendId::parse(parts[1].toStdString()))
      return project::SendGainTarget{send.value()};
  } else if (parts.size() == 2 && parts[0] == QStringLiteral("synth")) {
    return project::NativeParameterTarget{track, parts[1].toStdString()};
  } else if (parts.size() == 2 && parts[0] == QStringLiteral("macro")) {
    bool parsed = false;
    const auto index = parts[1].toUInt(&parsed);
    if (parsed)
      return project::MacroTarget{track, index};
  } else if (parts.size() == 3 && parts[0] == QStringLiteral("fx")) {
    if (auto slot = project::InsertSlotId::parse(parts[1].toStdString()))
      return project::NativeEffectParameterTarget{slot.value(), parts[2].toStdString()};
  } else if (parts.size() == 3 && parts[0] == QStringLiteral("vst3")) {
    bool parsed = false;
    const auto parameter = parts[2].toUInt(&parsed);
    if (auto plugin = project::PluginInstanceId::parse(parts[1].toStdString()); plugin && parsed)
      return project::Vst3ParameterTarget{plugin.value(), parameter};
  }
  return std::nullopt;
}

[[nodiscard]] const native_dsp::ParameterDescriptor*
descriptor_of(const native_dsp::EngineDescriptor& engine, std::string_view id) {
  const auto index = native_dsp::find_parameter(engine, id);
  if (!index)
    return nullptr;
  const auto& parameter = engine.parameters[*index];
  return parameter.automatable ? &parameter : nullptr;
}

[[nodiscard]] std::optional<Feel> feel_of(const project::Snapshot& snapshot,
                                          const project::Track& track,
                                          const project::AutomationTarget& target) {
  return std::visit(
      [&](const auto& item) -> std::optional<Feel> {
        using Target = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Target, project::TrackControlTarget>) {
          if (item.key == Key::channel_pan)
            return Feel{Feel::Kind::balance, nullptr, QStringLiteral("Pan"),
                        QStringLiteral("Mixer")};
          if (item.key == Key::channel_mute)
            return Feel{Feel::Kind::mute, nullptr, QStringLiteral("Mute"), QStringLiteral("Mixer")};
          if (item.key == Key::channel_level)
            return Feel{Feel::Kind::gain, nullptr, QStringLiteral("Volume"),
                        QStringLiteral("Mixer")};
          return std::nullopt;
        } else if constexpr (std::is_same_v<Target, project::SendGainTarget>) {
          const auto route = std::ranges::find(track.sends, item.send, &project::SendRoute::id);
          if (route == track.sends.end())
            return std::nullopt;
          return Feel{Feel::Kind::gain, nullptr,
                      QStringLiteral("Send → ") + track_name(snapshot, route->destination),
                      QStringLiteral("Sends")};
        } else if constexpr (std::is_same_v<Target, project::NativeEffectParameterTarget>) {
          const auto slot = std::ranges::find(track.effects, item.slot, &project::EffectInsert::id);
          const auto* patch = slot == track.effects.end()
                                  ? nullptr
                                  : std::get_if<project::NativeEffectPatch>(&slot->processor);
          if (patch == nullptr)
            return std::nullopt;
          auto resolved = native_inserts::resolve_native_effect(*patch);
          if (!resolved)
            return std::nullopt;
          const auto& engine = *resolved.value().engine->descriptor;
          const auto* parameter = descriptor_of(engine, item.parameter_id);
          if (parameter == nullptr)
            return std::nullopt;
          return Feel{Feel::Kind::parameter, parameter, text(parameter->name), text(engine.name)};
        } else if constexpr (std::is_same_v<Target, project::NativeParameterTarget>) {
          const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
          if (patch == nullptr)
            return std::nullopt;
          auto resolved = native_inserts::resolve_native_instrument(*patch);
          if (!resolved)
            return std::nullopt;
          const auto& engine = *resolved.value().engine->descriptor;
          const auto* parameter = descriptor_of(engine, item.parameter_id);
          if (parameter == nullptr)
            return std::nullopt;
          return Feel{Feel::Kind::parameter, parameter, text(parameter->name), text(engine.name)};
        } else if constexpr (std::is_same_v<Target, project::MacroTarget>) {
          if (item.index >= track.macros.size())
            return std::nullopt;
          const auto& name = track.macros[item.index].name;
          return Feel{Feel::Kind::normalised, nullptr,
                      name.empty() ? QStringLiteral("Macro %1").arg(item.index + 1) : text(name),
                      QStringLiteral("Macros")};
        } else {
          return Feel{Feel::Kind::normalised, nullptr,
                      QStringLiteral("Parameter %1").arg(item.parameter_id),
                      QStringLiteral("Plug-in")};
        }
      },
      target);
}

[[nodiscard]] QString shape_name(project::AutomationShape shape) {
  switch (shape) {
  case project::AutomationShape::hold:
    return QStringLiteral("hold");
  case project::AutomationShape::quadratic:
    return QStringLiteral("quadratic");
  case project::AutomationShape::exponential:
    return QStringLiteral("exponential");
  case project::AutomationShape::ease_in_out:
    return QStringLiteral("ease_in_out");
  case project::AutomationShape::bezier:
    return QStringLiteral("bezier");
  case project::AutomationShape::linear:
    break;
  }
  return QStringLiteral("linear");
}

// A bowed segment (bend()) is a Bezier whose handles sit on the thirds,
// lifted together by the bend; any other shape reads as no bend.
constexpr double max_bend = 2.0 / 3.0;
[[nodiscard]] project::AutomationHandles bow(double bend) {
  return {1.0 / 3.0, std::clamp(1.0 / 3.0 + bend, 0.0, 1.0), 2.0 / 3.0,
          std::clamp(2.0 / 3.0 + bend, 0.0, 1.0)};
}
[[nodiscard]] double bend_of(const project::AutomationPoint& point) {
  if (point.to_next != project::AutomationShape::bezier)
    return 0.0;
  const auto& h = point.handles;
  // Held back past a third, the first handle sits at 0 and the second moves.
  return h.y1 <= 0.0 ? h.y2 - 2.0 / 3.0 : h.y1 - 1.0 / 3.0;
}

// The engine's segment shape (automation/curve.cpp), on progress 0..1.
[[nodiscard]] double progress(const project::AutomationPoint& point, double u) {
  return automation::shape_progress(
      static_cast<automation::Shape>(point.to_next), u,
      {point.handles.x1, point.handles.y1, point.handles.x2, point.handles.y2});
}

[[nodiscard]] QVariantList targets_of(const project::Snapshot& snapshot,
                                      const project::Track& track) {
  QVariantList groups;
  const auto add_group = [&](QString name, QVariantList items) {
    if (!items.isEmpty())
      groups.push_back(QVariantMap{{"group", std::move(name)}, {"items", std::move(items)}});
  };
  add_group(QStringLiteral("Mixer"), {QVariantMap{{"key", "level"}, {"name", "Volume"}},
                                      QVariantMap{{"key", "pan"}, {"name", "Pan"}},
                                      QVariantMap{{"key", "mute"}, {"name", "Mute"}}});
  QVariantList sends;
  for (const auto& send : track.sends) {
    if (send.purpose == project::SendPurpose::audible)
      sends.push_back(QVariantMap{
          {"key", key_of(project::SendGainTarget{send.id})},
          {"name", QStringLiteral("Send → ") + track_name(snapshot, send.destination)}});
  }
  add_group(QStringLiteral("Sends"), std::move(sends));
  // A macro's lane drives the parameters it maps (ADR-0059): they offer no
  // lane of their own, and the track's macros are offered instead.
  QVariantList macros;
  for (std::uint32_t index = 0; index < track.macros.size(); ++index) {
    const auto& name = track.macros[index].name;
    macros.push_back(QVariantMap{
        {"key", key_of(project::MacroTarget{track.id, index})},
        {"name", name.empty() ? QStringLiteral("Macro %1").arg(index + 1) : text(name)}});
  }
  add_group(QStringLiteral("Macros"), std::move(macros));
  const auto mapped = [&](const project::MacroDestination& destination) {
    return std::ranges::any_of(track.macros, [&](const project::Macro& macro) {
      return std::ranges::any_of(macro.mappings, [&](const project::MacroMapping& mapping) {
        return mapping.destination == destination;
      });
    });
  };
  const auto add_engine = [&](const native_dsp::EngineDescriptor& engine, auto make_key,
                              auto make_destination) {
    QVariantList items;
    for (const auto& parameter : engine.parameters) {
      if (parameter.automatable && !mapped(make_destination(parameter.id)))
        items.push_back(
            QVariantMap{{"key", make_key(parameter.id)}, {"name", text(parameter.name)}});
    }
    add_group(text(engine.name), std::move(items));
  };
  if (const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source)) {
    if (auto resolved = native_inserts::resolve_native_instrument(*patch))
      add_engine(
          *resolved.value().engine->descriptor,
          [&](std::string_view id) {
            return key_of(project::NativeParameterTarget{track.id, std::string(id)});
          },
          [&](std::string_view id) -> project::MacroDestination {
            return project::NativeParameterTarget{track.id, std::string(id)};
          });
  }
  for (const auto& effect : track.effects) {
    if (!effect.enabled)
      continue;
    if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor)) {
      if (auto resolved = native_inserts::resolve_native_effect(*patch))
        add_engine(
            *resolved.value().engine->descriptor,
            [&](std::string_view id) {
              return key_of(project::NativeEffectParameterTarget{effect.id, std::string(id)});
            },
            [&](std::string_view id) -> project::MacroDestination {
              return project::NativeEffectParameterTarget{effect.id, std::string(id)};
            });
    } else if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&effect.processor)) {
      QVariantList items;
      for (const auto& reference : plugin->parameters)
        items.push_back(
            QVariantMap{{"key", key_of(project::Vst3ParameterTarget{plugin->id, reference.id})},
                        {"name", QStringLiteral("Parameter %1").arg(reference.id)}});
      add_group(QString::fromStdString(plugin->name), std::move(items));
    }
  }
  return groups;
}

} // namespace

ArrangementAutomationController::ArrangementAutomationController(Session session, CanEdit can_edit,
                                                                 Report report, Commit commit,
                                                                 QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

void ArrangementAutomationController::refresh() {
  QVariantList result;
  if (const auto* session = session_()) {
    const auto& snapshot = session->snapshot();
    for (const auto& track : snapshot.tracks) {
      QVariantList lanes;
      for (const auto& lane : track.automation) {
        const auto feel = feel_of(snapshot, track, lane.target);
        if (!feel)
          continue;
        QVariantList points;
        for (const auto& point : lane.points) {
          std::int64_t tick = point.coordinate;
          if (lane.timebase == project::AutomationTimebase::elapsed) {
            auto musical = snapshot.tempo.sample_to_tick({point.coordinate}, snapshot.sample_rate);
            if (!musical)
              continue;
            tick = musical.value().value;
          }
          const auto bar = bar_of_tick(snapshot, tick);
          if (!bar)
            continue;
          points.push_back(QVariantMap{{"id", QString::fromStdString(point.id.to_string())},
                                       {"bar", *bar},
                                       {"value", to_lane(*feel, point.value)},
                                       {"text", text_of(*feel, point.value)},
                                       {"shape", shape_name(point.to_next)},
                                       {"bend", bend_of(point)}});
        }
        lanes.push_back(
            QVariantMap{{"laneId", QString::fromStdString(lane.id.to_string())},
                        {"key", key_of(lane.target)},
                        {"name", feel->name},
                        {"group", feel->group},
                        {"discrete", lane.domain.discrete},
                        {"timebase", lane.timebase == project::AutomationTimebase::musical
                                         ? QStringLiteral("Musical")
                                         : QStringLiteral("Elapsed")},
                        {"points", points}});
      }
      result.push_back(QVariantMap{{"trackId", QString::fromStdString(track.id.to_string())},
                                   {"lanes", lanes},
                                   {"targets", targets_of(snapshot, track)}});
    }
  }
  if (result == tracks_)
    return;
  tracks_ = std::move(result);
  emit changed();
}

application::ProjectSession* ArrangementAutomationController::editable_session() {
  if (auto* project = session_(); project != nullptr && can_edit_())
    return project;
  report_(QStringLiteral("Finish the current project operation before editing automation"));
  return nullptr;
}

void ArrangementAutomationController::failure(const foundation::Error& error) {
  report_(QStringLiteral("Automation: %1").arg(QString::fromStdString(error.message)));
}

void ArrangementAutomationController::accepted(const QString& action) {
  commit_();
  report_(action);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): QML calls this positionally.
QString ArrangementAutomationController::add_point(const QString& track_id, const QString& key,
                                                   double bar, double value) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  auto* session = editable_session();
  if (!session)
    return {};
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto target = track ? target_of(track.value(), key) : std::nullopt;
  const auto& snapshot = session->snapshot();
  const auto owner = track ? std::ranges::find(snapshot.tracks, track.value(), &project::Track::id)
                           : snapshot.tracks.end();
  const auto feel =
      target && owner != snapshot.tracks.end() ? feel_of(snapshot, *owner, *target) : std::nullopt;
  const auto tick = tick_of_bar(snapshot, bar);
  if (!feel || !tick || !std::isfinite(value)) {
    report_(QStringLiteral("Choose a parameter and a place on the timeline"));
    return {};
  }
  auto added = session->automation().add_automation_point(track.value(), *target, *tick,
                                                          from_lane(*feel, value));
  if (!added) {
    failure(added.error());
    return {};
  }
  accepted(QStringLiteral("%1 point added").arg(feel->name));
  return QString::fromStdString(added.value().to_string());
}

void ArrangementAutomationController::move_points(const QString& lane_id,
                                                  const QVariantList& moves) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto& snapshot = session->snapshot();
  const project::Track* owner = nullptr;
  const project::AutomationLane* found = nullptr;
  for (const auto& track : snapshot.tracks) {
    for (const auto& candidate : track.automation) {
      if (lane && candidate.id == lane.value()) {
        owner = &track;
        found = &candidate;
      }
    }
  }
  const auto feel = found ? feel_of(snapshot, *owner, found->target) : std::nullopt;
  if (!feel) {
    report_(QStringLiteral("That automation lane is gone"));
    return;
  }
  std::vector<application::AutomationService::AutomationPointMove> parsed;
  parsed.reserve(static_cast<std::size_t>(moves.size()));
  for (const auto& entry : moves) {
    const auto map = entry.toMap();
    const auto point = project::AutomationPointId::parse(map.value("id").toString().toStdString());
    const auto tick = tick_of_bar(snapshot, map.value("bar").toDouble());
    const auto value = map.value("value").toDouble();
    if (!point || !tick || !std::isfinite(value)) {
      report_(QStringLiteral("An automation point moved off the timeline"));
      return;
    }
    parsed.push_back({point.value(), *tick, from_lane(*feel, value)});
  }
  auto moved = session->automation().move_automation_points(lane.value(), parsed);
  if (!moved)
    return failure(moved.error());
  accepted(QStringLiteral("%1 moved").arg(feel->name));
}

void ArrangementAutomationController::remove_points(const QString& lane_id,
                                                    const QStringList& point_ids) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  std::vector<project::AutomationPointId> points;
  for (const auto& id : point_ids) {
    if (auto parsed = project::AutomationPointId::parse(id.toStdString()))
      points.push_back(parsed.value());
  }
  if (!lane || points.empty())
    return;
  auto removed = session->automation().remove_automation_points(lane.value(), points);
  if (!removed)
    return failure(removed.error());
  accepted(points.size() == 1 ? QStringLiteral("Automation point removed")
                              : QStringLiteral("%1 automation points removed").arg(points.size()));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
void ArrangementAutomationController::draw(const QString& track_id, const QString& key,
                                           double from_bar, double to_bar,
                                           const QVariantList& points) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto target = track ? target_of(track.value(), key) : std::nullopt;
  const auto& snapshot = session->snapshot();
  const auto owner = track ? std::ranges::find(snapshot.tracks, track.value(), &project::Track::id)
                           : snapshot.tracks.end();
  const auto feel =
      target && owner != snapshot.tracks.end() ? feel_of(snapshot, *owner, *target) : std::nullopt;
  const auto from = tick_of_bar(snapshot, from_bar);
  const auto to = tick_of_bar(snapshot, to_bar);
  if (!feel || !from || !to)
    return;
  std::vector<application::AutomationService::AutomationDrawPoint> drawn;
  for (const auto& entry : points) {
    const auto map = entry.toMap();
    const auto tick = tick_of_bar(snapshot, map.value("bar").toDouble());
    if (!tick)
      return;
    // Strokes may revisit a column; the last value there wins.
    if (!drawn.empty() && drawn.back().tick >= *tick) {
      if (drawn.back().tick == *tick)
        drawn.back().value = from_lane(*feel, map.value("value").toDouble());
      continue;
    }
    drawn.push_back({*tick, from_lane(*feel, map.value("value").toDouble())});
  }
  if (drawn.empty())
    return;
  auto applied = session->automation().draw_automation(track.value(), *target, *from, *to, drawn);
  if (!applied)
    return failure(applied.error());
  accepted(QStringLiteral("%1 drawn").arg(feel->name));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
void ArrangementAutomationController::set_curve(const QString& lane_id, const QString& point_id,
                                                const QString& shape) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  const auto parsed =
      shape == QStringLiteral("hold")          ? project::AutomationShape::hold
      : shape == QStringLiteral("quadratic")   ? project::AutomationShape::quadratic
      : shape == QStringLiteral("exponential") ? project::AutomationShape::exponential
      : shape == QStringLiteral("ease_in_out") ? project::AutomationShape::ease_in_out
                                               : project::AutomationShape::linear;
  if (!lane || !point)
    return;
  auto applied =
      session->automation().set_automation_segment_curve(lane.value(), point.value(), parsed, {});
  if (!applied)
    return failure(applied.error());
  accepted(QStringLiteral("Curve changed"));
}

void ArrangementAutomationController::bend(const QString& lane_id, const QString& point_id,
                                           double bend) {
  auto* session = editable_session();
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!session || !lane || !point || !std::isfinite(bend))
    return;
  bend = std::clamp(bend, -max_bend, max_bend);
  // A barely bowed line is straight.
  const bool straight = std::fabs(bend) < 0.02;
  auto applied = session->automation().set_automation_segment_curve(
      lane.value(), point.value(),
      straight ? project::AutomationShape::linear : project::AutomationShape::bezier,
      straight ? project::AutomationHandles{} : bow(bend));
  if (!applied)
    return failure(applied.error());
  accepted(straight ? QStringLiteral("Curve straightened") : QStringLiteral("Curve bent"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
void ArrangementAutomationController::set_point_value(const QString& lane_id,
                                                      const QString& point_id, double value) {
  auto* session = editable_session();
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!session || !lane || !point || !std::isfinite(value) || value < 0.0 || value > 1.0) {
    report_(QStringLiteral("Type a value the parameter can take"));
    return;
  }
  const auto& snapshot = session->snapshot();
  for (const auto& track : snapshot.tracks) {
    const auto found =
        std::ranges::find(track.automation, lane.value(), &project::AutomationLane::id);
    if (found == track.automation.end())
      continue;
    const auto feel = feel_of(snapshot, track, found->target);
    const auto saved =
        std::ranges::find(found->points, point.value(), &project::AutomationPoint::id);
    if (!feel || saved == found->points.end())
      break;
    // The point keeps its place: an elapsed lane's coordinate is a sample,
    // which the session takes as the tick playing there.
    auto tick = saved->coordinate;
    if (found->timebase == project::AutomationTimebase::elapsed) {
      const auto musical = snapshot.tempo.sample_to_tick({saved->coordinate}, snapshot.sample_rate);
      if (!musical)
        return failure(musical.error());
      tick = musical.value().value;
    }
    const application::AutomationService::AutomationPointMove move{point.value(), tick,
                                                                   from_lane(*feel, value)};
    auto moved = session->automation().move_automation_points(lane.value(), {&move, 1});
    if (!moved)
      return failure(moved.error());
    accepted(QStringLiteral("%1 set to %2").arg(feel->name, text_of(*feel, move.value)));
    return;
  }
  report_(QStringLiteral("That automation point is gone"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
void ArrangementAutomationController::reveal(const QString& track_id, const QString& key) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track || !target_of(track.value(), key)) {
    report_(QStringLiteral("That parameter cannot be automated on the arrangement"));
    return;
  }
  emit revealRequested(track_id, key);
}

void ArrangementAutomationController::clear_lane(const QString& lane_id) {
  auto* session = editable_session();
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  if (!session || !lane)
    return;
  for (const auto& track : session->snapshot().tracks) {
    const auto found =
        std::ranges::find(track.automation, lane.value(), &project::AutomationLane::id);
    if (found == track.automation.end())
      continue;
    std::vector<project::AutomationPointId> points;
    points.reserve(found->points.size());
    for (const auto& point : found->points)
      points.push_back(point.id);
    auto removed = session->automation().remove_automation_points(lane.value(), points);
    if (!removed)
      return failure(removed.error());
    accepted(QStringLiteral("Automation lane cleared"));
    return;
  }
  report_(QStringLiteral("That automation lane is gone"));
}

void ArrangementAutomationController::toggle_timebase(const QString& lane_id) {
  auto* session = editable_session();
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  if (!session || !lane)
    return;
  auto toggled = session->automation().toggle_automation_lane_timebase(lane.value());
  if (!toggled)
    return failure(toggled.error());
  accepted(QStringLiteral("Automation timebase switched"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
double ArrangementAutomationController::value_from_text(const QString& track_id, const QString& key,
                                                        const QString& typed) const {
  constexpr double unreadable = -1.0;
  const auto* session = session_();
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!session || !track)
    return unreadable;
  const auto& snapshot = session->snapshot();
  const auto owner = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  const auto target = target_of(track.value(), key);
  if (owner == snapshot.tracks.end() || !target)
    return unreadable;
  const auto feel = feel_of(snapshot, *owner, *target);
  const auto lane = feel ? lane_from_text(*feel, typed) : std::nullopt;
  return lane ? std::clamp(*lane, 0.0, 1.0) : unreadable;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
QString ArrangementAutomationController::value_text(const QString& track_id, const QString& key,
                                                    double value) const {
  const auto* session = session_();
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!session || !track)
    return {};
  const auto& snapshot = session->snapshot();
  const auto owner = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  const auto target = target_of(track.value(), key);
  if (owner == snapshot.tracks.end() || !target)
    return {};
  const auto feel = feel_of(snapshot, *owner, *target);
  return feel ? text_of(*feel, from_lane(*feel, value)) : QString();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML calls this positionally.
double ArrangementAutomationController::current_value(const QString& track_id,
                                                      const QString& key) const {
  const auto* session = session_();
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!session || !track)
    return 0.0;
  const auto target = target_of(track.value(), key);
  const auto& snapshot = session->snapshot();
  const auto owner = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  if (!target || owner == snapshot.tracks.end())
    return 0.0;
  const auto feel = feel_of(snapshot, *owner, *target);
  const auto domain = session->automation().automation_domain(track.value(), *target);
  return feel && domain ? to_lane(*feel, domain.value().default_value) : 0.0;
}

QVariantList ArrangementAutomationController::curve(const QString& lane_id, double from_bar,
                                                    double to_bar, int samples) const {
  QVariantList result;
  const auto* session = session_();
  const auto id = project::AutomationLaneId::parse(lane_id.toStdString());
  if (!session || !id || samples < 2 || samples > 8'192 || !(to_bar > from_bar))
    return result;
  const auto& snapshot = session->snapshot();
  for (const auto& track : snapshot.tracks) {
    const auto lane = std::ranges::find(track.automation, id.value(), &project::AutomationLane::id);
    if (lane == track.automation.end())
      continue;
    const auto feel = feel_of(snapshot, track, lane->target);
    if (!feel || lane->points.empty())
      return result;
    // Each point's bar, in point order.
    std::vector<double> bars;
    bars.reserve(lane->points.size());
    for (const auto& point : lane->points) {
      std::int64_t tick = point.coordinate;
      if (lane->timebase == project::AutomationTimebase::elapsed) {
        auto musical = snapshot.tempo.sample_to_tick({point.coordinate}, snapshot.sample_rate);
        tick = musical ? musical.value().value : 0;
      }
      bars.push_back(bar_of_tick(snapshot, tick).value_or(0.0));
    }
    result.reserve(samples);
    std::size_t segment = 0;
    for (int index = 0; index < samples; ++index) {
      const auto bar = from_bar + (to_bar - from_bar) * index / (samples - 1);
      while (segment + 1 < bars.size() && bars[segment + 1] <= bar)
        ++segment;
      double value = 0.0;
      if (bar <= bars.front()) {
        value = lane->points.front().value;
      } else if (segment + 1 >= bars.size()) {
        value = lane->points.back().value;
      } else {
        const auto& left = lane->points[segment];
        const auto& right = lane->points[segment + 1];
        const auto span = bars[segment + 1] - bars[segment];
        const auto u = span > 0.0 ? (bar - bars[segment]) / span : 1.0;
        // Straight on the lane: the lane is drawn in the space the engine
        // blends in (the lane's scale), so this is the path it plays.
        const auto from = to_lane(*feel, left.value);
        const auto to = to_lane(*feel, right.value);
        result.push_back(from + (to - from) * progress(left, u));
        continue;
      }
      result.push_back(to_lane(*feel, value));
    }
    return result;
  }
  return result;
}

} // namespace oscillate::quick
