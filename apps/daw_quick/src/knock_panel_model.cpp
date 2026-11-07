#include "knock_panel_model.hpp"
#include "knock_patterns_model.hpp"

#include "oscillate/foundation/value_text.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"

#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace oscillate::quick {
namespace {

namespace synth = native_dsp::synth;
namespace knock = native_dsp::synth::knock;

// The controls a pad is named by: its resonator's material and its striker.
constexpr std::string_view material_control = "res.material";
constexpr std::string_view striker_control = "striker.type";
constexpr std::string_view label_separator = " · ";

// Knock's keys: C1 is MIDI 36 (knock.hpp), so the octave is key / 12 - 2.
constexpr int semitones_per_octave = 12;
constexpr int octave_of_key_zero = -2;
constexpr std::array<std::string_view, semitones_per_octave> note_names{
    "C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

const synth::EngineDescriptor& table() { return *knock::engine().descriptor; }

// The chosen label of choice parameter `id`, or nothing.
std::optional<std::string_view> choice_label(const synth::EngineDescriptor& engine,
                                             std::span<const double> values, std::string_view id) {
  const auto index = synth::find_parameter(engine, id);
  if (!index || *index >= values.size())
    return std::nullopt;
  const auto& choices = engine.parameters[*index].choices;
  const double value = values[*index];
  if (!std::isfinite(value) || value < 0.0)
    return std::nullopt;
  const auto choice = static_cast<std::size_t>(std::lround(value));
  if (choice >= choices.size())
    return std::nullopt;
  return choices[choice];
}

QString part_name(knock::ModePart part) {
  switch (part) {
  case knock::ModePart::resonator:
    return QStringLiteral("resonator");
  case knock::ModePart::coupled:
    return QStringLiteral("coupled");
  case knock::ModePart::body:
    return QStringLiteral("body");
  }
  return QStringLiteral("resonator");
}

} // namespace

std::string knock_pad_parameter(std::size_t pad, std::string_view control) {
  std::string id(table().id);
  id += ".pad";
  id += std::to_string(pad + 1);
  id += '.';
  id += control;
  return id;
}

std::string knock_pad_label(const synth::EngineDescriptor& engine, std::span<const double> values,
                            std::size_t pad) {
  if (pad >= knock::pad_count || values.size() != engine.parameters.size())
    return {};
  const auto material = choice_label(engine, values, knock_pad_parameter(pad, material_control));
  const auto striker = choice_label(engine, values, knock_pad_parameter(pad, striker_control));
  if (!material || !striker)
    return {};
  std::string label(*material);
  label += label_separator;
  label += *striker;
  return label;
}

QString knock_key_name(int key) {
  if (key < 0)
    return {};
  // A whole octave number; the formatter gives a negative one its minus sign.
  const int octave = key / semitones_per_octave + octave_of_key_zero;
  return text(note_names[static_cast<std::size_t>(key % semitones_per_octave)]) +
         QString::fromStdString(foundation::format_number(octave, 0));
}

KnockPanelModel::KnockPanelModel(NativeSynthDeviceController& controller, std::uint32_t sample_rate,
                                 NativeSynthPanelAdapter* adapter, QObject* parent)
    : QObject(parent), controller_(controller), sample_rate_(sample_rate),
      patterns_(new KnockPatternsModel(controller, adapter, this)) {
  connect(&controller_, &NativeSynthDeviceController::values_changed, this,
          &KnockPanelModel::update);
  update();
}

QObject* KnockPanelModel::patterns() const { return patterns_; }

int KnockPanelModel::pad_count() noexcept { return static_cast<int>(knock::pad_count); }

int KnockPanelModel::first_key() noexcept { return static_cast<int>(knock::first_key); }

QStringList KnockPanelModel::key_names() const {
  QStringList names;
  for (int pad = 0; pad < pad_count(); ++pad)
    names.append(knock_key_name(first_key() + pad));
  return names;
}

void KnockPanelModel::update() {
  QStringList labels;
  const auto& values = controller_.current_values();
  for (std::size_t pad = 0; pad < knock::pad_count; ++pad)
    labels.append(QString::fromStdString(knock_pad_label(table(), values, pad)));
  labels_ = labels;
  ++revision_;
  emit changed();
}

QString KnockPanelModel::pad_parameter(int pad, const QString& control) const {
  if (pad < 0 || pad >= pad_count())
    return {};
  const auto id = knock_pad_parameter(static_cast<std::size_t>(pad), control.toStdString());
  return synth::find_parameter(table(), id) ? QString::fromStdString(id) : QString{};
}

QVariantList KnockPanelModel::modes(int pad) const {
  QVariantList result;
  const auto& values = controller_.current_values();
  if (pad < 0 || pad >= pad_count() || values.size() != table().parameters.size())
    return result;
  const auto designed = knock::design_pad(std::span<const double>(values),
                                          static_cast<std::size_t>(pad), sample_rate_);
  result.reserve(static_cast<qsizetype>(designed.size()));
  for (const auto& mode : designed) {
    result.push_back(QVariantMap{{QStringLiteral("hz"), mode.hz},
                                 {QStringLiteral("level"), mode.level},
                                 {QStringLiteral("t60"), mode.t60},
                                 {QStringLiteral("part"), part_name(mode.part)}});
  }
  return result;
}

double KnockPanelModel::view_fraction(double hz) const {
  if (!std::isfinite(hz) || hz <= view_min_hz_value)
    return 0.0;
  return std::clamp(
      std::log(hz / view_min_hz_value) / std::log(view_max_hz_value / view_min_hz_value), 0.0, 1.0);
}

} // namespace oscillate::quick
