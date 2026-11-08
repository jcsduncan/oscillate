#include "knock_patterns_model.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::quick {
namespace {

namespace patterns = application::knock_patterns;
namespace knock = native_dsp::synth::knock;

// A grid click steps a cell through every kind and round again.
constexpr std::array<patterns::Cell, patterns::cell_kinds> cycle_order{
    patterns::Cell::off,   patterns::Cell::hit,    patterns::Cell::accent,
    patterns::Cell::ghost, patterns::Cell::chance, patterns::Cell::ratchet};
constexpr std::array<const char*, patterns::cell_kinds> cell_words{"off",   "hit",    "accent",
                                                                   "ghost", "chance", "ratchet"};

// Rolled seeds walk a full-period linear congruential sequence through
// 1..max_seed (the multiplier less one is a multiple of 3 × 11 × 101, the
// primes of 9 999, and the step is prime to it), so Roll visits every seed
// before it repeats one.
constexpr std::uint64_t seed_step_multiplier = 3'334;
constexpr std::uint64_t seed_step_increment = 4'327;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString role_word(knock::PadRole role) {
  switch (role) {
  case knock::PadRole::low:
    return QStringLiteral("low");
  case knock::PadRole::mid:
    return QStringLiteral("mid");
  case knock::PadRole::high:
    return QStringLiteral("high");
  }
  return QStringLiteral("mid");
}

QVariantList bar_cells(const patterns::Bar& bar) {
  QVariantList out;
  out.reserve(static_cast<qsizetype>(patterns::lane_count * patterns::steps_per_bar));
  for (const auto& lane : bar)
    for (const auto cell : lane)
      out.push_back(static_cast<int>(cell));
  return out;
}

double unit(double value) { return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0; }

} // namespace

KnockPatternsModel::KnockPatternsModel(NativeSynthDeviceController& controller,
                                       NativeSynthPanelAdapter* adapter, QObject* parent)
    : QObject(parent), controller_(controller), adapter_(adapter) {
  roles_ = patterns::roles_of(controller_.current_values());
  rebuild_loops();
  connect(&controller_, &NativeSynthDeviceController::values_changed, this,
          &KnockPatternsModel::follow_values);
  // Start on the kit's own top loop, else the library's first.
  const auto kit = controller_.preset_name();
  const auto& library = patterns::library();
  const auto own = std::ranges::find_if(library, [&](const patterns::LibraryEntry& entry) {
    return !entry.kit.empty() && text(entry.kit) == kit;
  });
  const auto& first = own != library.end() ? *own : library.front();
  load_loop(QString::fromStdString(first.name));
}

int KnockPatternsModel::lane_count() noexcept { return static_cast<int>(patterns::lane_count); }
int KnockPatternsModel::steps_per_bar() noexcept {
  return static_cast<int>(patterns::steps_per_bar);
}
int KnockPatternsModel::steps_per_quarter() noexcept {
  return static_cast<int>(patterns::steps_per_quarter);
}

QStringList KnockPatternsModel::style_names() {
  QStringList names;
  for (std::size_t style = 0; style < patterns::style_count; ++style)
    names.append(text(patterns::style_name(static_cast<patterns::Style>(style))));
  return names;
}

QVariantList KnockPatternsModel::bar_choices() {
  QVariantList out;
  for (const auto bars : patterns::bar_choices)
    out.push_back(static_cast<int>(bars));
  return out;
}

QVariantList KnockPatternsModel::evolve_choices() {
  QVariantList out;
  for (const auto bars : patterns::evolve_choices)
    out.push_back(static_cast<int>(bars));
  return out;
}

QStringList KnockPatternsModel::cell_names() {
  QStringList names;
  for (const auto* word : cell_words)
    names.append(QString::fromLatin1(word));
  return names;
}

QStringList KnockPatternsModel::roles() const {
  QStringList out;
  for (const auto role : roles_)
    out.append(role_word(role));
  return out;
}

void KnockPatternsModel::follow_values() {
  const auto roles = patterns::roles_of(controller_.current_values());
  if (roles == roles_)
    return;
  // The generated loops follow the pads' roles; the working pattern stays.
  roles_ = roles;
  rebuild_loops();
}

void KnockPatternsModel::rebuild_loops() {
  QVariantList loops;
  const auto& library = patterns::library();
  loops.reserve(static_cast<qsizetype>(library.size()));
  for (const auto& entry : library) {
    const auto loop = patterns::loop_pattern(entry, roles_);
    loops.push_back(
        QVariantMap{{QStringLiteral("name"), QString::fromStdString(entry.name)},
                    {QStringLiteral("kit"), QString::fromStdString(entry.kit)},
                    {QStringLiteral("style"), static_cast<int>(entry.style)},
                    {QStringLiteral("bars"), static_cast<int>(loop.bars.size())},
                    {QStringLiteral("thumb"),
                     loop.bars.empty() ? QVariantList{} : bar_cells(loop.bars.front())}});
  }
  loops_ = loops;
  emit loops_changed();
}

void KnockPatternsModel::touched() {
  view_bar_ =
      std::min(view_bar_, pattern_.bars.empty() ? std::size_t{0} : pattern_.bars.size() - 1);
  ++revision_;
  emit changed();
}

void KnockPatternsModel::reshape() {
  pattern_ = patterns::roll(settings_, roles_, pattern_, locks_);
  loop_name_.clear();
  touched();
}

void KnockPatternsModel::set_style(int style) {
  if (style < 0 || static_cast<std::size_t>(style) >= patterns::style_count ||
      style == this->style())
    return;
  settings_.style = static_cast<patterns::Style>(style);
  reshape();
}

void KnockPatternsModel::set_density(double value) {
  if (unit(value) == settings_.density)
    return;
  settings_.density = unit(value);
  reshape();
}

void KnockPatternsModel::set_syncopation(double value) {
  if (unit(value) == settings_.syncopation)
    return;
  settings_.syncopation = unit(value);
  reshape();
}

void KnockPatternsModel::set_ghosts(double value) {
  if (unit(value) == settings_.ghosts)
    return;
  settings_.ghosts = unit(value);
  reshape();
}

void KnockPatternsModel::set_ratchets(double value) {
  if (unit(value) == settings_.ratchets)
    return;
  settings_.ratchets = unit(value);
  reshape();
}

void KnockPatternsModel::set_chance(double value) {
  if (unit(value) == settings_.chance)
    return;
  settings_.chance = unit(value);
  reshape();
}

void KnockPatternsModel::set_variation(double value) {
  if (unit(value) == settings_.variation)
    return;
  settings_.variation = unit(value);
  reshape();
}

void KnockPatternsModel::set_bars(int bars) {
  if (bars <= 0 || std::ranges::find(patterns::bar_choices, static_cast<std::size_t>(bars)) ==
                       patterns::bar_choices.end())
    return;
  if (static_cast<std::size_t>(bars) == settings_.bars && pattern_.bars.size() == settings_.bars)
    return;
  settings_.bars = static_cast<std::size_t>(bars);
  reshape();
}

void KnockPatternsModel::set_evolve_bars(int bars) {
  if (bars < 0 ||
      std::ranges::find(patterns::evolve_choices, static_cast<std::size_t>(bars)) ==
          patterns::evolve_choices.end() ||
      static_cast<std::size_t>(bars) == evolve_bars_)
    return;
  evolve_bars_ = static_cast<std::size_t>(bars);
  touched();
}

void KnockPatternsModel::set_view_bar(int bar) {
  if (bar < 0 || static_cast<std::size_t>(bar) >= pattern_.bars.size() ||
      static_cast<std::size_t>(bar) == view_bar_)
    return;
  view_bar_ = static_cast<std::size_t>(bar);
  touched();
}

QVariantList KnockPatternsModel::cells() const {
  if (pattern_.bars.empty())
    return {};
  return bar_cells(pattern_.bars[view_bar_]);
}

QVariantList KnockPatternsModel::locked() const {
  QVariantList out;
  for (const bool lock : locks_)
    out.push_back(lock);
  return out;
}

int KnockPatternsModel::written_bars() const noexcept {
  return static_cast<int>(evolve_bars_ > 0 ? evolve_bars_ : pattern_.bars.size());
}

bool KnockPatternsModel::load_loop(const QString& name) {
  const auto* entry = patterns::find_loop(name.toStdString());
  if (entry == nullptr)
    return false;
  pattern_ = patterns::loop_pattern(*entry, roles_);
  if (!entry->fixed)
    settings_ = entry->settings;
  loop_name_ = name;
  view_bar_ = 0;
  message_.clear();
  touched();
  return true;
}

void KnockPatternsModel::roll() {
  settings_.seed = static_cast<std::uint32_t>(
      1 + (settings_.seed * seed_step_multiplier + seed_step_increment) % max_seed);
  reshape();
}

void KnockPatternsModel::mutate() {
  ++mutations_;
  pattern_ =
      patterns::mutate(pattern_, patterns::mutate_amount, settings_.seed + mutations_, locks_);
  touched();
}

void KnockPatternsModel::set_locked(int lane, bool locked) {
  if (lane < 0 || static_cast<std::size_t>(lane) >= patterns::lane_count ||
      locks_[static_cast<std::size_t>(lane)] == locked)
    return;
  locks_[static_cast<std::size_t>(lane)] = locked;
  touched();
}

int KnockPatternsModel::cell(int bar, int lane, int step) const {
  if (bar < 0 || lane < 0 || step < 0 || static_cast<std::size_t>(bar) >= pattern_.bars.size() ||
      static_cast<std::size_t>(lane) >= patterns::lane_count ||
      static_cast<std::size_t>(step) >= patterns::steps_per_bar)
    return 0;
  return static_cast<int>(pattern_.bars[static_cast<std::size_t>(bar)][static_cast<std::size_t>(
      lane)][static_cast<std::size_t>(step)]);
}

void KnockPatternsModel::cycle_cell(int lane, int step) {
  if (pattern_.bars.empty() || lane < 0 || step < 0 ||
      static_cast<std::size_t>(lane) >= patterns::lane_count ||
      static_cast<std::size_t>(step) >= patterns::steps_per_bar)
    return;
  auto& cell =
      pattern_.bars[view_bar_][static_cast<std::size_t>(lane)][static_cast<std::size_t>(step)];
  const auto at = std::ranges::find(cycle_order, cell);
  const auto index = static_cast<std::size_t>(at - cycle_order.begin());
  cell = cycle_order[(index + 1) % cycle_order.size()];
  touched();
}

patterns::Pattern KnockPatternsModel::written() const {
  if (evolve_bars_ == 0)
    return pattern_;
  return patterns::evolve(pattern_, {.total_bars = evolve_bars_, .seed = settings_.seed}, locks_);
}

int KnockPatternsModel::note_count() const {
  const auto clip = patterns::to_notes(written(), settings_.seed);
  return clip ? static_cast<int>(clip.value().notes.size()) : 0;
}

void KnockPatternsModel::refuse(QString text) {
  message_ = std::move(text);
  ++revision_;
  emit changed();
}

bool KnockPatternsModel::write_to_clip(int start_bar, const QStringList& selected_clip_ids) {
  if (adapter_ == nullptr) {
    refuse(QStringLiteral("Open a Knock track to write its clips"));
    return false;
  }
  if (patterns::hit_count(pattern_) == 0) {
    refuse(QStringLiteral("The pattern is empty"));
    return false;
  }
  auto clip = patterns::to_notes(written(), settings_.seed);
  if (!clip) {
    refuse(QString::fromStdString(clip.error().message));
    return false;
  }
  const auto bars = written_bars();
  const auto ids = adapter_->midi_clip_ids();
  for (const auto& selected : selected_clip_ids) {
    const auto id = selected.toStdString();
    if (std::ranges::find(ids, id) == ids.end())
      continue;
    if (!adapter_->replace_midi_clip(id, clip.value().length, std::move(clip.value().notes))) {
      const auto status = adapter_->status();
      refuse(status.message.empty() ? QStringLiteral("Could not write the clip")
                                    : QString::fromStdString(status.message));
      return false;
    }
    message_ = QStringLiteral("%1 bars written to the selected clip").arg(bars);
    ++revision_;
    emit changed();
    return true;
  }
  if (start_bar < 0) {
    refuse(QStringLiteral("Choose a bar in the song"));
    return false;
  }
  if (!adapter_->add_midi_clip(start_bar, clip.value().length, std::move(clip.value().notes))) {
    const auto status = adapter_->status();
    refuse(status.message.empty() ? QStringLiteral("Could not write the clip")
                                  : QString::fromStdString(status.message));
    return false;
  }
  message_ = QStringLiteral("%1 bars written to a new clip at bar %2").arg(bars).arg(start_bar + 1);
  ++revision_;
  emit changed();
  return true;
}

std::vector<application::PreviewNote>
KnockPatternsModel::audition_notes(double bpm, std::uint32_t sample_rate, std::size_t limit) const {
  const auto clip = patterns::to_notes(pattern_, settings_.seed);
  if (!clip)
    return {};
  return patterns::preview_notes(clip.value(), bpm, sample_rate, limit);
}

} // namespace oscillate::quick
