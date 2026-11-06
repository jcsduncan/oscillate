#include "arrange_controller.hpp"

#include "oscillate/application/project_session.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {
namespace arrange = application::arrange;

constexpr int default_loop_bars = 16;
constexpr int seconds_per_minute = 60;

[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }
[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QVariantMap span(double start, double end, const QString& label,
                               const QString& detail) {
  return {{QStringLiteral("start"), start},
          {QStringLiteral("end"), end},
          {QStringLiteral("label"), label},
          {QStringLiteral("detail"), detail}};
}

} // namespace

ArrangeController::ArrangeController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)) {}

application::ProjectSession* ArrangeController::session() const {
  return hooks_.session ? hooks_.session() : nullptr;
}

void ArrangeController::set_status(QString status) {
  // `changed` also refreshes the rest of the view, so it is sent either way.
  status_.set(std::move(status));
  emit changed();
}

// ---- Opening and planning ------------------------------------------------------------

void ArrangeController::open() {
  if (open_)
    return;
  open_ = true;
  options_ = {};
  if (const auto loop = hooks_.loop ? hooks_.loop() : std::nullopt) {
    options_.loop_start_bar = loop->first;
    options_.loop_bars = loop->second - loop->first >= default_loop_bars
                             ? default_loop_bars
                             : static_cast<int>(arrange::loop_choices.front());
  }
  status_.set(
      hooks_.loop && hooks_.loop()
          ? QString()
          : QStringLiteral(
                "No loop set: arranging from bars 1–16. Drag across the ruler to pick one."));
  replan();
  emit changed();
}

void ArrangeController::close() {
  if (!open_)
    return;
  if (!withdraw())
    applied_revision_.reset();
  open_ = false;
  emit changed();
}

void ArrangeController::replan() {
  auto* current = session();
  if (current == nullptr) {
    plan_.reset();
    error_ = QStringLiteral("Open a project first");
    return;
  }
  auto planned = arrange::plan(current->snapshot(), options_);
  if (!planned) {
    plan_.reset();
    error_ = text(planned.error().message);
    return;
  }
  plan_ = std::move(planned).value();
  options_ = plan_->options;
  error_.clear();
}

void ArrangeController::update() {
  const bool was_listening = listening();
  if (was_listening && !withdraw())
    return;
  replan();
  if (was_listening && plan_)
    static_cast<void>(apply());
  emit changed();
}

void ArrangeController::set_loop_bars(int bars) {
  if (std::ranges::find(arrange::loop_choices, bars) == arrange::loop_choices.end())
    return;
  options_.loop_bars = bars;
  update();
}

void ArrangeController::set_style(int style) {
  if (style < 0 || style >= static_cast<int>(arrange::template_count))
    return;
  options_.style = static_cast<arrange::Template>(style);
  options_.length_bars = 0;
  options_.sections.clear();
  update();
}

void ArrangeController::set_length_bars(int bars) {
  const auto snapped = std::clamp<std::int64_t>(
      static_cast<std::int64_t>(std::lround(static_cast<double>(bars) / arrange::phrase_bars)) *
          arrange::phrase_bars,
      arrange::min_length_bars(options_.style, options_.edge_bars), arrange::max_length_bars);
  if (snapped == options_.length_bars)
    return;
  options_.length_bars = snapped;
  options_.sections.clear();
  update();
}

void ArrangeController::set_edge_bars(int bars) {
  if (std::ranges::find(arrange::edge_choices, bars) == arrange::edge_choices.end())
    return;
  options_.edge_bars = bars;
  options_.length_bars =
      std::max(options_.length_bars, arrange::min_length_bars(options_.style, bars));
  options_.sections.clear();
  update();
}

void ArrangeController::set_breakdown(double position) {
  options_.breakdown = std::clamp(position, 0.0, 1.0);
  options_.sections.clear();
  update();
}

void ArrangeController::set_variation(double amount) {
  options_.variation = std::clamp(amount, 0.0, 1.0);
  update();
}

void ArrangeController::set_transitions(bool on) {
  options_.transitions = on;
  update();
}

void ArrangeController::toggle_lock(const QString& track_id) {
  auto id = project::TrackId::parse(track_id.toStdString());
  if (!id)
    return;
  auto& locked = options_.locked;
  if (const auto found = std::ranges::find(locked, id.value()); found != locked.end())
    locked.erase(found);
  else
    locked.push_back(id.value());
  update();
}

void ArrangeController::set_role(const QString& track_id, int role) {
  auto id = project::TrackId::parse(track_id.toStdString());
  if (!id || role >= static_cast<int>(arrange::role_count))
    return;
  auto& roles = options_.roles;
  std::erase_if(roles, [&](const auto& entry) { return entry.first == id.value(); });
  if (role >= 0)
    roles.emplace_back(id.value(), static_cast<arrange::Role>(role));
  update();
}

void ArrangeController::move_section_edge(int index, int bar) {
  if (!plan_)
    return;
  auto moved =
      arrange::move_edge(plan_->sections, static_cast<std::size_t>(std::max(0, index)), bar);
  if (moved == plan_->sections)
    return;
  options_.sections = std::move(moved);
  update();
}

void ArrangeController::regenerate() {
  ++options_.seed;
  update();
}

// ---- Hearing, keeping and binning ----------------------------------------------------

bool ArrangeController::apply() {
  auto* current = session();
  if (!plan_ || current == nullptr)
    return false;
  if (hooks_.can_edit && !hooks_.can_edit())
    return false;
  busy_ = true;
  auto done = current->song().apply_arrangement(*plan_);
  if (done) {
    applied_revision_ = current->snapshot().revision;
    if (hooks_.commit)
      hooks_.commit();
  }
  busy_ = false;
  if (!done) {
    set_status(QStringLiteral("Couldn't arrange: %1").arg(text(done.error().message)));
    return false;
  }
  return true;
}

bool ArrangeController::withdraw() {
  auto* current = session();
  if (!applied_revision_ || current == nullptr)
    return true;
  if (current->snapshot().revision != *applied_revision_) {
    applied_revision_.reset();
    set_status(QStringLiteral("You edited the arrangement, so it stays: undo it from the history"));
    return false;
  }
  if (hooks_.can_edit && !hooks_.can_edit())
    return false;
  busy_ = true;
  auto undone = current->history().undo();
  if (undone) {
    applied_revision_.reset();
    if (hooks_.commit)
      hooks_.commit();
  }
  busy_ = false;
  if (!undone) {
    set_status(
        QStringLiteral("Couldn't take the proposal back: %1").arg(text(undone.error().message)));
    return false;
  }
  return true;
}

void ArrangeController::listen(bool on) {
  if (on == listening())
    return;
  if (on) {
    if (apply())
      set_status(QStringLiteral("Hearing the proposal: Keep it, or Bin to put the loop back"));
  } else if (withdraw()) {
    replan();
    set_status(QStringLiteral("Back to the loop"));
  }
  emit changed();
}

void ArrangeController::keep() {
  if (!listening() && !apply())
    return;
  applied_revision_.reset();
  open_ = false;
  set_status(QStringLiteral("Arranged: one step in the history, Ctrl+Z takes it back"));
  emit closeRequested();
}

void ArrangeController::bin() {
  if (!withdraw())
    return;
  open_ = false;
  set_status(QStringLiteral("Binned: the project is as it was"));
  emit closeRequested();
}

void ArrangeController::project_changed() {
  if (busy_)
    return;
  auto* current = session();
  if (applied_revision_) {
    if (current != nullptr && current->snapshot().revision == *applied_revision_)
      return;
    // Edited or undone from elsewhere: the proposal is the user's now.
    applied_revision_.reset();
  }
  if (open_)
    replan();
  emit changed();
}

// ---- Reading ------------------------------------------------------------------------

int ArrangeController::loop_start_bar() const noexcept {
  return static_cast<int>(options_.loop_start_bar);
}
int ArrangeController::loop_bars() const noexcept { return static_cast<int>(options_.loop_bars); }
int ArrangeController::style() const noexcept { return static_cast<int>(options_.style); }
int ArrangeController::length_bars() const noexcept {
  return static_cast<int>(plan_ ? plan_->length_bars : options_.length_bars);
}
int ArrangeController::min_length_bars() const noexcept {
  return static_cast<int>(arrange::min_length_bars(options_.style, options_.edge_bars));
}
int ArrangeController::max_length_bars() const noexcept {
  return static_cast<int>(arrange::max_length_bars);
}
int ArrangeController::edge_bars() const noexcept { return static_cast<int>(options_.edge_bars); }
int ArrangeController::phrase_bars() const noexcept {
  return static_cast<int>(arrange::phrase_bars);
}
int ArrangeController::change_bars() const noexcept {
  return static_cast<int>(arrange::change_bars);
}

double ArrangeController::bpm() const {
  const auto* current = session();
  if (current == nullptr || current->snapshot().tempo.events().empty())
    return 0.0;
  return current->snapshot().tempo.events().front().bpm;
}

QString ArrangeController::length_text() const {
  const auto* current = session();
  if (current == nullptr || !plan_)
    return {};
  const auto& state = current->snapshot();
  auto end = state.meter.tick_at({plan_->length_bars + 1, 1, 0});
  if (!end)
    return {};
  auto sample = state.tempo.tick_to_sample(end.value(), state.sample_rate);
  if (!sample || state.sample_rate == 0)
    return {};
  const auto seconds =
      static_cast<int>(std::lround(static_cast<double>(sample.value().value) / state.sample_rate));
  return QStringLiteral("%1:%2")
      .arg(seconds / seconds_per_minute)
      .arg(seconds % seconds_per_minute, 2, 10, QLatin1Char('0'));
}

QVariantList ArrangeController::templates() const {
  QVariantList result;
  for (std::size_t i = 0; i < arrange::template_count; ++i) {
    const auto& facts = arrange::templates[i];
    const auto seconds = static_cast<int>(facts.seconds);
    result.push_back(QVariantMap{{QStringLiteral("id"), static_cast<int>(i)},
                                 {QStringLiteral("name"), text(facts.name)},
                                 {QStringLiteral("length"), QStringLiteral("%1:%2")
                                                                .arg(seconds / seconds_per_minute)
                                                                .arg(seconds % seconds_per_minute,
                                                                     2, 10, QLatin1Char('0'))}});
  }
  return result;
}

QVariantList ArrangeController::loop_choices() const {
  QVariantList result;
  for (const auto bars : arrange::loop_choices)
    result.push_back(static_cast<int>(bars));
  return result;
}

QVariantList ArrangeController::edge_choices() const {
  QVariantList result;
  for (const auto bars : arrange::edge_choices)
    result.push_back(static_cast<int>(bars));
  return result;
}

QStringList ArrangeController::role_names() const {
  QStringList result;
  for (const auto name : arrange::role_names)
    result.push_back(text(name));
  return result;
}

QVariantList ArrangeController::tracks() const {
  QVariantList result;
  if (!plan_)
    return result;
  for (const auto& finding : plan_->findings) {
    QVariantList runs;
    const auto part = std::ranges::find(plan_->parts, finding.track, &arrange::TrackPart::track);
    if (part != plan_->parts.end()) {
      const auto count = static_cast<int>(part->plays.size());
      for (int bar = 0; bar < count;) {
        if (!part->plays[static_cast<std::size_t>(bar)]) {
          ++bar;
          continue;
        }
        int end = bar;
        while (end < count && part->plays[static_cast<std::size_t>(end)])
          ++end;
        runs.push_back(QVariantMap{{QStringLiteral("start"), bar}, {QStringLiteral("end"), end}});
        bar = end;
      }
    }
    QVariantList fills;
    for (const auto& fill : plan_->fills)
      if (fill.track == finding.track)
        fills.push_back(static_cast<int>(fill.bar));
    result.push_back(QVariantMap{{QStringLiteral("id"), text(finding.track.to_string())},
                                 {QStringLiteral("name"), text(finding.name)},
                                 {QStringLiteral("role"), text(arrange::role_name(finding.role))},
                                 {QStringLiteral("roleIndex"), static_cast<int>(finding.role)},
                                 {QStringLiteral("detected"), finding.detected},
                                 {QStringLiteral("evidence"), text(finding.evidence)},
                                 {QStringLiteral("locked"), finding.locked},
                                 {QStringLiteral("unsupported"), text(finding.unsupported)},
                                 {QStringLiteral("hasContent"), finding.has_content},
                                 {QStringLiteral("arranged"), finding.arranged()},
                                 {QStringLiteral("runs"), runs},
                                 {QStringLiteral("fills"), fills}});
  }
  return result;
}

QVariantList ArrangeController::sections() const {
  QVariantList result;
  if (!plan_)
    return result;
  for (std::size_t i = 0; i < plan_->sections.size(); ++i) {
    const auto& section = plan_->sections[i];
    result.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                    {QStringLiteral("kind"), static_cast<int>(section.kind)},
                    {QStringLiteral("name"), text(arrange::section_name(section.kind))},
                    {QStringLiteral("start"), static_cast<int>(section.start_bar)},
                    {QStringLiteral("bars"), static_cast<int>(section.bars)}});
  }
  return result;
}

QVariantList ArrangeController::lanes() const {
  QVariantList result;
  if (!plan_)
    return result;
  const auto name_of = [&](project::TrackId track) {
    const auto found = std::ranges::find(plan_->findings, track, &arrange::TrackFinding::track);
    return found == plan_->findings.end() ? QString() : text(found->name);
  };
  const auto lane = [&](const QString& kind, const QString& label, QVariantList items) {
    result.push_back(QVariantMap{{QStringLiteral("kind"), kind},
                                 {QStringLiteral("label"), label},
                                 {QStringLiteral("items"), items}});
  };
  QVariantList low;
  QVariantList high;
  for (const auto& sweep : plan_->sweeps)
    (sweep.high_pass ? high : low)
        .push_back(span(static_cast<double>(sweep.start_bar), static_cast<double>(sweep.end_bar),
                        name_of(sweep.track), text(sweep.reason)));
  lane(QStringLiteral("low-pass"), QStringLiteral("Low-pass sweeps"), low);
  lane(QStringLiteral("high-pass"), QStringLiteral("High-pass into the drop"), high);
  QVariantList throws;
  for (const auto& thrown : plan_->throws)
    throws.push_back(span(thrown.start_bar, thrown.start_bar + thrown.bars, name_of(thrown.track),
                          text(thrown.reason)));
  lane(QStringLiteral("throws"), QStringLiteral("Send throws"), throws);
  QVariantList fills;
  for (const auto& fill : plan_->fills)
    fills.push_back(span(static_cast<double>(fill.bar), static_cast<double>(fill.bar + 1),
                         name_of(fill.track), text(fill.reason)));
  lane(QStringLiteral("fills"), QStringLiteral("Chop fills"), fills);
  QVariantList transitions;
  for (const auto& transition : plan_->transitions)
    transitions.push_back(
        span(static_cast<double>(transition.start_bar),
             static_cast<double>(transition.start_bar + transition.bars),
             text(arrange::transition_facts[static_cast<std::size_t>(transition.kind)].track_name),
             text(transition.reason)));
  lane(QStringLiteral("transitions"), QStringLiteral("Transitions"), transitions);
  return result;
}

QVariantList ArrangeController::checks() const {
  QVariantList result;
  if (!plan_)
    return result;
  for (const auto& check : plan_->checks) {
    const auto status = check.status == arrange::CheckStatus::held    ? QStringLiteral("held")
                        : check.status == arrange::CheckStatus::added ? QStringLiteral("added")
                                                                      : QStringLiteral("failed");
    result.push_back(QVariantMap{{QStringLiteral("bar"), static_cast<int>(check.bar)},
                                 {QStringLiteral("status"), status},
                                 {QStringLiteral("reason"), text(check.reason)}});
  }
  return result;
}

QVariantList ArrangeController::rules() const {
  QVariantList result;
  if (!plan_)
    return result;
  for (const auto& rule : arrange::rules(*plan_))
    result.push_back(QVariantMap{{QStringLiteral("text"), text(rule.text)},
                                 {QStringLiteral("holds"), rule.holds}});
  return result;
}

QVariantList ArrangeController::decisions() const {
  QVariantList result;
  if (!plan_)
    return result;
  for (const auto& decision : plan_->decisions)
    result.push_back(QVariantMap{{QStringLiteral("bar"), static_cast<int>(decision.bar)},
                                 {QStringLiteral("text"), text(decision.text)}});
  return result;
}

} // namespace oscillate::quick
