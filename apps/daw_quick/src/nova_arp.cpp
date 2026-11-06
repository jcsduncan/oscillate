#include "nova_arp.hpp"

#include "native_synth_device_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace oscillate::quick {

namespace {

namespace arp = application::nova_arp;
namespace nova = native_dsp::synth::nova;

// The panel's mode groups: each mode's group, in mode order.
constexpr std::array<std::string_view, nova::arp_mode_count> mode_groups{
    "Order", "Order",  "Order",  "Order",  "Order",   "Order",   "Order", "Pedal",
    "Pedal", "Chance", "Chance", "Chance", "Harmony", "Harmony", "Drawn", "Rhythm"};

[[nodiscard]] QString text(std::string_view view) {
  return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

[[nodiscard]] QString kind_name(nova::ArpLaneKind kind) {
  switch (kind) {
  case nova::ArpLaneKind::level:
    return QStringLiteral("level");
  case nova::ArpLaneKind::integer:
    return QStringLiteral("integer");
  case nova::ArpLaneKind::toggle:
    return QStringLiteral("toggle");
  case nova::ArpLaneKind::choice:
    return QStringLiteral("choice");
  }
  return {};
}

[[nodiscard]] std::optional<std::size_t> lane_index(const QString& id) {
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    if (text(nova::arp_lane_specs[l].id) == id)
      return l;
  }
  return std::nullopt;
}

} // namespace

NovaArp::NovaArp(NativeSynthDeviceController& controller, QObject* parent)
    : QObject(parent), controller_(controller) {}

QVariantList NovaArp::patterns() const {
  QVariantList list;
  const auto library = arp::factory_patterns();
  for (std::size_t i = 0; i < library.size(); ++i) {
    const auto& p = library[i];
    list.append(QVariantMap{
        {QStringLiteral("index"), static_cast<int>(i)},
        {QStringLiteral("name"), text(p.name)},
        {QStringLiteral("category"), text(p.category)},
        {QStringLiteral("mode"), text(nova::arp_mode_names[static_cast<std::size_t>(p.mode)])},
        {QStringLiteral("rate"), text(p.rate)}});
  }
  return list;
}

QStringList NovaArp::categories() const {
  QStringList list;
  for (const auto category : arp::categories)
    list.append(text(category));
  return list;
}

QVariantList NovaArp::modes() const {
  QVariantList list;
  for (std::size_t m = 0; m < nova::arp_mode_count; ++m) {
    list.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(m)},
                            {QStringLiteral("name"), text(nova::arp_mode_names[m])},
                            {QStringLiteral("group"), text(mode_groups[m])}});
  }
  return list;
}

QVariantList NovaArp::lanes() const {
  QVariantList list;
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    const auto& spec = nova::arp_lane_specs[l];
    QStringList choices;
    if (spec.kind == nova::ArpLaneKind::choice) {
      const auto lane = static_cast<nova::ArpLane>(l);
      if (lane == nova::ArpLane::condition) {
        for (const auto name : nova::arp_condition_names)
          choices.append(text(name));
      } else {
        for (const auto name : nova::arp_chord_names)
          choices.append(text(name));
      }
    }
    list.append(QVariantMap{{QStringLiteral("id"), text(spec.id)},
                            {QStringLiteral("name"), text(spec.name)},
                            {QStringLiteral("kind"), kind_name(spec.kind)},
                            {QStringLiteral("minimum"), spec.minimum},
                            {QStringLiteral("maximum"), spec.maximum},
                            {QStringLiteral("fallback"), spec.fallback},
                            {QStringLiteral("choices"), choices}});
  }
  return list;
}

int NovaArp::max_steps() const noexcept { return static_cast<int>(nova::arp_max_steps); }

QStringList NovaArp::locks() const {
  QStringList list;
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    if (locks_[l])
      list.append(text(nova::arp_lane_specs[l].id));
  }
  return list;
}

void NovaArp::set_amount(double amount) {
  const double clean = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : amount_;
  if (clean == amount_)
    return;
  amount_ = clean;
  emit mutation_changed();
}

bool NovaArp::apply_pattern(int index) {
  const auto library = arp::factory_patterns();
  if (index < 0 || static_cast<std::size_t>(index) >= library.size()) {
    set_message(QStringLiteral("No such pattern"));
    return false;
  }
  const auto& pattern = library[static_cast<std::size_t>(index)];
  auto values = arp::apply(controller_.table(), controller_.current_values(), pattern, locks_);
  if (!values) {
    set_message(QString::fromStdString(values.error().message));
    return false;
  }
  const QString name = text(pattern.name);
  if (!commit(std::move(values.value()), &name, nullptr))
    return false;
  base_ = controller_.current_values();
  if (mutation_ != 0) {
    mutation_ = 0;
    emit mutation_changed();
  }
  return true;
}

int NovaArp::find_pattern(const QString& name) const {
  const auto library = arp::factory_patterns();
  for (std::size_t i = 0; i < library.size(); ++i) {
    if (text(library[i].name) == name)
      return static_cast<int>(i);
  }
  return -1;
}

bool NovaArp::mutate(int seed) {
  if (seed < 0) {
    set_message(QStringLiteral("Seeds start at 0"));
    return false;
  }
  // Start from the sound now unless it is this model's own last mutation.
  if (mutation_ == 0 || base_.size() != controller_.current_values().size() ||
      controller_.revision() != written_revision_)
    base_ = controller_.current_values();
  auto values =
      arp::mutate(controller_.table(), base_, static_cast<std::uint32_t>(seed), amount_, locks_);
  if (!commit(std::move(values), nullptr, nullptr))
    return false;
  mutation_ = static_cast<std::uint32_t>(seed);
  emit mutation_changed();
  return true;
}

bool NovaArp::set_lock(const QString& lane, bool locked) {
  const auto l = lane_index(lane);
  if (!l)
    return false;
  if (locks_[*l] == locked)
    return true;
  locks_[*l] = locked;
  emit locks_changed();
  return true;
}

bool NovaArp::is_locked(const QString& lane) const {
  const auto l = lane_index(lane);
  return l && locks_[*l];
}

bool NovaArp::set_groove(const QVariantMap& groove) {
  persistence::GrooveTemplate source;
  source.name = groove.value(QStringLiteral("name")).toString().toStdString();
  source.steps_per_quarter =
      static_cast<std::uint32_t>(groove.value(QStringLiteral("stepsPerQuarter")).toInt());
  const auto timings = groove.value(QStringLiteral("timings")).toList();
  const auto velocities = groove.value(QStringLiteral("velocities")).toList();
  if (timings.size() != velocities.size()) {
    set_message(QStringLiteral("That groove's timings and velocities differ in length"));
    return false;
  }
  for (qsizetype i = 0; i < timings.size(); ++i)
    source.steps.push_back({timings[i].toDouble(), velocities[i].toDouble()});
  auto values = arp::apply_groove(controller_.table(), controller_.current_values(), source);
  if (!values) {
    set_message(QString::fromStdString(values.error().message));
    return false;
  }
  const QString name = QString::fromStdString(source.name);
  return commit(std::move(values.value()), nullptr, &name);
}

bool NovaArp::clear_groove() {
  const QString none;
  return commit(arp::clear_groove(controller_.table(), controller_.current_values()), nullptr,
                &none);
}

double NovaArp::period() const {
  return static_cast<double>(
      nova::arp_period(arp::settings_of(controller_.table(), controller_.current_values())));
}

double NovaArp::period_beats() const {
  const auto settings = arp::settings_of(controller_.table(), controller_.current_values());
  const auto rate = std::min(settings.rate, nova::arp_rate_beats.size() - 1);
  return static_cast<double>(nova::arp_period(settings)) * nova::arp_rate_beats[rate];
}

QVariantList NovaArp::euclid_steps() const {
  const auto settings = arp::settings_of(controller_.table(), controller_.current_values());
  const auto steps = std::clamp<std::uint32_t>(settings.euclid_steps, 1,
                                               static_cast<std::uint32_t>(nova::arp_max_steps));
  QVariantList list;
  for (std::uint32_t i = 0; i < steps; ++i)
    list.append(nova::euclid_hit(i, settings.euclid_hits, steps, settings.euclid_rotate));
  return list;
}

bool NovaArp::commit(std::vector<double> values, const QString* pattern, const QString* groove) {
  if (!controller_.editable()) {
    set_message(QStringLiteral("The synth cannot be edited now"));
    return false;
  }
  auto document = controller_.current_document();
  const auto label = [&](std::string_view id, const QString& name) {
    const std::string key(id);
    std::erase_if(document.labels, [&](const project::NativeLabel& l) { return l.id == key; });
    const auto clean =
        name.trimmed().left(static_cast<qsizetype>(project::max_native_label_length));
    if (!clean.isEmpty())
      document.labels.push_back({key, clean.toStdString()});
  };
  // A load names the pattern and a groove names the groove; MUTATE keeps
  // the name (a variation of that pattern).
  if (pattern != nullptr)
    label(pattern_label, *pattern);
  if (groove != nullptr)
    label(groove_label, *groove);
  controller_.apply_state(values, std::move(document));
  written_revision_ = controller_.revision();
  set_message({});
  return true;
}

void NovaArp::set_message(const QString& message) {
  if (message_ == message)
    return;
  message_ = message;
  emit message_changed();
}

} // namespace oscillate::quick
