#include "native_synth_document_model.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/nova/analogue.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::quick {
namespace {

using project::NativeCurve;
using project::NativeCurvePoint;
using project::NativeCurveSegment;
using project::NativeModRoute;

// Neighbouring curve points stay at least this far apart, so x is strictly
// increasing as the model requires.
constexpr double min_point_gap = 1.0 / 512.0;

std::string utf8(const QString& text) { return text.toStdString(); }

QString qstring(const std::string& text) { return QString::fromStdString(text); }

double clamp01(double value) { return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0; }

double quarters(timeline::TickPosition ticks) {
  return static_cast<double>(ticks.value) / static_cast<double>(timeline::ticks_per_quarter);
}

timeline::TickPosition ticks(double quarter_notes) {
  return timeline::TickPosition{static_cast<std::int64_t>(
      std::llround(quarter_notes * static_cast<double>(timeline::ticks_per_quarter)))};
}

QVariantMap point_map(const NativeCurvePoint& point) {
  return {{QStringLiteral("x"), point.x},
          {QStringLiteral("y"), point.y},
          {QStringLiteral("tension"), point.tension},
          {QStringLiteral("step"), point.segment == NativeCurveSegment::step}};
}

QVariantList point_list(const std::vector<NativeCurvePoint>& points) {
  QVariantList list;
  list.reserve(static_cast<qsizetype>(points.size()));
  for (const auto& point : points) {
    list.append(point_map(point));
  }
  return list;
}

std::vector<NativeCurvePoint> points_from(const QVariantList& list) {
  std::vector<NativeCurvePoint> points;
  points.reserve(static_cast<std::size_t>(list.size()));
  for (const auto& entry : list) {
    const QVariantMap map = entry.toMap();
    NativeCurvePoint point;
    point.x = clamp01(map.value(QStringLiteral("x")).toDouble());
    point.y = clamp01(map.value(QStringLiteral("y")).toDouble());
    point.tension = std::clamp(map.value(QStringLiteral("tension")).toDouble(), -1.0, 1.0);
    point.segment = map.value(QStringLiteral("step")).toBool() ? NativeCurveSegment::step
                                                               : NativeCurveSegment::curve;
    points.push_back(point);
  }
  return points;
}

// Sorted, pinned to x = 0 and x = 1, strictly increasing and within the
// model's point limit; empty when fewer than two points remain.
std::vector<NativeCurvePoint> tidy(std::vector<NativeCurvePoint> points) {
  if (points.size() < 2) {
    return {};
  }
  std::ranges::stable_sort(points, {}, &NativeCurvePoint::x);
  if (points.size() > project::max_native_curve_points) {
    points.erase(points.begin() + static_cast<std::ptrdiff_t>(project::max_native_curve_points - 1),
                 points.end() - 1);
  }
  points.front().x = 0.0;
  points.back().x = 1.0;
  std::vector<NativeCurvePoint> result;
  result.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    const bool last = i + 1 == points.size();
    if (!result.empty() && !last && points[i].x < result.back().x + min_point_gap) {
      continue; // Too close to the previous point to keep.
    }
    if (last && result.size() >= 2 && result.back().x > 1.0 - min_point_gap) {
      result.pop_back(); // An inner point crowding the end point gives way.
    }
    result.push_back(points[i]);
  }
  return result.size() < 2 ? std::vector<NativeCurvePoint>{} : result;
}

KeyedListModel::Row route_row(const NativeModRoute& route, const QString& amount) {
  return {QString::number(route.slot),
          {{QStringLiteral("slot"), static_cast<int>(route.slot)},
           {QStringLiteral("source"), qstring(route.source)},
           {QStringLiteral("aux"), qstring(route.aux)},
           {QStringLiteral("destination"), qstring(route.destination)},
           {QStringLiteral("bipolar"), route.bipolar},
           {QStringLiteral("tension"), route.tension},
           {QStringLiteral("scale"), route.scale},
           {QStringLiteral("amount_id"), amount}}};
}

} // namespace

NativeSynthDocumentModel::NativeSynthDocumentModel(NativeSynthPanelAdapter& adapter,
                                                   NativeSynthDeviceController& controller,
                                                   QObject* parent)
    : QObject(parent), adapter_(adapter), controller_(controller), routes_({}, this) {
  // The engine's own default document: what it plays for a curve the
  // document lacks, so what an editor shows (and edits start from) then.
  const std::string engine = utf8(controller_.engine_id());
  std::optional<project::NativeSynthDocument> defaults;
  if (const auto* synth = native_dsp::synth::find_engine(engine);
      synth != nullptr && synth->preset_document != nullptr)
    defaults = synth->preset_document(std::nullopt);
  else if (const auto* effect = native_dsp::effects::find_engine(engine);
           effect != nullptr && effect->preset_document != nullptr)
    defaults = effect->preset_document(std::nullopt);
  if (defaults) {
    for (auto& curve : defaults->curves)
      engine_curves_[QString::fromStdString(curve.id)] = std::move(curve.points);
  }
  refresh();
}

void NativeSynthDocumentModel::refresh() {
  auto next = adapter_.document();
  const bool routes = next.routes != document_.routes;
  const bool curves = next.curves != document_.curves;
  const bool chains = next.chains != document_.chains;
  const bool media = next.media != document_.media;
  const bool labels = next.labels != document_.labels;
  const bool sequences = next.sequences != document_.sequences;
  const bool analogue = next.voice_lanes != document_.voice_lanes;
  document_ = std::move(next);
  if (routes) {
    auto sorted = document_.routes;
    std::ranges::sort(sorted, {}, &NativeModRoute::slot);
    std::vector<KeyedListModel::Row> rows;
    rows.reserve(sorted.size());
    for (const auto& route : sorted) {
      rows.push_back(route_row(route, amount_id(static_cast<int>(route.slot))));
    }
    routes_.update(std::move(rows));
    ++routes_revision_;
    emit routes_changed();
  }
  if (curves) {
    ++curves_revision_;
    emit curves_changed();
  }
  if (chains) {
    ++chains_revision_;
    emit chains_changed();
  }
  if (media) {
    ++media_revision_;
    emit media_changed();
  }
  if (labels) {
    ++labels_revision_;
    emit labels_changed();
  }
  if (sequences) {
    ++sequences_revision_;
    emit sequences_changed();
  }
  if (analogue) {
    ++analogue_revision_;
    emit analogue_changed();
  }
}

bool NativeSynthDocumentModel::editable() const { return controller_.editable(); }

bool NativeSynthDocumentModel::submit(project::NativeSynthDocument next) {
  if (!editable() || next == document_) {
    return false;
  }
  adapter_.submit_document(std::move(next),
                           editing_ ? NativeSynthGesture::update : NativeSynthGesture::single);
  // Adopt whatever the project accepted, which may differ from the request.
  refresh();
  return true;
}

void NativeSynthDocumentModel::begin_edit() {
  if (editing_ || !editable()) {
    return;
  }
  editing_ = true;
  emit editing_changed();
  adapter_.submit_document(document_, NativeSynthGesture::begin);
  refresh();
}

void NativeSynthDocumentModel::commit_edit() {
  if (!editing_) {
    return;
  }
  editing_ = false;
  emit editing_changed();
  adapter_.submit_document(document_, NativeSynthGesture::commit);
  refresh();
}

void NativeSynthDocumentModel::cancel_edit() {
  if (!editing_) {
    return;
  }
  editing_ = false;
  emit editing_changed();
  adapter_.submit_document(document_, NativeSynthGesture::cancel);
  refresh();
}

// ---- Routes --------------------------------------------------------------------------

QString NativeSynthDocumentModel::amount_id(int slot) const {
  return QStringLiteral("%1.mod.%2.amount").arg(controller_.engine_id()).arg(slot);
}

QVariantMap NativeSynthDocumentModel::route(int slot) const {
  for (const auto& route : document_.routes) {
    if (static_cast<int>(route.slot) == slot) {
      return route_row(route, amount_id(slot)).data;
    }
  }
  return {};
}

QVariantList NativeSynthDocumentModel::routes_to(const QString& destination) const {
  const std::string target = utf8(destination);
  std::vector<const NativeModRoute*> found;
  for (const auto& route : document_.routes) {
    if (route.destination == target) {
      found.push_back(&route);
    }
  }
  std::ranges::sort(found, {}, &NativeModRoute::slot);
  QVariantList list;
  for (const auto* route : found) {
    list.append(route_row(*route, amount_id(static_cast<int>(route->slot))).data);
  }
  return list;
}

int NativeSynthDocumentModel::routes_from(const QString& source) const {
  const std::string from = utf8(source);
  return static_cast<int>(std::ranges::count(document_.routes, from, &NativeModRoute::source));
}

int NativeSynthDocumentModel::next_free_slot() const {
  constexpr auto slot_count = static_cast<std::uint32_t>(project::max_native_synth_routes);
  if (document_.routes.size() >= slot_count) {
    return -1;
  }
  std::vector<bool> used(slot_count, false);
  std::uint32_t highest = 0;
  bool any = false;
  for (const auto& route : document_.routes) {
    if (route.slot < slot_count) {
      used[route.slot] = true;
      highest = std::max(highest, route.slot);
      any = true;
    }
  }
  if (!any) {
    return 0;
  }
  if (highest + 1 < slot_count) {
    return static_cast<int>(highest + 1);
  }
  const auto free = std::ranges::find(used, false);
  return free == used.end() ? -1 : static_cast<int>(std::distance(used.begin(), free));
}

int NativeSynthDocumentModel::add_route(const QString& source, const QString& destination,
                                        double amount) {
  if (!editable() || source.isEmpty() || destination.isEmpty()) {
    return -1;
  }
  const std::string from = utf8(source);
  const std::string to = utf8(destination);
  for (const auto& route : document_.routes) {
    if (route.source == from && route.destination == to) {
      return static_cast<int>(route.slot);
    }
  }
  const int slot = next_free_slot();
  if (slot < 0) {
    return -1;
  }
  controller_.set_value(amount_id(slot), std::clamp(amount, -1.0, 1.0));
  auto next = document_;
  NativeModRoute route;
  route.slot = static_cast<std::uint32_t>(slot);
  route.source = from;
  route.destination = to;
  next.routes.push_back(std::move(route));
  return submit(std::move(next)) ? slot : -1;
}

bool NativeSynthDocumentModel::remove_route(int slot) {
  auto next = document_;
  const auto erased = std::erase_if(next.routes, [slot](const NativeModRoute& route) {
    return static_cast<int>(route.slot) == slot;
  });
  if (erased == 0 || !submit(std::move(next))) {
    return false;
  }
  controller_.reset_to_default(amount_id(slot));
  return true;
}

bool NativeSynthDocumentModel::set_route(int slot, const QVariantMap& changes) {
  auto next = document_;
  const auto found = std::ranges::find_if(next.routes, [slot](const NativeModRoute& route) {
    return static_cast<int>(route.slot) == slot;
  });
  if (found == next.routes.end()) {
    return false;
  }
  if (changes.contains(QStringLiteral("source"))) {
    found->source = utf8(changes.value(QStringLiteral("source")).toString());
  }
  if (changes.contains(QStringLiteral("aux"))) {
    found->aux = utf8(changes.value(QStringLiteral("aux")).toString());
  }
  if (changes.contains(QStringLiteral("destination"))) {
    found->destination = utf8(changes.value(QStringLiteral("destination")).toString());
  }
  if (changes.contains(QStringLiteral("bipolar"))) {
    found->bipolar = changes.value(QStringLiteral("bipolar")).toBool();
  }
  if (changes.contains(QStringLiteral("tension"))) {
    found->tension = std::clamp(changes.value(QStringLiteral("tension")).toDouble(), -1.0, 1.0);
  }
  if (changes.contains(QStringLiteral("scale"))) {
    found->scale = clamp01(changes.value(QStringLiteral("scale")).toDouble());
  }
  return submit(std::move(next));
}

void NativeSynthDocumentModel::set_source_polarity(const QString& source, bool bipolar) {
  auto next = document_;
  const std::string from = utf8(source);
  for (auto& route : next.routes) {
    if (route.source == from) {
      route.bipolar = bipolar;
    }
  }
  submit(std::move(next));
}

// ---- Curves --------------------------------------------------------------------------

QVariantList NativeSynthDocumentModel::curve(const QString& id) const {
  return point_list(working_curve(id).points);
}

bool NativeSynthDocumentModel::has_curve(const QString& id) const {
  const std::string key = utf8(id);
  return std::ranges::find(document_.curves, key, &NativeCurve::id) != document_.curves.end();
}

void NativeSynthDocumentModel::set_curve_default(const QString& id, const QVariantList& points) {
  auto tidied = tidy(points_from(points));
  if (tidied.empty()) {
    default_curves_.erase(id);
    return;
  }
  default_curves_[id] = std::move(tidied);
}

NativeCurve NativeSynthDocumentModel::working_curve(const QString& id) const {
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document_.curves, key, &NativeCurve::id);
  if (found != document_.curves.end()) {
    return *found;
  }
  if (const auto fallback = default_curves_.find(id); fallback != default_curves_.end())
    return {key, fallback->second};
  const auto engine = engine_curves_.find(id);
  return {key, engine == engine_curves_.end() ? std::vector<NativeCurvePoint>{} : engine->second};
}

bool NativeSynthDocumentModel::replace_curve(NativeCurve curve) {
  curve.points = tidy(std::move(curve.points));
  if (curve.points.empty()) {
    return false;
  }
  auto next = document_;
  const auto found = std::ranges::find(next.curves, curve.id, &NativeCurve::id);
  if (found != next.curves.end()) {
    *found = std::move(curve);
  } else {
    if (next.curves.size() >= project::max_native_synth_curves) {
      return false;
    }
    next.curves.push_back(std::move(curve));
  }
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::set_curve(const QString& id, const QVariantList& points) {
  return replace_curve({utf8(id), points_from(points)});
}

bool NativeSynthDocumentModel::remove_curve(const QString& id) {
  auto next = document_;
  const auto removed = std::erase_if(
      next.curves, [key = utf8(id)](const NativeCurve& curve) { return curve.id == key; });
  return removed > 0 && submit(std::move(next));
}

bool NativeSynthDocumentModel::move_curve_point(const QString& id, int index, double x, double y,
                                                bool link_ends) {
  auto curve = working_curve(id);
  auto& points = curve.points;
  if (index < 0 || static_cast<std::size_t>(index) >= points.size()) {
    return false;
  }
  const auto at = static_cast<std::size_t>(index);
  const double level = clamp01(y);
  if ((at == 0 || at + 1 == points.size()) && link_ends) {
    points.front().y = level;
    points.back().y = level;
  } else if (at == 0 || at + 1 == points.size()) {
    points[at].y = level;
  } else {
    points[at].x =
        std::clamp(clamp01(x), points[at - 1].x + min_point_gap, points[at + 1].x - min_point_gap);
    points[at].y = level;
  }
  return replace_curve(std::move(curve));
}

int NativeSynthDocumentModel::insert_curve_point(const QString& id, double x, double y) {
  auto curve = working_curve(id);
  auto& points = curve.points;
  if (points.size() < 2 || points.size() >= project::max_native_curve_points) {
    return -1;
  }
  const double at_x = clamp01(x);
  const auto after = std::ranges::find_if(
      points, [at_x](const NativeCurvePoint& point) { return point.x > at_x; });
  if (after == points.begin() || after == points.end() ||
      at_x - std::prev(after)->x < min_point_gap || after->x - at_x < min_point_gap) {
    return -1;
  }
  NativeCurvePoint point;
  point.x = at_x;
  point.y = clamp01(y);
  point.segment = std::prev(after)->segment;
  const auto index = std::distance(points.begin(), after);
  points.insert(after, point);
  return replace_curve(std::move(curve)) ? static_cast<int>(index) : -1;
}

bool NativeSynthDocumentModel::remove_curve_point(const QString& id, int index) {
  auto curve = working_curve(id);
  auto& points = curve.points;
  // The end points stay: a curve always spans 0..1.
  if (index <= 0 || static_cast<std::size_t>(index) + 1 >= points.size()) {
    return false;
  }
  points.erase(points.begin() + index);
  return replace_curve(std::move(curve));
}

bool NativeSynthDocumentModel::set_curve_tension(const QString& id, int index, double tension) {
  auto curve = working_curve(id);
  if (index < 0 || static_cast<std::size_t>(index) + 1 >= curve.points.size()) {
    return false;
  }
  curve.points[static_cast<std::size_t>(index)].tension =
      std::isfinite(tension) ? std::clamp(tension, -1.0, 1.0) : 0.0;
  return replace_curve(std::move(curve));
}

bool NativeSynthDocumentModel::set_curve_step(const QString& id, int index, bool step) {
  auto curve = working_curve(id);
  if (index < 0 || static_cast<std::size_t>(index) + 1 >= curve.points.size()) {
    return false;
  }
  curve.points[static_cast<std::size_t>(index)].segment =
      step ? NativeCurveSegment::step : NativeCurveSegment::curve;
  return replace_curve(std::move(curve));
}

// ---- Chains --------------------------------------------------------------------------

project::NativeChain* NativeSynthDocumentModel::find_chain(project::NativeSynthDocument& document,
                                                           const QString& id) const {
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document.chains, key, &project::NativeChain::id);
  return found == document.chains.end() ? nullptr : &*found;
}

QVariantList NativeSynthDocumentModel::chain(const QString& id) const {
  QVariantList list;
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document_.chains, key, &project::NativeChain::id);
  if (found == document_.chains.end()) {
    return list;
  }
  for (const auto& module : found->modules) {
    list.append(QVariantMap{{QStringLiteral("uid"), static_cast<int>(module.uid)},
                            {QStringLiteral("module"), qstring(module.module)}});
  }
  return list;
}

int NativeSynthDocumentModel::free_chain_uid(int max_uid) const {
  for (int uid = 1; uid <= max_uid; ++uid) {
    const bool used = std::ranges::any_of(document_.chains, [uid](const project::NativeChain& c) {
      return std::ranges::any_of(c.modules, [uid](const project::NativeChainSlot& module) {
        return static_cast<int>(module.uid) == uid;
      });
    });
    if (!used) {
      return uid;
    }
  }
  return -1;
}

bool NativeSynthDocumentModel::move_chain_module(const QString& chain, int from, int to) {
  auto next = document_;
  auto* target = find_chain(next, chain);
  if (target == nullptr) {
    return false;
  }
  auto& modules = target->modules;
  const int count = static_cast<int>(modules.size());
  if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
    return false;
  }
  auto moving = modules[static_cast<std::size_t>(from)];
  modules.erase(modules.begin() + from);
  modules.insert(modules.begin() + to, std::move(moving));
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::move_chain_module_to(const QString& from_chain, int from,
                                                    const QString& to_chain, int to) {
  if (from_chain == to_chain) {
    return move_chain_module(from_chain, from, to);
  }
  auto next = document_;
  auto* source = find_chain(next, from_chain);
  if (source == nullptr || from < 0 || from >= static_cast<int>(source->modules.size())) {
    return false;
  }
  if (find_chain(next, to_chain) == nullptr) {
    if (next.chains.size() >= project::max_native_synth_chains) {
      return false;
    }
    next.chains.push_back({utf8(to_chain), {}});
    source = find_chain(next, from_chain);
  }
  auto* target = find_chain(next, to_chain);
  if (target->modules.size() >= project::max_native_chain_slots) {
    return false;
  }
  auto moving = source->modules[static_cast<std::size_t>(from)];
  source->modules.erase(source->modules.begin() + from);
  const auto at = std::clamp<std::ptrdiff_t>(to, 0, std::ssize(target->modules));
  target->modules.insert(target->modules.begin() + at, std::move(moving));
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::insert_chain_module(const QString& chain, int index,
                                                   const QString& module, int uid) {
  if (uid <= 0 || module.isEmpty()) {
    return false;
  }
  auto next = document_;
  auto* target = find_chain(next, chain);
  if (target == nullptr) {
    if (next.chains.size() >= project::max_native_synth_chains) {
      return false;
    }
    next.chains.push_back({utf8(chain), {}});
    target = &next.chains.back();
  }
  if (target->modules.size() >= project::max_native_chain_slots) {
    return false;
  }
  const auto at = std::clamp<std::ptrdiff_t>(index, 0, std::ssize(target->modules));
  target->modules.insert(target->modules.begin() + at,
                         {static_cast<std::uint32_t>(uid), utf8(module)});
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::remove_chain_module(const QString& chain, int index) {
  auto next = document_;
  auto* target = find_chain(next, chain);
  if (target == nullptr || index < 0 || index >= static_cast<int>(target->modules.size())) {
    return false;
  }
  target->modules.erase(target->modules.begin() + index);
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::set_chain_module(const QString& chain, int index,
                                                const QString& module) {
  auto next = document_;
  auto* target = find_chain(next, chain);
  if (target == nullptr || module.isEmpty() || index < 0 ||
      index >= static_cast<int>(target->modules.size())) {
    return false;
  }
  target->modules[static_cast<std::size_t>(index)].module = utf8(module);
  return submit(std::move(next));
}

// ---- Media, labels, sequences --------------------------------------------------------

QVariantMap NativeSynthDocumentModel::media(const QString& id) const {
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document_.media, key, &project::NativeMediaSlot::id);
  if (found == document_.media.end()) {
    return {{QStringLiteral("kind"), QString()},
            {QStringLiteral("name"), QString()},
            {QStringLiteral("frame_length"), 0}};
  }
  const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&found->media);
  QVariantMap out{{QStringLiteral("kind"),
                   builtin != nullptr ? QStringLiteral("builtin") : QStringLiteral("managed")},
                  {QStringLiteral("name"), builtin != nullptr ? qstring(builtin->name) : QString()},
                  {QStringLiteral("frame_length"), static_cast<int>(found->frame_length)}};
  // A user's audio: its length, for the panel to show.
  if (const auto* managed = std::get_if<project::ManagedAudioReference>(&found->media)) {
    out.insert(QStringLiteral("frames"), static_cast<double>(managed->frames));
    out.insert(QStringLiteral("sample_rate"), static_cast<int>(managed->sample_rate));
  }
  return out;
}

bool NativeSynthDocumentModel::set_builtin_media(const QString& id, const QString& name,
                                                 int frame_length) {
  if (name.isEmpty() || frame_length < 0) {
    return false;
  }
  auto next = document_;
  project::NativeMediaSlot slot{utf8(id), project::NativeBuiltinMedia{utf8(name)},
                                static_cast<std::uint32_t>(frame_length)};
  const auto found = std::ranges::find(next.media, slot.id, &project::NativeMediaSlot::id);
  if (found != next.media.end()) {
    *found = std::move(slot);
  } else {
    if (next.media.size() >= project::max_native_synth_media) {
      return false;
    }
    next.media.push_back(std::move(slot));
  }
  return submit(std::move(next));
}

QString NativeSynthDocumentModel::label(const QString& id, const QString& fallback) const {
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document_.labels, key, &project::NativeLabel::id);
  return found == document_.labels.end() || found->text.empty() ? fallback : qstring(found->text);
}

bool NativeSynthDocumentModel::set_label(const QString& id, const QString& text) {
  // Trim to the model's limit without splitting a UTF-8 sequence.
  QString clean = text.trimmed();
  while (clean.toUtf8().size() > static_cast<qsizetype>(project::max_native_label_length)) {
    clean.chop(1);
  }
  auto next = document_;
  const std::string key = utf8(id);
  const auto found = std::ranges::find(next.labels, key, &project::NativeLabel::id);
  if (clean.isEmpty()) {
    if (found == next.labels.end()) {
      return false;
    }
    next.labels.erase(found);
  } else if (found != next.labels.end()) {
    found->text = utf8(clean);
  } else {
    if (next.labels.size() >= project::max_native_synth_labels) {
      return false;
    }
    next.labels.push_back({key, utf8(clean)});
  }
  return submit(std::move(next));
}

QVariantMap NativeSynthDocumentModel::sequence(const QString& id) const {
  const std::string key = utf8(id);
  const auto found = std::ranges::find(document_.sequences, key, &project::NativeSequence::id);
  if (found == document_.sequences.end()) {
    return {{QStringLiteral("length"), 0.0}, {QStringLiteral("notes"), QVariantList{}}};
  }
  QVariantList notes;
  for (const auto& note : found->notes) {
    notes.append(QVariantMap{{QStringLiteral("start"), quarters(note.start)},
                             {QStringLiteral("length"), quarters(note.length)},
                             {QStringLiteral("key"), static_cast<int>(note.key)},
                             {QStringLiteral("velocity"), static_cast<int>(note.velocity)}});
  }
  return {{QStringLiteral("length"), quarters(found->length)}, {QStringLiteral("notes"), notes}};
}

bool NativeSynthDocumentModel::toggle_sequence_note(const QString& id, double start, int key,
                                                    double length, int velocity,
                                                    double sequence_length) {
  if (!(start >= 0.0) || !(length > 0.0) || key < 0 || key > 127 || !(sequence_length > 0.0) ||
      start >= sequence_length) {
    return false;
  }
  auto next = document_;
  const std::string name = utf8(id);
  auto found = std::ranges::find(next.sequences, name, &project::NativeSequence::id);
  if (found == next.sequences.end()) {
    if (next.sequences.size() >= project::max_native_synth_sequences) {
      return false;
    }
    next.sequences.push_back({name, ticks(sequence_length), {}});
    found = std::prev(next.sequences.end());
  }
  auto& notes = found->notes;
  const auto at = ticks(start);
  const auto covering = std::ranges::find_if(notes, [&](const project::NativeSequenceNote& note) {
    return note.key == key && note.start <= at && at.value < note.start.value + note.length.value;
  });
  if (covering != notes.end()) {
    notes.erase(covering);
  } else {
    if (notes.size() >= project::max_native_sequence_notes) {
      return false;
    }
    project::NativeSequenceNote note{at, ticks(std::min(length, sequence_length - start)),
                                     static_cast<std::uint8_t>(key),
                                     static_cast<std::uint8_t>(std::clamp(velocity, 1, 127))};
    notes.insert(std::ranges::upper_bound(notes, at, {}, &project::NativeSequenceNote::start),
                 note);
  }
  return submit(std::move(next));
}

bool NativeSynthDocumentModel::set_sequence_length(const QString& id, double length) {
  if (!(length > 0.0) || length > 16.0) {
    return false;
  }
  auto next = document_;
  const std::string name = utf8(id);
  const auto end = ticks(length);
  auto found = std::ranges::find(next.sequences, name, &project::NativeSequence::id);
  if (found == next.sequences.end()) {
    if (next.sequences.size() >= project::max_native_synth_sequences) {
      return false;
    }
    next.sequences.push_back({name, end, {}});
    return submit(std::move(next));
  }
  found->length = end;
  std::erase_if(found->notes,
                [end](const project::NativeSequenceNote& note) { return note.start >= end; });
  for (auto& note : found->notes) {
    note.length.value = std::min(note.length.value, end.value - note.start.value);
  }
  return submit(std::move(next));
}

// ---- Nova Analogue: Voice Lanes ----

namespace {

namespace nova = native_dsp::synth::nova;

struct AnalogueLaneRef {
  nova::AnalogueKind kind;
  std::size_t osc;
  nova::AnalogueLane lane;
};

std::optional<AnalogueLaneRef> analogue_lane_ref(const QString& kind, int osc,
                                                 const QString& lane) {
  const auto k = nova::analogue_kind_from_id(utf8(kind));
  const auto l = nova::analogue_lane_from_id(utf8(lane));
  if (!k || !l || osc < 0 || static_cast<std::size_t>(osc) >= nova::analogue_oscillators)
    return std::nullopt;
  return AnalogueLaneRef{*k, static_cast<std::size_t>(osc), *l};
}

} // namespace

QVariantList NativeSynthDocumentModel::analogueLane(const QString& kind, int osc,
                                                    const QString& lane, int count) const {
  QVariantList result;
  const auto ref = analogue_lane_ref(kind, osc, lane);
  if (!ref || count <= 0)
    return result;
  const auto values =
      nova::analogue_lane_values(controller_.current_values(), document_, ref->kind, ref->osc,
                                 ref->lane, static_cast<std::size_t>(count));
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const auto& value : values)
    result.append(QVariantMap{{QStringLiteral("value"), value.value},
                              {QStringLiteral("edited"), value.edited}});
  return result;
}

void NativeSynthDocumentModel::setAnalogueLaneValue(const QString& kind, int osc,
                                                    const QString& lane, int voice, double value) {
  const auto ref = analogue_lane_ref(kind, osc, lane);
  if (!ref || voice < 0 || !editable())
    return;
  auto next = document_;
  if (nova::set_analogue_lane_value(next, ref->kind, ref->osc, ref->lane,
                                    static_cast<std::size_t>(voice), value))
    submit(std::move(next));
}

void NativeSynthDocumentModel::clearAnalogueLane(const QString& kind, int osc,
                                                 const QString& lane) {
  const auto ref = analogue_lane_ref(kind, osc, lane);
  if (!ref || !editable())
    return;
  auto next = document_;
  if (nova::clear_analogue_lane(next, ref->kind, ref->osc, ref->lane))
    submit(std::move(next));
}

} // namespace oscillate::quick
