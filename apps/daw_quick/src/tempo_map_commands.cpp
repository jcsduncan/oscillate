#include "tempo_map_commands.hpp"

#include "project_bridge_projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <utility>

namespace oscillate::quick {
namespace {

std::optional<timeline::TempoShape> tempo_shape(int index) {
  switch (index) {
  case 0:
    return timeline::TempoShape::step;
  case 1:
    return timeline::TempoShape::linear_bpm;
  case 2:
    return timeline::TempoShape::ease_in;
  case 3:
    return timeline::TempoShape::ease_out;
  case 4:
    return timeline::TempoShape::ease_in_out;
  default:
    return std::nullopt;
  }
}

} // namespace

TempoMapCommands::TempoMapCommands(Session session, CanEdit can_edit, Report report, Commit commit,
                                   Publish publish)
    : session_(std::move(session)), can_edit_(std::move(can_edit)), report_(std::move(report)),
      commit_(std::move(commit)), publish_(std::move(publish)) {}

void TempoMapCommands::clamp_page(int page_count) noexcept {
  page_ = std::clamp(page_, 0, std::max(1, page_count) - 1);
}

void TempoMapCommands::set_page(int page) {
  auto* current = session_();
  if (!current)
    return;
  const auto count = static_cast<int>(current->snapshot().tempo.events().size());
  const auto pages =
      std::max(1, (count + detail::tempo_events_per_page - 1) / detail::tempo_events_per_page);
  if (page < 0 || page >= pages || page == page_)
    return;
  page_ = page;
  publish_();
}

void TempoMapCommands::set_initial_bpm(double bpm) {
  if (!can_edit_())
    return;
  auto* current = session_();
  if (!current || !std::isfinite(bpm) || bpm <= 0.0 || bpm > 1000.0) {
    report_(QStringLiteral("Enter BPM above 0 and up to 1000"));
    return;
  }
  const auto& events = current->snapshot().tempo.events();
  if (events.empty()) {
    report_(QStringLiteral("Project has no initial tempo event"));
    return;
  }
  const auto before = current->snapshot().revision;
  const auto applied =
      current->song().set_tempo_event(events.front().tick, bpm, events.front().shape_to_next);
  if (!applied) {
    report_(
        QStringLiteral("Initial tempo: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  if (current->snapshot().revision == before)
    return;
  commit_();
  report_(QStringLiteral("Project BPM changed · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

void TempoMapCommands::set_initial_meter(int numerator, int denominator) {
  if (!can_edit_())
    return;
  auto* current = session_();
  if (!current || numerator < 1 || numerator > 32 || denominator < 1 || denominator > 64 ||
      (denominator & (denominator - 1)) != 0) {
    report_(
        QStringLiteral("Enter a meter from 1–32 beats with a power-of-two denominator up to 64"));
    return;
  }
  const auto before = current->snapshot().revision;
  const auto applied = current->song().set_initial_meter(static_cast<std::uint8_t>(numerator),
                                                         static_cast<std::uint8_t>(denominator));
  if (!applied) {
    report_(
        QStringLiteral("Initial meter: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  if (current->snapshot().revision == before)
    return;
  commit_();
  report_(QStringLiteral("Project meter changed · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

// QML invokes the bridge with primitive numeric arguments; this owner checks
// them before passing strongly typed positions and shapes to the session.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoMapCommands::add_event(int bar, int beat, double bpm, int shape_index) {
  if (!can_edit_())
    return;
  auto* current = session_();
  const auto shape = tempo_shape(shape_index);
  if (bar < 1 || bar > 9999 || beat < 1 || beat > 16 || !std::isfinite(bpm) || bpm <= 0.0 ||
      bpm > 1000.0 || !shape) {
    report_(QStringLiteral("Enter bar 1–9999, a valid beat, BPM above 0 up to 1000, and a shape"));
    return;
  }
  const auto tick = current->snapshot().meter.tick_at({bar, static_cast<std::uint8_t>(beat), 0});
  if (!tick) {
    report_(QStringLiteral("Tempo position: %1").arg(QString::fromStdString(tick.error().message)));
    return;
  }
  const auto& events = current->snapshot().tempo.events();
  const auto existing =
      std::ranges::lower_bound(events, tick.value(), {}, &timeline::TempoEvent::tick);
  if (existing != events.end() && existing->tick == tick.value()) {
    report_(QStringLiteral("A tempo event is already at that position; select it to edit"));
    return;
  }
  if (existing == events.end() && *shape != timeline::TempoShape::step) {
    report_(QStringLiteral(
        "The final tempo event must use Step; set the preceding event’s shape for a ramp"));
    return;
  }
  const auto applied = current->song().set_tempo_event(tick.value(), bpm, *shape);
  if (!applied) {
    report_(QStringLiteral("Tempo event: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  const auto& updated = current->snapshot().tempo.events();
  const auto inserted =
      std::ranges::lower_bound(updated, tick.value(), {}, &timeline::TempoEvent::tick);
  page_ =
      static_cast<int>(std::distance(updated.begin(), inserted)) / detail::tempo_events_per_page;
  commit_();
  report_(QStringLiteral("Project timing changed · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TempoMapCommands::update_event(const QString& tick_text, double bpm, int shape_index) {
  if (!can_edit_())
    return;
  auto* current = session_();
  bool valid_tick = false;
  const auto tick_value = tick_text.toLongLong(&valid_tick, 10);
  const auto shape = tempo_shape(shape_index);
  if (!valid_tick || tick_value < 0 || !std::isfinite(bpm) || bpm <= 0.0 || bpm > 1000.0 ||
      !shape) {
    report_(QStringLiteral("Select an event and enter BPM above 0 up to 1000 and a shape"));
    return;
  }
  const timeline::TickPosition tick{tick_value};
  const auto& events = current->snapshot().tempo.events();
  const auto existing = std::ranges::lower_bound(events, tick, {}, &timeline::TempoEvent::tick);
  if (existing == events.end() || existing->tick != tick) {
    report_(QStringLiteral("Tempo event changed; select it again before editing"));
    return;
  }
  if (std::next(existing) == events.end() && *shape != timeline::TempoShape::step) {
    report_(QStringLiteral(
        "The final tempo event must use Step; set the preceding event’s shape for a ramp"));
    return;
  }
  const auto before = current->snapshot().revision;
  const auto applied = current->song().set_tempo_event(tick, bpm, *shape);
  if (!applied) {
    report_(QStringLiteral("Tempo event: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  if (current->snapshot().revision == before) {
    report_(QStringLiteral("Tempo event unchanged"));
    return;
  }
  commit_();
  report_(QStringLiteral("Project timing changed · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

void TempoMapCommands::remove_event(const QString& tick_text) {
  if (!can_edit_())
    return;
  auto* current = session_();
  bool valid_tick = false;
  const auto tick_value = tick_text.toLongLong(&valid_tick, 10);
  if (!valid_tick || tick_value <= 0) {
    report_(QStringLiteral("The initial tempo event cannot be removed"));
    return;
  }
  const auto& events = current->snapshot().tempo.events();
  const auto existing = std::ranges::lower_bound(events, timeline::TickPosition{tick_value}, {},
                                                 &timeline::TempoEvent::tick);
  if (existing != events.end() && existing->tick.value == tick_value &&
      std::next(existing) == events.end() &&
      std::prev(existing)->shape_to_next != timeline::TempoShape::step) {
    report_(QStringLiteral("Set the preceding event to Step before removing the final event"));
    return;
  }
  const auto applied = current->song().remove_tempo_event({tick_value});
  if (!applied) {
    report_(QStringLiteral("Remove tempo event: %1")
                .arg(QString::fromStdString(applied.error().message)));
    return;
  }
  commit_();
  report_(QStringLiteral("Tempo event removed · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

} // namespace oscillate::quick
