#include "tempo_lane_controller.hpp"

#include "bar_ticks.hpp"
#include "oscillate/application/clip_warp_edit.hpp"

#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace oscillate::quick {
namespace {

using timeline::TempoShape;

struct ShapeName {
  TempoShape shape;
  std::string_view key;
  const char* name;
};
// Menu order: the ramps first, then a hold.
constexpr std::array shape_names{
    ShapeName{TempoShape::linear_bpm, "straight", "Straight"},
    ShapeName{TempoShape::ease_in_out, "s_curve", "S-curve"},
    ShapeName{TempoShape::ease_in, "ease_in", "Ease in"},
    ShapeName{TempoShape::ease_out, "ease_out", "Ease out"},
    ShapeName{TempoShape::step, "hold", "Hold"},
};

[[nodiscard]] std::optional<TempoShape> shape_of(const QString& key) {
  const auto text = key.toStdString();
  const auto found = std::ranges::find(shape_names, std::string_view{text}, &ShapeName::key);
  if (found == shape_names.end())
    return std::nullopt;
  return found->shape;
}

[[nodiscard]] QString key_of(TempoShape shape) {
  const auto found = std::ranges::find(shape_names, shape, &ShapeName::shape);
  return found == shape_names.end() ? QString{} : QString::fromUtf8(found->key.data());
}

// The lane edits tempi inside the range a track tempo can take.
constexpr double min_lane_bpm = timeline::min_detected_bpm;
constexpr double max_lane_bpm = timeline::max_detected_bpm;
// The scale reaches this far past the slowest and fastest point, and spans
// at least this much, so a steady tempo sits mid-lane.
constexpr double scale_margin_bpm = 2.0;
constexpr double min_scale_span_bpm = 8.0;
constexpr int min_curve_samples = 2;
constexpr int max_curve_samples = 8'192;

[[nodiscard]] bool lane_bpm(double bpm) noexcept {
  return std::isfinite(bpm) && bpm >= min_lane_bpm && bpm <= max_lane_bpm;
}

[[nodiscard]] std::optional<timeline::TickPosition> tick_text(const QString& text) {
  bool valid = false;
  const auto value = text.toLongLong(&valid, 10);
  if (!valid || value < 0)
    return std::nullopt;
  return timeline::TickPosition{value};
}

[[nodiscard]] std::optional<double> bpm_at_tick(const project::Snapshot& snapshot,
                                                std::int64_t tick) {
  const auto sample = snapshot.tempo.tick_to_sample({tick}, snapshot.sample_rate);
  double bpm = 0.0;
  if (!sample || !snapshot.tempo.bpm_at_sample_rt(sample.value(), snapshot.sample_rate, bpm))
    return std::nullopt;
  return bpm;
}

// The opening tempo the project controls accept.
constexpr double max_initial_bpm = 1'000.0;
// Meters from 1 to 32 beats, over a power of two up to 64.
constexpr int max_meter_numerator = 32;
constexpr int max_meter_denominator = 64;

} // namespace

TempoLaneController::TempoLaneController(Session session, CanEdit can_edit, Report report,
                                         Commit commit, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

QVariantList TempoLaneController::shapes() const {
  QVariantList list;
  for (const auto& entry : shape_names)
    list.push_back(QVariantMap{{"key", QString::fromUtf8(entry.key.data())},
                               {"name", QString::fromUtf8(entry.name)}});
  return list;
}

void TempoLaneController::refresh() {
  const auto* session = session_ ? session_() : nullptr;
  QVariantList points;
  double low = 0.0;
  double high = 0.0;
  if (session) {
    const auto& snapshot = session->snapshot();
    const auto& events = snapshot.tempo.events();
    for (std::size_t index = 0; index < events.size(); ++index) {
      const auto& event = events[index];
      const auto bar = bar_of_tick(snapshot, event.tick.value);
      if (!bar)
        continue;
      points.push_back(QVariantMap{{"tick", QString::number(event.tick.value)},
                                   {"bar", *bar},
                                   {"bpm", event.bpm},
                                   {"shape", key_of(event.shape_to_next)},
                                   {"initial", index == 0},
                                   {"last", index + 1 == events.size()}});
      low = index == 0 ? event.bpm : std::min(low, event.bpm);
      high = index == 0 ? event.bpm : std::max(high, event.bpm);
    }
    low = std::floor(low) - scale_margin_bpm;
    high = std::ceil(high) + scale_margin_bpm;
    if (high - low < min_scale_span_bpm) {
      const auto middle = (low + high) / 2.0;
      low = middle - min_scale_span_bpm / 2.0;
      high = middle + min_scale_span_bpm / 2.0;
    }
  }
  if (points == points_ && low == low_bpm_ && high == high_bpm_)
    return;
  points_ = std::move(points);
  low_bpm_ = low;
  high_bpm_ = high;
  emit changed();
}

application::ProjectSession* TempoLaneController::editable_session() {
  auto* session = session_ ? session_() : nullptr;
  if (session && can_edit_ && can_edit_())
    return session;
  return nullptr;
}

void TempoLaneController::finish(const foundation::Result<void>& applied, std::uint64_t before,
                                 const QString& done) {
  if (!applied) {
    report_(QStringLiteral("Tempo: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  const auto* session = session_();
  if (session->snapshot().revision == before)
    return;
  commit_();
  report_(done);
}

void TempoLaneController::set_initial_bpm(double bpm) {
  auto* session = editable_session();
  if (!session)
    return;
  if (!std::isfinite(bpm) || bpm <= 0.0 || bpm > max_initial_bpm) {
    report_(QStringLiteral("Enter BPM above 0 and up to %1").arg(max_initial_bpm));
    return;
  }
  const auto& events = session->snapshot().tempo.events();
  const auto before = session->snapshot().revision;
  finish(session->song().set_tempo_event(events.front().tick, bpm, events.front().shape_to_next),
         before, QStringLiteral("Project tempo %1 BPM").arg(bpm, 0, 'f', 2));
}

void TempoLaneController::set_initial_meter(int numerator, int denominator) {
  auto* session = editable_session();
  if (!session)
    return;
  if (numerator < 1 || numerator > max_meter_numerator || denominator < 1 ||
      denominator > max_meter_denominator || (denominator & (denominator - 1)) != 0) {
    report_(QStringLiteral("Enter a meter of 1–%1 beats over a power of two up to %2")
                .arg(max_meter_numerator)
                .arg(max_meter_denominator));
    return;
  }
  const auto before = session->snapshot().revision;
  finish(session->song().set_initial_meter(static_cast<std::uint8_t>(numerator),
                                           static_cast<std::uint8_t>(denominator)),
         before, QStringLiteral("Project meter %1/%2").arg(numerator).arg(denominator));
}

double TempoLaneController::bpm_at(double bar) const {
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return 0.0;
  const auto tick = tick_of_bar(session->snapshot(), bar);
  return tick ? bpm_at_tick(session->snapshot(), *tick).value_or(0.0) : 0.0;
}

QVariantList TempoLaneController::curve(double from_bar, double to_bar, int samples) const {
  const auto* session = session_ ? session_() : nullptr;
  if (!session || samples < min_curve_samples || samples > max_curve_samples ||
      !(to_bar > from_bar))
    return {};
  const auto& snapshot = session->snapshot();
  QVariantList values;
  values.reserve(samples);
  for (int index = 0; index < samples; ++index) {
    const auto bar = from_bar + (to_bar - from_bar) * index / (samples - 1);
    const auto tick = tick_of_bar(snapshot, std::max(0.0, bar));
    values.push_back(tick ? bpm_at_tick(snapshot, *tick).value_or(0.0) : 0.0);
  }
  return values;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoLaneController::add_point(double bar, double bpm) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto& snapshot = session->snapshot();
  const auto tick = tick_of_bar(snapshot, bar);
  if (!tick || *tick <= 0 || !lane_bpm(bpm)) {
    report_(QStringLiteral("Place a tempo point inside the song, at %1–%2 BPM")
                .arg(min_lane_bpm)
                .arg(max_lane_bpm));
    return;
  }
  const auto& events = snapshot.tempo.events();
  const auto after = std::ranges::upper_bound(events, timeline::TickPosition{*tick}, {},
                                              &timeline::TempoEvent::tick);
  if (after != events.begin() && std::prev(after)->tick.value == *tick) {
    report_(QStringLiteral("A tempo point is already there"));
    return;
  }
  // Inside a ramp the new point carries it on; after the last it holds.
  const auto shape = after == events.end() ? TempoShape::step : std::prev(after)->shape_to_next;
  const auto before = snapshot.revision;
  finish(session->song().set_tempo_event({*tick}, bpm, shape), before,
         QStringLiteral("Tempo point %1 BPM").arg(bpm, 0, 'f', 2));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoLaneController::move_point(const QString& tick, double bar, double bpm) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto from = tick_text(tick);
  const auto to = from && from->value == 0 ? std::optional<std::int64_t>{0}
                                           : tick_of_bar(session->snapshot(), bar);
  if (!from || !to || !lane_bpm(bpm)) {
    report_(QStringLiteral("Keep the tempo point inside the song, at %1–%2 BPM")
                .arg(min_lane_bpm)
                .arg(max_lane_bpm));
    return;
  }
  const auto before = session->snapshot().revision;
  finish(session->song().move_tempo_event(*from, {*to}, bpm), before,
         QStringLiteral("Tempo point moved to %1 BPM").arg(bpm, 0, 'f', 2));
}

void TempoLaneController::remove_point(const QString& tick) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto at = tick_text(tick);
  if (!at || at->value == 0) {
    report_(QStringLiteral("The song's opening tempo stays; drag it to change it"));
    return;
  }
  const auto before = session->snapshot().revision;
  finish(session->song().remove_tempo_event(*at), before, QStringLiteral("Tempo point removed"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoLaneController::set_shape(const QString& tick, const QString& shape) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto at = tick_text(tick);
  const auto chosen = shape_of(shape);
  const auto& events = session->snapshot().tempo.events();
  const auto event =
      at ? std::ranges::find(events, *at, &timeline::TempoEvent::tick) : events.end();
  if (!chosen || event == events.end() || std::next(event) == events.end()) {
    report_(QStringLiteral("Choose the way between two tempo points"));
    return;
  }
  const auto before = session->snapshot().revision;
  finish(session->song().set_tempo_event(event->tick, event->bpm, *chosen), before,
         QStringLiteral("Tempo ramp shape changed"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoLaneController::ramp(double from_bar, double to_bar, double end_bpm,
                               const QString& shape) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto& snapshot = session->snapshot();
  const auto from = tick_of_bar(snapshot, from_bar);
  const auto to = tick_of_bar(snapshot, to_bar);
  const auto chosen = shape_of(shape);
  if (!from || !to || *to <= *from || !lane_bpm(end_bpm) || !chosen) {
    report_(QStringLiteral("Drag across bars to draw a ramp to %1–%2 BPM")
                .arg(min_lane_bpm)
                .arg(max_lane_bpm));
    return;
  }
  const auto before = snapshot.revision;
  finish(session->song().set_tempo_ramp({*from}, {*to}, end_bpm, *chosen), before,
         QStringLiteral("Tempo ramp to %1 BPM").arg(end_bpm, 0, 'f', 2));
}

void TempoLaneController::ramp_to_clip(const QString& clip_id) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip) {
    report_(QStringLiteral("That clip no longer exists"));
    return;
  }
  const auto planned = application::plan_transition_ramp(session->snapshot(), clip.value());
  const auto before = session->snapshot().revision;
  finish(session->song().ramp_to_clip_tempo(clip.value()), before,
         planned ? QStringLiteral("Tempo ramps to this track's %1 BPM across the transition")
                       .arg(planned.value().bpm, 0, 'f', 2)
                 : QString{});
}

} // namespace oscillate::quick
