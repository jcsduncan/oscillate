#include "native_synth_device_controller.hpp"
#include "crate_controller.hpp"
#include "device_panels.hpp"
#include "native_effect_visuals.hpp"
#include "native_synth_document_model.hpp"
#include "nova_arp.hpp"
#include "nova_constellation.hpp"
#include "stick_panel_data.hpp"
#include "ui_contracts.hpp"

#include "oscillate/foundation/trace.hpp"
#include "oscillate/foundation/value_text.hpp"
#include "oscillate/native_dsp/effects/cast.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/chop.hpp"
#include "oscillate/native_dsp/effects/contour.hpp"
#include "oscillate/native_dsp/effects/dust.hpp"
#include "oscillate/native_dsp/effects/edge.hpp"
#include "oscillate/native_dsp/effects/flyby.hpp"
#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/native_dsp/effects/hush.hpp"
#include "oscillate/native_dsp/effects/keel.hpp"
#include "oscillate/native_dsp/effects/orbit.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/pump.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/effects/strata.hpp"
#include "oscillate/native_dsp/effects/strike.hpp"
#include "oscillate/native_dsp/effects/temper.hpp"
#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::quick {
namespace {

using native_dsp::ParameterDescriptor;
using native_dsp::ParameterScale;
using native_dsp::ParameterTiming;

constexpr double min_hz = 20.0;
constexpr double max_hz = 20'000.0;
// Strata's sketches draw at the panel's nominal rate.
constexpr double strata_sketch_rate = 48'000.0;

// A sample library's state (Concert's or Fret's: both name missing,
// invalid and ready) as QML reads it.
template <typename State> QString library_state_name(State state) {
  switch (state) {
  case State::ready:
    return QStringLiteral("ready");
  case State::invalid:
    return QStringLiteral("invalid");
  case State::missing:
    break;
  }
  return QStringLiteral("missing");
}

// Bow's library can also be loading (its first look reads in the
// background).
QString library_state_name(native_dsp::synth::bow::library::State state) {
  using State = native_dsp::synth::bow::library::State;
  switch (state) {
  case State::ready:
    return QStringLiteral("ready");
  case State::invalid:
    return QStringLiteral("invalid");
  case State::loading:
    return QStringLiteral("loading");
  case State::missing:
    break;
  }
  return QStringLiteral("missing");
}

QString concert_state_name(native_dsp::synth::concert::library::State state) {
  return library_state_name(state);
}

QString text(std::string_view view);

constexpr double bytes_per_mb = 1024.0 * 1024.0;

// What an engine's own sample library needs said about it.
struct LibraryFacts {
  std::string_view install_command;
  std::string_view name;
  std::string_view attribution;
  int first_key = 0;
  int last_key = 0;
};

// Fret's or Wren's library status as QML reads it: both carry the same
// fields.
template <typename Status>
QVariantMap sample_library_map(const Status& status, const LibraryFacts& facts) {
  return {
      {QStringLiteral("state"), library_state_name(status.state)},
      {QStringLiteral("message"), QString::fromStdString(status.message)},
      {QStringLiteral("directory"), QString::fromStdString(status.directory.string())},
      {QStringLiteral("name"), QString::fromStdString(status.name)},
      {QStringLiteral("attribution"), QString::fromStdString(status.attribution)},
      {QStringLiteral("install_command"), text(facts.install_command)},
      {QStringLiteral("required"),
       text(facts.name) + QStringLiteral(" · ") + text(facts.attribution)},
      {QStringLiteral("resident_mb"), static_cast<double>(status.resident_bytes) / bytes_per_mb},
      {QStringLiteral("samples"), static_cast<int>(status.samples)},
      {QStringLiteral("first_key"), facts.first_key},
      {QStringLiteral("last_key"), facts.last_key}};
}

QVariantMap fret_library_map(const native_dsp::synth::fret::library::Status& status) {
  namespace fret = native_dsp::synth::fret;
  return sample_library_map(status, {fret::library::install_command, fret::library::name,
                                     fret::library::attribution, fret::first_key, fret::last_key});
}

QVariantMap bow_library_map(const native_dsp::synth::bow::library::Status& status) {
  namespace bow = native_dsp::synth::bow;
  return sample_library_map(status, {bow::library::install_command, bow::library::name,
                                     bow::library::attribution, bow::first_key, bow::last_key});
}

QVariantMap wren_library_map(const native_dsp::synth::wren::library::Status& status) {
  namespace wren = native_dsp::synth::wren;
  return sample_library_map(status, {wren::library::install_command, wren::library::name,
                                     wren::library::attribution, wren::first_key, wren::last_key});
}

// How a Wren note starts, as QML reads it.
QString tongue_name(native_dsp::synth::wren::breath::Tongue tongue) {
  using T = native_dsp::synth::wren::breath::Tongue;
  switch (tongue) {
  case T::k:
    return QStringLiteral("k");
  case T::slur:
    return QStringLiteral("slur");
  case T::t:
    break;
  }
  return QStringLiteral("t");
}

QString articulation_name(native_dsp::synth::fret::feel::Articulation articulation) {
  using A = native_dsp::synth::fret::feel::Articulation;
  switch (articulation) {
  case A::staccato:
    return QStringLiteral("staccato");
  case A::ghost:
    return QStringLiteral("ghost");
  case A::bridge:
    return QStringLiteral("bridge");
  case A::pluck:
    break;
  }
  return QStringLiteral("pluck");
}

// Key names with C-1 at MIDI 0, so Fret's lowest key, 23, is B0.
constexpr std::array<std::string_view, 12> pitch_names{"C",  "C♯", "D",  "D♯", "E",  "F",
                                                       "F♯", "G",  "G♯", "A",  "A♯", "B"};
constexpr int semitones_per_octave = static_cast<int>(pitch_names.size());
constexpr int octave_of_key_zero = -1;

// "B0" for 23; empty for a key below 0 (none yet).
QString key_name(int key) {
  if (key < 0) {
    return {};
  }
  // A whole octave number; the formatter gives a negative one its minus sign.
  const int octave = key / semitones_per_octave + octave_of_key_zero;
  return text(pitch_names[static_cast<std::size_t>(key % semitones_per_octave)]) +
         QString::fromStdString(foundation::format_number(octave, 0));
}

QVariantMap concert_library_map(const native_dsp::synth::concert::library::Status& status) {
  namespace concert = native_dsp::synth::concert;
  QVariantList ring;
  for (const double seconds : status.ring_seconds) {
    ring.push_back(seconds);
  }
  return {
      {QStringLiteral("state"), concert_state_name(status.state)},
      {QStringLiteral("message"), QString::fromStdString(status.message)},
      {QStringLiteral("directory"), QString::fromStdString(status.directory.string())},
      {QStringLiteral("name"), QString::fromStdString(status.name)},
      {QStringLiteral("attribution"), QString::fromStdString(status.attribution)},
      {QStringLiteral("install_command"), text(concert::library::install_command)},
      {QStringLiteral("required"),
       text(concert::library::name) + QStringLiteral(" · ") + text(concert::library::attribution)},
      {QStringLiteral("resident_mb"), static_cast<double>(status.resident_bytes) / bytes_per_mb},
      {QStringLiteral("ring_seconds"), ring},
      {QStringLiteral("first_key"), concert::first_key},
      {QStringLiteral("key_count"), static_cast<int>(concert::key_count)},
      {QStringLiteral("last_damped_key"), status.last_damped_key}};
}

QVariantList list(const std::vector<double>& points) {
  QVariantList result;
  result.reserve(static_cast<qsizetype>(points.size()));
  for (const double point : points) {
    result.append(point);
  }
  return result;
}

QString text(std::string_view view) {
  return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

QString scale_name(ParameterScale scale) {
  switch (scale) {
  case ParameterScale::linear:
    return QStringLiteral("linear");
  case ParameterScale::logarithmic:
    return QStringLiteral("log");
  case ParameterScale::stepped:
    return QStringLiteral("stepped");
  case ParameterScale::choice:
    return QStringLiteral("choice");
  }
  return QStringLiteral("linear");
}

} // namespace

NativeSynthDeviceController::NativeSynthDeviceController(const native_dsp::EngineDescriptor& engine,
                                                         NativeSynthPanelAdapter& adapter,
                                                         QObject* parent)
    : QObject(parent), adapter_(adapter), engine_(engine), visuals_(visuals_for(engine.id)) {
  register_ui_contracts();
  index_.reserve(engine_.parameters.size());
  for (std::size_t i = 0; i < engine_.parameters.size(); ++i) {
    index_.emplace(engine_.parameters[i].id, i);
  }
  document_ = new NativeSynthDocumentModel(adapter_, *this, this);
  if (engine_.id == native_dsp::synth::crate::engine_id)
    crate_ = new CrateController(engine_, adapter_, [this] { refresh(); }, this);
  if (engine_.id == native_dsp::synth::nova::engine().descriptor->id) {
    constellation_ = new NovaConstellation(*this, NovaConstellation::default_cache_file(), this);
    arp_ = new NovaArp(*this, this);
  }
  for (const auto& preset : engine_.presets) {
    auto values = native_dsp::preset_values(engine_, preset);
    preset_values_.push_back(values ? values.value() : native_dsp::default_values(engine_));
  }
  load_favourites();
  refresh();
}

void NativeSynthDeviceController::refresh() {
  OSCILLATE_TRACE_SPAN("ui.device_refresh");
  auto values = adapter_.values();
  if (values.size() != engine_.parameters.size()) {
    values = native_dsp::default_values(engine_);
  }
  const bool changed = values != values_;
  values_ = std::move(values);
  const auto status = adapter_.status();
  const bool status_differs = status.saved_revision != status_.saved_revision ||
                              status.audible_revision != status_.audible_revision ||
                              status.persistence_pending != status_.persistence_pending ||
                              status.deferred_changes != status_.deferred_changes ||
                              status.editable != status_.editable ||
                              status.message != status_.message;
  status_ = status;
  if (auto bindings = adapter_.macro_bindings(); bindings != macro_bindings_) {
    macro_bindings_ = std::move(bindings);
    ++macro_revision_;
    emit macros_changed();
  }
  if (changed) {
    // A change from outside the panel (undo, another track) may be any patch.
    preset_index_ = -1;
    if (values_ != user_preset_values_)
      user_preset_.clear();
    identify_preset();
    ++revision_;
    emit values_changed();
  }
  if (status_differs) {
    emit status_changed();
  }
  if (const auto linked = static_cast<int>(adapter_.linked_count()); linked != linked_count_) {
    linked_count_ = linked;
    emit link_changed();
  }
  document_->refresh();
  if (crate_ != nullptr)
    crate_->refresh();
}

QObject* NativeSynthDeviceController::document() const { return document_; }

QObject* NativeSynthDeviceController::crate() const { return crate_; }

QObject* NativeSynthDeviceController::constellation() const { return constellation_; }

QObject* NativeSynthDeviceController::arp() const { return arp_; }

std::optional<std::size_t> NativeSynthDeviceController::index_of(const QString& id) const {
  const QByteArray utf8 = id.toUtf8();
  const auto found =
      index_.find(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
  return found == index_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

QVariantList NativeSynthDeviceController::presets() const {
  QVariantList list;
  for (std::size_t i = 0; i < engine_.presets.size(); ++i) {
    QStringList tags;
    for (const auto tag : engine_.presets[i].tags)
      tags.append(text(tag));
    list.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                            {QStringLiteral("name"), text(engine_.presets[i].name)},
                            {QStringLiteral("category"), text(engine_.presets[i].category)},
                            {QStringLiteral("tags"), tags}});
  }
  return list;
}

QStringList NativeSynthDeviceController::preset_tags() const {
  QStringList tags;
  for (const auto& preset : engine_.presets) {
    for (const auto tag : preset.tags) {
      if (const QString t = text(tag); !tags.contains(t))
        tags.append(t);
    }
  }
  return tags;
}

QStringList NativeSynthDeviceController::preset_categories() const {
  QStringList categories;
  for (const auto& preset : engine_.presets) {
    const QString category = text(preset.category);
    if (!categories.contains(category)) {
      categories.append(category);
    }
  }
  return categories;
}

QString NativeSynthDeviceController::preset_name() const {
  if (!user_preset_.isEmpty())
    return user_preset_;
  return preset_index_ < 0 ? QStringLiteral("Custom")
                           : text(engine_.presets[static_cast<std::size_t>(preset_index_)].name);
}

QString NativeSynthDeviceController::preset_category() const {
  if (!user_preset_.isEmpty())
    return QStringLiteral("User");
  return preset_index_ < 0
             ? QStringLiteral("Patch")
             : text(engine_.presets[static_cast<std::size_t>(preset_index_)].category);
}

bool NativeSynthDeviceController::preset_modified() const {
  if (!user_preset_.isEmpty())
    return user_preset_values_ != values_;
  return preset_index_ >= 0 && preset_values_[static_cast<std::size_t>(preset_index_)] != values_;
}

QString NativeSynthDeviceController::status_text() const {
  if (!status_.editable) {
    return QStringLiteral("Read only");
  }
  if (!status_.message.empty()) {
    return QStringLiteral("Not applied");
  }
  if (status_.persistence_pending)
    return QStringLiteral("Unsaved");
  if (status_.deferred_changes) {
    return QStringLiteral("Applies to new notes");
  }
  if (pending()) {
    return QStringLiteral("Updating");
  }
  return QStringLiteral("Live");
}

QString NativeSynthDeviceController::status_detail() const {
  if (!status_.message.empty()) {
    return QString::fromStdString(status_.message);
  }
  if (status_.persistence_pending)
    return QStringLiteral("Patch is active in the project and awaits a durable save. Check project "
                          "status if saving fails.");
  if (status_.deferred_changes) {
    return QStringLiteral("Notes already sounding keep their voice settings; the next notes "
                          "you play use the new ones.");
  }
  if (pending()) {
    return QStringLiteral("Saved; the running synth picks it up in a moment.");
  }
  return QStringLiteral("What you hear matches the saved patch.");
}

QVariantMap NativeSynthDeviceController::parameter(const QString& id) const {
  const auto index = index_of(id);
  if (!index) {
    return {};
  }
  const ParameterDescriptor& p = engine_.parameters[*index];
  QStringList choices;
  for (const auto choice : p.choices) {
    choices.append(text(choice));
  }
  return {{QStringLiteral("id"), id},
          {QStringLiteral("name"), text(p.name)},
          {QStringLiteral("group"), text(p.group)},
          {QStringLiteral("unit"), text(p.unit)},
          {QStringLiteral("scale"), scale_name(p.scale)},
          {QStringLiteral("minimum"), p.minimum},
          {QStringLiteral("maximum"), p.maximum},
          {QStringLiteral("default"), p.default_value},
          {QStringLiteral("default_normalised"), native_dsp::to_normalised(p, p.default_value)},
          {QStringLiteral("choices"), choices},
          {QStringLiteral("discrete"), native_dsp::is_discrete(p)},
          {QStringLiteral("steps"),
           native_dsp::is_discrete(p) ? static_cast<int>(p.maximum - p.minimum) : 0},
          {QStringLiteral("bipolar"), p.minimum < 0.0 && p.maximum > 0.0},
          {QStringLiteral("next_note"), p.timing == ParameterTiming::next_note},
          {QStringLiteral("automatable"), p.automatable}};
}

QStringList NativeSynthDeviceController::parameter_ids(const QString& group) const {
  QStringList ids;
  for (const auto& p : engine_.parameters) {
    if (text(p.group) == group) {
      ids.append(text(p.id));
    }
  }
  return ids;
}

QStringList NativeSynthDeviceController::modulatable_ids() const {
  QStringList ids;
  for (const auto& p : engine_.parameters) {
    if (p.automatable && !native_dsp::is_discrete(p) && p.timing == ParameterTiming::live) {
      ids.append(text(p.id));
    }
  }
  return ids;
}

double NativeSynthDeviceController::normalised(const QString& id) const {
  const auto index = index_of(id);
  return index ? native_dsp::to_normalised(engine_.parameters[*index], values_[*index]) : 0.0;
}

double NativeSynthDeviceController::value(const QString& id) const {
  const auto index = index_of(id);
  return index ? values_[*index] : 0.0;
}

int NativeSynthDeviceController::choice(const QString& id) const {
  const auto index = index_of(id);
  return index ? static_cast<int>(values_[*index] - engine_.parameters[*index].minimum) : 0;
}

QString NativeSynthDeviceController::display(const QString& id) const {
  const auto index = index_of(id);
  return index ? QString::fromStdString(
                     native_dsp::format_value(engine_.parameters[*index], values_[*index]))
               : QString();
}

QString NativeSynthDeviceController::display_at(const QString& id, double normalised) const {
  const auto index = index_of(id);
  if (!index) {
    return {};
  }
  const auto& p = engine_.parameters[*index];
  return QString::fromStdString(
      native_dsp::format_value(p, native_dsp::from_normalised(p, normalised)));
}

bool NativeSynthDeviceController::is_default(const QString& id) const {
  const auto index = index_of(id);
  return index && values_[*index] == engine_.parameters[*index].default_value;
}

void NativeSynthDeviceController::write(std::size_t index, double value) {
  const double clean = native_dsp::sanitise(engine_.parameters[index], value);
  if (clean == values_[index] || !status_.editable) {
    return;
  }
  values_[index] = clean;
  submit(gesture_parameter_.isEmpty() ? NativeSynthGesture::single : NativeSynthGesture::update);
}

void NativeSynthDeviceController::submit(NativeSynthGesture gesture) {
  OSCILLATE_TRACE_SPAN("ui.device_submit");
  adapter_.submit(values_, gesture);
  // Adopt whatever the project accepted, which may differ from the request.
  refresh();
  if (gesture != NativeSynthGesture::begin) {
    identify_preset();
    ++revision_;
    emit values_changed();
  }
}

void NativeSynthDeviceController::set_normalised(const QString& id, double normalised) {
  if (const auto index = index_of(id)) {
    write(*index, native_dsp::from_normalised(engine_.parameters[*index], normalised));
  }
}

void NativeSynthDeviceController::set_value(const QString& id, double value) {
  if (const auto index = index_of(id)) {
    write(*index, value);
  }
}

void NativeSynthDeviceController::set_choice(const QString& id, int index_in_choices) {
  if (const auto index = index_of(id)) {
    write(*index, engine_.parameters[*index].minimum + index_in_choices);
  }
}

void NativeSynthDeviceController::set_values(const QVariantMap& values) {
  if (!status_.editable) {
    return;
  }
  bool changed = false;
  for (auto entry = values.constBegin(); entry != values.constEnd(); ++entry) {
    const auto index = index_of(entry.key());
    if (!index) {
      continue;
    }
    const double clean = native_dsp::sanitise(engine_.parameters[*index], entry.value().toDouble());
    changed = changed || clean != values_[*index];
    values_[*index] = clean;
  }
  if (changed) {
    submit(gesture_parameter_.isEmpty() ? NativeSynthGesture::single : NativeSynthGesture::update);
  }
}

void NativeSynthDeviceController::step(const QString& id, int direction, bool fine) {
  const auto index = index_of(id);
  if (!index || direction == 0) {
    return;
  }
  const auto& p = engine_.parameters[*index];
  const double sign = direction > 0 ? 1.0 : -1.0;
  if (native_dsp::is_discrete(p)) {
    write(*index, values_[*index] + sign);
    return;
  }
  const double position = native_dsp::to_normalised(p, values_[*index]);
  write(*index, native_dsp::from_normalised(p, position + sign * (fine ? 0.001 : 0.01)));
}

void NativeSynthDeviceController::reset_to_default(const QString& id) {
  if (const auto index = index_of(id)) {
    write(*index, engine_.parameters[*index].default_value);
  }
}

void NativeSynthDeviceController::begin_gesture(const QString& id) {
  if (!gesture_parameter_.isEmpty() || !index_of(id) || !status_.editable) {
    return;
  }
  gesture_parameter_ = id;
  emit gesture_changed();
  submit(NativeSynthGesture::begin);
}

void NativeSynthDeviceController::end_gesture(const QString& id) {
  if (gesture_parameter_ != id) {
    return;
  }
  gesture_parameter_.clear();
  emit gesture_changed();
  submit(NativeSynthGesture::commit);
}

void NativeSynthDeviceController::cancel_gesture(const QString& id) {
  if (gesture_parameter_ != id) {
    return;
  }
  gesture_parameter_.clear();
  emit gesture_changed();
  submit(NativeSynthGesture::cancel);
}

void NativeSynthDeviceController::begin_state_gesture() {
  if (state_gesture_ || !gesture_parameter_.isEmpty() || !status_.editable)
    return;
  state_gesture_ = true;
  submit(NativeSynthGesture::begin);
}

void NativeSynthDeviceController::update_state(std::span<const double> values,
                                               const project::NativeSynthDocument* document) {
  if (values.size() != values_.size() || !status_.editable)
    return;
  if (!state_gesture_) {
    apply_state(values, document != nullptr ? *document : adapter_.document());
    return;
  }
  // The document is an edit under the drag's gesture, with these values so
  // the project never holds the new document with the old sound; the values
  // then go the live path. A refresh the document edit causes may read the
  // values from before, so this keeps its own copy.
  const std::vector<double> wanted(values.begin(), values.end());
  if (document != nullptr)
    adapter_.submit_state(wanted, *document, NativeSynthGesture::update);
  values_ = wanted;
  submit(NativeSynthGesture::update);
}

void NativeSynthDeviceController::end_state_gesture() {
  if (!state_gesture_)
    return;
  state_gesture_ = false;
  submit(NativeSynthGesture::commit);
}

void NativeSynthDeviceController::apply_state(std::span<const double> values,
                                              project::NativeSynthDocument document) {
  if (values.size() != values_.size() || !status_.editable)
    return;
  preset_index_ = -1;
  user_preset_.clear();
  adapter_.submit_state(values, std::move(document), NativeSynthGesture::single);
  refresh();
  identify_preset();
  ++revision_;
  emit values_changed();
}

void NativeSynthDeviceController::apply_preset(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= preset_values_.size() || !status_.editable) {
    return;
  }
  preset_index_ = index;
  user_preset_.clear();
  if (adapter_.submit_preset(static_cast<std::size_t>(index))) {
    refresh();
    preset_index_ = index;
    return;
  }
  values_ = preset_values_[static_cast<std::size_t>(index)];
  submit(NativeSynthGesture::single);
}

void NativeSynthDeviceController::step_preset(int direction) {
  const int count = static_cast<int>(preset_values_.size());
  if (count == 0) {
    return;
  }
  const int from = preset_index_ < 0 ? (direction > 0 ? -1 : 0) : preset_index_;
  apply_preset(((from + (direction > 0 ? 1 : -1)) % count + count) % count);
}

namespace {

// Preset files are small; anything larger is not one.
constexpr qint64 max_preset_bytes = qint64{4} * 1024 * 1024;

// A preset name is a file name: printable, no path, not hidden.
bool usable_preset_name(const QString& name) {
  if (name.isEmpty() || name.size() > 64 || name.startsWith(QLatin1Char('.')))
    return false;
  for (const QChar c : name) {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\') || !c.isPrint())
      return false;
  }
  return true;
}

QString local_path(const QString& path) {
  const QUrl url(path);
  return url.isLocalFile() ? url.toLocalFile() : path;
}

std::optional<QByteArray> read_preset(const QString& path) {
  QFile file(path);
  if (file.size() > max_preset_bytes || !file.open(QIODevice::ReadOnly))
    return std::nullopt;
  return file.readAll();
}

bool write_preset(const QString& path, const std::string& text) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(text.data(), static_cast<qint64>(text.size()));
  return file.commit();
}

} // namespace

QString NativeSynthDeviceController::user_preset_directory() const {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty())
    data = QDir::homePath() + QStringLiteral("/.local/share");
  return data + QStringLiteral("/oscillate/presets/") + engine_id();
}

QString NativeSynthDeviceController::user_preset_file(const QString& name) const {
  return user_preset_directory() + QLatin1Char('/') + name + QStringLiteral(".json");
}

QVariantList NativeSynthDeviceController::user_presets() const {
  QDir directory(user_preset_directory());
  QStringList names;
  for (const auto& file :
       directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable)) {
    if (usable_preset_name(file.completeBaseName()))
      names.append(file.completeBaseName());
  }
  names.sort(Qt::CaseInsensitive);
  QVariantList list;
  for (const auto& name : names)
    list.append(QVariantMap{{QStringLiteral("name"), name}});
  return list;
}

bool NativeSynthDeviceController::save_user_preset(const QString& name) {
  const auto trimmed = name.trimmed();
  const auto text = adapter_.preset_text();
  if (!usable_preset_name(trimmed) || !text || !QDir().mkpath(user_preset_directory()) ||
      !write_preset(user_preset_file(trimmed), *text))
    return false;
  user_preset_ = trimmed;
  user_preset_values_ = values_;
  ++revision_;
  emit values_changed();
  emit user_presets_changed();
  return true;
}

bool NativeSynthDeviceController::load_preset_text(const QByteArray& text, const QString& name) {
  if (!status_.editable)
    return false;
  const bool loaded = adapter_.submit_preset_text(
      std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
  refresh();
  if (!loaded)
    return false;
  preset_index_ = -1;
  user_preset_ = name;
  user_preset_values_ = values_;
  ++revision_;
  emit values_changed();
  return true;
}

std::optional<QByteArray> NativeSynthDeviceController::user_preset_text(const QString& name) const {
  if (!usable_preset_name(name))
    return std::nullopt;
  return read_preset(user_preset_file(name));
}

bool NativeSynthDeviceController::apply_user_preset(const QString& name) {
  if (!usable_preset_name(name))
    return false;
  const auto text = read_preset(user_preset_file(name));
  return text && load_preset_text(*text, name);
}

bool NativeSynthDeviceController::delete_user_preset(const QString& name) {
  if (!usable_preset_name(name) || !QFile::remove(user_preset_file(name)))
    return false;
  if (user_preset_ == name) {
    user_preset_.clear();
    ++revision_;
    emit values_changed();
  }
  if (favourites_.contains(preset_key(true, name)))
    (void)set_favourite(preset_key(true, name), false);
  emit user_presets_changed();
  return true;
}

// The two names are named at every call (from QML).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool NativeSynthDeviceController::rename_user_preset(const QString& from, const QString& to) {
  const auto target = to.trimmed();
  if (!usable_preset_name(from) || !usable_preset_name(target) || target == from ||
      !QFile::exists(user_preset_file(from)) || QFile::exists(user_preset_file(target)) ||
      !QFile::rename(user_preset_file(from), user_preset_file(target)))
    return false;
  if (user_preset_ == from) {
    user_preset_ = target;
    ++revision_;
    emit values_changed();
  }
  if (const auto old_key = preset_key(true, from); favourites_.contains(old_key)) {
    favourites_.replace(favourites_.indexOf(old_key), preset_key(true, target));
    (void)save_favourites();
    emit favourites_changed();
  }
  emit user_presets_changed();
  return true;
}

// ---- Nova FX module presets ------------------------------------------------------

QString NativeSynthDeviceController::fx_preset_directory(const QString& module) const {
  return user_preset_directory() + QStringLiteral("/fx/") + module;
}

QStringList NativeSynthDeviceController::fx_presets(const QString& module) const {
  QStringList names;
  if (!usable_preset_name(module))
    return names;
  QDir directory(fx_preset_directory(module));
  for (const auto& file :
       directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable)) {
    if (usable_preset_name(file.completeBaseName()))
      names.append(file.completeBaseName());
  }
  names.sort(Qt::CaseInsensitive);
  return names;
}

// The module and the name are named at every call (from QML).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool NativeSynthDeviceController::save_fx_preset(const QString& module, const QString& name,
                                                 const QVariantMap& values) {
  const QString trimmed = name.trimmed();
  if (!usable_preset_name(module) || !usable_preset_name(trimmed) || values.isEmpty() ||
      !QDir().mkpath(fx_preset_directory(module)))
    return false;
  const QJsonObject preset{{QStringLiteral("module"), module},
                           {QStringLiteral("values"), QJsonObject::fromVariantMap(values)}};
  const QByteArray text = QJsonDocument(preset).toJson(QJsonDocument::Indented);
  if (!write_preset(fx_preset_directory(module) + QLatin1Char('/') + trimmed +
                        QStringLiteral(".json"),
                    text.toStdString()))
    return false;
  ++fx_presets_revision_;
  emit fx_presets_changed();
  return true;
}

QVariantMap NativeSynthDeviceController::fx_preset(const QString& module,
                                                   const QString& name) const {
  if (!usable_preset_name(module) || !usable_preset_name(name))
    return {};
  const auto text =
      read_preset(fx_preset_directory(module) + QLatin1Char('/') + name + QStringLiteral(".json"));
  if (!text)
    return {};
  const QJsonObject preset = QJsonDocument::fromJson(*text).object();
  if (preset.value(QStringLiteral("module")).toString() != module)
    return {};
  return preset.value(QStringLiteral("values")).toObject().toVariantMap();
}

bool NativeSynthDeviceController::delete_fx_preset(const QString& module, const QString& name) {
  if (!usable_preset_name(module) || !usable_preset_name(name) ||
      !QFile::remove(fx_preset_directory(module) + QLatin1Char('/') + name +
                     QStringLiteral(".json")))
    return false;
  ++fx_presets_revision_;
  emit fx_presets_changed();
  return true;
}

QString NativeSynthDeviceController::preset_key(bool user, const QString& name) const {
  return (user ? QStringLiteral("user/") : QStringLiteral("factory/")) + name;
}

QString NativeSynthDeviceController::favourites_file() const {
  // Not a .json file, so it is never listed as a user preset.
  return user_preset_directory() + QStringLiteral("/favourites.txt");
}

void NativeSynthDeviceController::load_favourites() {
  favourites_.clear();
  const auto text = read_preset(favourites_file());
  if (!text)
    return;
  for (const auto& line : QString::fromUtf8(*text).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const auto key = line.trimmed();
    if (!key.isEmpty() && !favourites_.contains(key))
      favourites_.append(key);
  }
}

bool NativeSynthDeviceController::save_favourites() const {
  std::string out;
  for (const auto& key : favourites_)
    out += key.toStdString() + '\n';
  return QDir().mkpath(user_preset_directory()) && write_preset(favourites_file(), out);
}

bool NativeSynthDeviceController::set_favourite(const QString& key, bool favourite) {
  if (key.isEmpty() || favourites_.contains(key) == favourite)
    return true;
  if (favourite)
    favourites_.append(key);
  else
    favourites_.removeAll(key);
  emit favourites_changed();
  return save_favourites();
}

bool NativeSynthDeviceController::export_preset(const QString& path) {
  const auto text = adapter_.preset_text();
  return text && write_preset(local_path(path), *text);
}

bool NativeSynthDeviceController::import_preset(const QString& path) {
  const auto file = local_path(path);
  const auto text = read_preset(file);
  auto name = QFileInfo(file).completeBaseName().trimmed();
  if (!usable_preset_name(name))
    name = QStringLiteral("Imported");
  if (!text || !load_preset_text(*text, name))
    return false;
  // Kept with the user's own presets once it has loaded.
  if (QDir().mkpath(user_preset_directory()))
    (void)write_preset(user_preset_file(name),
                       std::string(text->constData(), static_cast<std::size_t>(text->size())));
  emit user_presets_changed();
  return true;
}

// The slot and the path are named at every call (from QML).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool NativeSynthDeviceController::import_media(const QString& slot, const QString& path) {
  const auto file = local_path(path).toStdString();
  const bool imported = adapter_.import_media(slot.toStdString(), file);
  refresh();
  // A refusal reads in the status even when nothing else changed.
  emit status_changed();
  return imported;
}

void NativeSynthDeviceController::set_clipboard_text(const QString& text) {
  if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr)
    QGuiApplication::clipboard()->setText(text);
}

QString NativeSynthDeviceController::clipboard_text() const {
  if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr)
    return {};
  return QGuiApplication::clipboard()->text();
}

int NativeSynthDeviceController::macro_map_index() const {
  return macro_map_ ? macro_map_->armed() : -1;
}

void NativeSynthDeviceController::set_macro_map_state(MacroMapState* state) {
  if (macro_map_ == state)
    return;
  if (macro_map_)
    disconnect(macro_map_, nullptr, this, nullptr);
  macro_map_ = state;
  if (macro_map_)
    connect(macro_map_, &MacroMapState::armed_changed, this,
            &NativeSynthDeviceController::macro_map_changed);
  emit macro_map_changed();
}

int NativeSynthDeviceController::macro_of(const QString& id) const {
  const auto key = id.toStdString();
  const auto found =
      std::ranges::find(macro_bindings_, key, &std::pair<std::string, std::uint32_t>::first);
  return found == macro_bindings_.end() ? -1 : static_cast<int>(found->second);
}

bool NativeSynthDeviceController::map_to_macro(const QString& id, int index) {
  const auto parameter = index_of(id);
  if (!parameter || index < 0 || index >= 8 || !status_.editable)
    return false;
  const auto& descriptor = engine_.parameters[*parameter];
  const double current = values_[*parameter];
  const bool at_top = current >= descriptor.maximum;
  const bool mapped = adapter_.map_to_macro(descriptor.id, static_cast<std::uint32_t>(index),
                                            at_top ? descriptor.minimum : current,
                                            at_top ? current : descriptor.maximum);
  refresh();
  emit status_changed();
  return mapped;
}

bool NativeSynthDeviceController::unmap_macro(const QString& id) {
  const auto parameter = index_of(id);
  if (!parameter || !status_.editable)
    return false;
  const bool unmapped = adapter_.unmap_macro(engine_.parameters[*parameter].id);
  refresh();
  return unmapped;
}

void NativeSynthDeviceController::end_macro_map() {
  if (macro_map_)
    macro_map_->set_armed(-1);
}

void NativeSynthDeviceController::unlink() {
  if (!status_.editable)
    return;
  adapter_.unlink();
  refresh();
}

void NativeSynthDeviceController::identify_preset() {
  if (preset_index_ >= 0) {
    return; // Keep the loaded preset's name, shown as modified after edits.
  }
  for (std::size_t i = 0; i < preset_values_.size(); ++i) {
    if (preset_values_[i] == values_) {
      preset_index_ = static_cast<int>(i);
      return;
    }
  }
}

double NativeSynthDeviceController::frequency_at(double fraction) const {
  return min_hz * std::pow(max_hz / min_hz, std::clamp(fraction, 0.0, 1.0));
}

double NativeSynthDeviceController::fraction_of(double hz) const {
  return std::clamp(std::log(std::max(hz, min_hz) / min_hz) / std::log(max_hz / min_hz), 0.0, 1.0);
}

namespace {

// Where an engine's output spectrum sits in its telemetry frame.
struct SpectrumSpan {
  std::size_t first;
  std::size_t count;
  [[nodiscard]] std::size_t end() const noexcept { return first + count; }
};

std::optional<SpectrumSpan> output_spectrum_span(std::string_view engine) {
  namespace fx = native_dsp::effects;
  if (engine == "shape") {
    return SpectrumSpan{fx::shape::telemetry::output_spectrum, fx::shape::spectrum_bands};
  }
  if (engine == "strata") {
    return SpectrumSpan{fx::strata::telemetry::output_spectrum, fx::strata::display_bands};
  }
  if (engine == "temper") {
    return SpectrumSpan{fx::temper::telemetry::output_spectrum, fx::temper::display_bands};
  }
  return std::nullopt;
}

} // namespace

QVariantMap NativeSynthDeviceController::telemetry() {
  namespace synth = native_dsp::synth;
  QVariantMap out;
  std::vector<float> frame;
  const bool live = adapter_.telemetry(frame);
  out[QStringLiteral("live")] = false;
  if (!live) {
    strip_view_.clear();
    return out;
  }
  // The raw frame too, for panels that decode their engine's documented
  // layout themselves (the native effects, ADR-0040).
  QVariantList raw;
  raw.reserve(static_cast<qsizetype>(frame.size()));
  for (const float value : frame) {
    raw.push_back(static_cast<double>(value));
  }
  out[QStringLiteral("frame")] = raw;
  // The output spectrum (dBFS, log-spaced bands from low to high), for the
  // device-chain strip's live view. Engines without one leave it out, and the
  // strip shows its response or a baseline instead.
  if (const auto span = output_spectrum_span(engine_.id); span && frame.size() >= span->end()) {
    QVariantList spectrum;
    spectrum.reserve(static_cast<qsizetype>(span->count));
    for (std::size_t band = span->first; band < span->end(); ++band) {
      spectrum.push_back(static_cast<double>(frame[band]));
    }
    out[QStringLiteral("spectrum")] = spectrum;
  }
  if (auto strip =
          strip_view_.decode(engine_.id, frame,
                             [this](std::string_view parameter) {
                               return value(QString::fromUtf8(
                                   parameter.data(), static_cast<qsizetype>(parameter.size())));
                             });
      !strip.isEmpty()) {
    out[QStringLiteral("strip")] = strip;
  }
  const std::string_view id = engine_.id;
  const auto at = [&](std::size_t i) {
    return i < frame.size() ? static_cast<double>(frame[i]) : 0.0;
  };
  if (id == "torque" && frame.size() >= synth::torque::telemetry::size) {
    namespace t = synth::torque::telemetry;
    out[QStringLiteral("running")] = at(t::running) > 0.5;
    out[QStringLiteral("step")] = static_cast<int>(at(t::step));
    out[QStringLiteral("key")] = at(t::key);
    out[QStringLiteral("sweep")] = at(t::sweep);
    out[QStringLiteral("cutoff")] = at(t::cutoff);
    out[QStringLiteral("gate")] = at(t::gate) > 0.5;
  } else if (id == "pulse" && frame.size() >= synth::pulse::telemetry::size) {
    namespace t = synth::pulse::telemetry;
    out[QStringLiteral("gain_reduction")] = at(t::gain_reduction);
    QVariantList pads;
    for (std::size_t pad = 0; pad < synth::pulse::pad_count; ++pad) {
      pads.push_back(at(t::first_pad + pad));
    }
    out[QStringLiteral("pads")] = pads;
  } else if (id == "drift" && !frame.empty()) {
    namespace t = synth::drift::telemetry;
    const auto count = static_cast<std::size_t>(at(t::count));
    QVariantList grains;
    for (std::size_t g = 0; g < count && t::first_grain + (g + 1) * t::stride <= frame.size();
         ++g) {
      const std::size_t base = t::first_grain + g * t::stride;
      QVariantMap grain;
      grain[QStringLiteral("key")] = static_cast<int>(at(base + t::key));
      grain[QStringLiteral("behind")] = at(base + t::behind);
      grain[QStringLiteral("semitones")] = at(base + t::semitones);
      grain[QStringLiteral("reverse")] = at(base + t::reverse) > 0.5;
      grain[QStringLiteral("phase")] = at(base + t::phase);
      grain[QStringLiteral("frozen")] = at(base + t::frozen) > 0.5;
      grains.push_back(grain);
    }
    out[QStringLiteral("grains")] = grains;
  } else if (id == "contour" && frame.size() >= native_dsp::effects::contour::telemetry::size) {
    namespace t = native_dsp::effects::contour::telemetry;
    out[QStringLiteral("cutoff_hz")] = at(t::cutoff_hz);
    out[QStringLiteral("envelope")] = at(t::envelope);
    out[QStringLiteral("lfo")] = at(t::lfo);
  } else if (id == "ember" && frame.size() >= synth::ember::telemetry::size) {
    namespace t = synth::ember::telemetry;
    QVariantList cards;
    for (std::size_t c = 0; c < synth::ember_circuit::cards; ++c) {
      cards.push_back(QVariantMap{{QStringLiteral("active"), at(t::card_active + c) > 0.5},
                                  {QStringLiteral("vco_cents"), at(t::card_vco_cents + c)},
                                  {QStringLiteral("dco_cents"), at(t::card_dco_cents + c)},
                                  {QStringLiteral("cutoff"), at(t::card_cutoff + c)}});
    }
    out[QStringLiteral("cards")] = cards;
    out[QStringLiteral("hum")] = at(t::hum_gate);
    out[QStringLiteral("load")] = at(t::load);
  } else if (id == "concert" && frame.size() >= synth::concert::telemetry::size) {
    namespace t = synth::concert::telemetry;
    out[QStringLiteral("sustain")] = at(t::sustain);
    out[QStringLiteral("sostenuto")] = at(t::sostenuto);
    out[QStringLiteral("soft")] = at(t::soft);
    QVariantList energies;
    QVariantList dampers;
    for (std::size_t key = 0; key < t::first_damper - t::first_energy; ++key) {
      energies.push_back(at(t::first_energy + key));
      dampers.push_back(at(t::first_damper + key));
    }
    out[QStringLiteral("energies")] = energies;
    out[QStringLiteral("dampers")] = dampers;
    out[QStringLiteral("library")] = concert_state_name(
        static_cast<synth::concert::library::State>(static_cast<int>(at(t::library))));
    out[QStringLiteral("underruns")] = at(t::underruns);
  } else if (id == "orbit" && frame.size() >= native_dsp::effects::orbit::telemetry::size) {
    namespace t = native_dsp::effects::orbit::telemetry;
    out[QStringLiteral("lfo_left")] = at(t::lfo_left);
    out[QStringLiteral("lfo_right")] = at(t::lfo_right);
    out[QStringLiteral("sweep_left")] = at(t::sweep_left);
    out[QStringLiteral("sweep_right")] = at(t::sweep_right);
  } else if (id == "forge" && frame.size() >= native_dsp::effects::forge::telemetry::size) {
    namespace t = native_dsp::effects::forge::telemetry;
    out[QStringLiteral("input_rms")] = at(t::input_rms);
    out[QStringLiteral("output_rms")] = at(t::output_rms);
    out[QStringLiteral("makeup_db")] = at(t::makeup_db);
    QVariantList curve;
    for (std::size_t i = 0; i < native_dsp::effects::forge::curve_points; ++i) {
      curve.push_back(at(t::curve + i));
    }
    out[QStringLiteral("curve")] = curve;
  } else if (id == "shape" && frame.size() >= native_dsp::effects::shape::telemetry::size) {
    namespace t = native_dsp::effects::shape::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    QVariantList before;
    QVariantList after;
    for (std::size_t b = 0; b < native_dsp::effects::shape::spectrum_bands; ++b) {
      before.push_back(at(t::input_spectrum + b));
      after.push_back(at(t::output_spectrum + b));
    }
    out[QStringLiteral("input_spectrum")] = before;
    out[QStringLiteral("output_spectrum")] = after;
  } else if (id == "press" && frame.size() >= native_dsp::effects::press::telemetry::size) {
    namespace t = native_dsp::effects::press::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    out[QStringLiteral("reduction")] = at(t::reduction);
  } else if (id == "strata" && frame.size() >= native_dsp::effects::strata::telemetry::size) {
    namespace strata = native_dsp::effects::strata;
    namespace t = strata::telemetry;
    const auto run = [&](std::size_t from, std::size_t count) {
      QVariantList values;
      for (std::size_t i = 0; i < count; ++i) {
        values.push_back(at(from + i));
      }
      return values;
    };
    out[QStringLiteral("input_peak")] = run(t::input_peak, 2);
    out[QStringLiteral("output_peak")] = run(t::output_peak, 2);
    out[QStringLiteral("keyed")] = at(t::keyed) > 0.5;
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    out[QStringLiteral("auto_gain_db")] = at(t::auto_gain_db);
    out[QStringLiteral("learn_progress")] = at(t::learn_progress);
    out[QStringLiteral("band_count")] = static_cast<int>(at(t::band_count));
    out[QStringLiteral("band_gain")] = run(t::band_gain, strata::max_bands);
    out[QStringLiteral("band_level")] = run(t::band_level, strata::max_bands);
    out[QStringLiteral("learned")] = run(t::learned, strata::max_bands);
    out[QStringLiteral("crossovers")] = run(t::crossovers, strata::max_crossovers);
    out[QStringLiteral("input_spectrum")] = run(t::input_spectrum, strata::display_bands);
    out[QStringLiteral("output_spectrum")] = run(t::output_spectrum, strata::display_bands);
  } else if (id == "pump" && frame.size() >= native_dsp::effects::pump::telemetry::size) {
    namespace pump = native_dsp::effects::pump;
    namespace t = pump::telemetry;
    out[QStringLiteral("phase")] = at(t::phase);
    out[QStringLiteral("gain_db")] = at(t::gain_db);
    out[QStringLiteral("cycle_seconds")] = at(t::cycle_seconds);
    out[QStringLiteral("cycle_beats")] = at(t::cycle_beats);
    out[QStringLiteral("keyed")] = at(t::keyed) > 0.5;
    out[QStringLiteral("gain_mid_db")] = at(t::gain_mid_db);
    out[QStringLiteral("gain_high_db")] = at(t::gain_high_db);
    out[QStringLiteral("last_note")] = static_cast<int>(at(t::last_note));
    out[QStringLiteral("step")] = static_cast<int>(at(t::step));
    out[QStringLiteral("slot")] = static_cast<int>(at(t::slot));
    QVariantList kick;
    QVariantList applied;
    QVariantList input;
    QVariantList output;
    for (std::size_t i = 0; i < pump::view_points; ++i) {
      kick.push_back(at(t::kick + i));
      applied.push_back(at(t::applied + i));
      input.push_back(at(t::input + i));
      output.push_back(at(t::output + i));
    }
    out[QStringLiteral("kick")] = kick;
    out[QStringLiteral("applied")] = applied;
    out[QStringLiteral("input")] = input;
    out[QStringLiteral("output")] = output;
  } else if (id == "chop" && frame.size() >= native_dsp::effects::chop::telemetry::size) {
    namespace chop = native_dsp::effects::chop;
    namespace t = chop::telemetry;
    out[QStringLiteral("phase")] = at(t::phase);
    out[QStringLiteral("step")] = static_cast<int>(at(t::step));
    out[QStringLiteral("slot")] = static_cast<int>(at(t::slot));
    out[QStringLiteral("pattern_beats")] = at(t::pattern_beats);
    out[QStringLiteral("pattern")] = static_cast<int>(at(t::pattern));
    QVariantList played;
    for (std::size_t i = 0; i < chop::max_steps; ++i) {
      played.push_back(at(t::played + i) > 0.5);
    }
    QVariantList input;
    QVariantList output;
    QVariantList gain;
    for (std::size_t i = 0; i < chop::view_points; ++i) {
      input.push_back(at(t::input + i));
      output.push_back(at(t::output + i));
      gain.push_back(at(t::gain + i));
    }
    out[QStringLiteral("played")] = played;
    out[QStringLiteral("input")] = input;
    out[QStringLiteral("output")] = output;
    out[QStringLiteral("gain")] = gain;
  } else if (id == "ceiling" && frame.size() >= native_dsp::effects::ceiling::telemetry::size) {
    namespace t = native_dsp::effects::ceiling::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    out[QStringLiteral("reduction")] = at(t::reduction);
    out[QStringLiteral("clipped")] = at(t::clipped);
    out[QStringLiteral("guarded")] = at(t::guarded);
    // No reading yet (silence, too short, or just after a Gain change): NaN.
    const auto loudness = [&](std::size_t index) {
      const double value = at(index);
      return value > static_cast<double>(native_dsp::effects::ceiling::no_loudness)
                 ? value
                 : std::numeric_limits<double>::quiet_NaN();
    };
    out[QStringLiteral("momentary")] = loudness(t::momentary);
    out[QStringLiteral("short_term")] = loudness(t::short_term);
    out[QStringLiteral("integrated")] = loudness(t::integrated);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
  } else if (id == "edge" && frame.size() >= native_dsp::effects::edge::telemetry::size) {
    namespace edge = native_dsp::effects::edge;
    namespace t = edge::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    out[QStringLiteral("clipped")] = at(t::clipped);
    out[QStringLiteral("dot_input")] = at(t::dot_input);
    out[QStringLiteral("dot_output")] = at(t::dot_output);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    out[QStringLiteral("factor")] = at(t::factor);
    QVariantList history;
    for (std::size_t b = 0; b < edge::history_beats; ++b) {
      history.push_back(at(t::history + b));
    }
    out[QStringLiteral("history")] = history;
  } else if (id == "cast" && frame.size() >= native_dsp::effects::cast::telemetry::size) {
    namespace cast = native_dsp::effects::cast;
    namespace t = cast::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("wet_peak")] = QVariantList{at(t::wet_peak), at(t::wet_peak + 1)};
    out[QStringLiteral("duck")] = at(t::duck);
    out[QStringLiteral("length_seconds")] = at(t::length_seconds);
    out[QStringLiteral("responses")] = at(t::responses);
    out[QStringLiteral("rt60_seconds")] = at(t::rt60_seconds);
    QVariantList envelope;
    QVariantList decay;
    for (std::size_t p = 0; p < cast::overview_points; ++p) {
      envelope.push_back(at(t::envelope + p));
      decay.push_back(at(t::decay_curve + p));
    }
    out[QStringLiteral("envelope")] = envelope;
    out[QStringLiteral("decay_curve")] = decay;
  } else if (id == "hush" && frame.size() >= native_dsp::effects::hush::telemetry::size) {
    namespace hush = native_dsp::effects::hush;
    namespace t = hush::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    out[QStringLiteral("key_level")] = at(t::key_level);
    out[QStringLiteral("reduction")] = at(t::reduction);
    out[QStringLiteral("open")] = at(t::open);
    out[QStringLiteral("open_threshold")] = at(t::open_threshold);
    out[QStringLiteral("close_threshold")] = at(t::close_threshold);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    QVariantList peaks;
    QVariantList gains;
    for (std::size_t s = 0; s < hush::history_slices; ++s) {
      peaks.push_back(at(t::history_peak + s));
      gains.push_back(at(t::history_gain + s));
    }
    out[QStringLiteral("history_peak")] = peaks;
    out[QStringLiteral("history_gain")] = gains;
  } else if (id == "strike" && frame.size() >= native_dsp::effects::strike::telemetry::size) {
    namespace strike = native_dsp::effects::strike;
    namespace t = strike::telemetry;
    const auto run = [&](std::size_t from, std::size_t count) {
      QVariantList values;
      for (std::size_t i = 0; i < count; ++i) {
        values.push_back(at(from + i));
      }
      return values;
    };
    out[QStringLiteral("input_peak")] = run(t::input_peak, 2);
    out[QStringLiteral("output_peak")] = run(t::output_peak, 2);
    out[QStringLiteral("clipped")] = at(t::clipped);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    out[QStringLiteral("band_count")] = static_cast<int>(at(t::band_count));
    out[QStringLiteral("crossovers")] = run(t::crossovers, strike::max_crossovers);
    out[QStringLiteral("band_gain")] = run(t::band_gain, strike::max_bands);
    out[QStringLiteral("history_input")] = run(t::history_input, strike::history_slices);
    out[QStringLiteral("history_output")] = run(t::history_output, strike::history_slices);
    QVariantList gains;
    for (std::size_t b = 0; b < strike::max_bands; ++b) {
      gains.push_back(run(t::history_gain + b * strike::history_slices, strike::history_slices));
    }
    out[QStringLiteral("history_gain")] = gains;
  } else if (id == "keel" && frame.size() >= native_dsp::effects::keel::telemetry::size) {
    namespace keel = native_dsp::effects::keel;
    namespace t = keel::telemetry;
    const auto run = [&](std::size_t from, std::size_t count) {
      QVariantList values;
      for (std::size_t i = 0; i < count; ++i) {
        values.push_back(at(from + i));
      }
      return values;
    };
    out[QStringLiteral("input_peak")] = run(t::input_peak, 2);
    out[QStringLiteral("output_peak")] = run(t::output_peak, 2);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    out[QStringLiteral("match_db")] = at(t::match_db);
    out[QStringLiteral("tracked")] = at(t::tracked) > 0.5;
    out[QStringLiteral("referenced")] = at(t::referenced) > 0.5;
    out[QStringLiteral("target_source")] = static_cast<int>(std::lround(at(t::target_source)));
    out[QStringLiteral("target")] = run(t::target, keel::bands);
    out[QStringLiteral("balance")] = run(t::spectrum, keel::bands);
    out[QStringLiteral("correction")] = run(t::correction, keel::bands);
    QVariantList history;
    for (std::size_t s = 0; s < keel::history_slices; ++s) {
      history.push_back(run(t::history + s * keel::bands, keel::bands));
    }
    out[QStringLiteral("history")] = history;
  } else if (id == "dust" && frame.size() >= native_dsp::effects::dust::telemetry::size) {
    namespace dust = native_dsp::effects::dust;
    namespace t = dust::telemetry;
    QVariantList layers;
    for (std::size_t layer = 0; layer < dust::layer_count; ++layer) {
      layers.push_back(at(t::first_layer + layer));
    }
    out[QStringLiteral("layers")] = layers;
    out[QStringLiteral("duck_db")] = at(t::duck_db);
    out[QStringLiteral("rotation")] = at(t::rotation);
    out[QStringLiteral("pops")] = at(t::pops);
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
  } else if (id == "flyby" && frame.size() >= native_dsp::effects::flyby::telemetry::size) {
    namespace t = native_dsp::effects::flyby::telemetry;
    out[QStringLiteral("x")] = at(t::x);
    out[QStringLiteral("y")] = at(t::y);
    out[QStringLiteral("z")] = at(t::z);
    out[QStringLiteral("distance")] = at(t::distance);
    out[QStringLiteral("pitch_cents")] = at(t::pitch_cents);
    out[QStringLiteral("itd_us")] = at(t::itd_us);
    out[QStringLiteral("level_left_db")] = at(t::level_left_db);
    out[QStringLiteral("level_right_db")] = at(t::level_right_db);
    out[QStringLiteral("room")] = at(t::room);
    out[QStringLiteral("position")] = at(t::position);
    out[QStringLiteral("closest")] = at(t::closest);
    out[QStringLiteral("speed")] = at(t::speed);
    out[QStringLiteral("input_peak")] = at(t::input_peak);
    out[QStringLiteral("output_peak_left")] = at(t::output_peak);
    out[QStringLiteral("output_peak_right")] = at(t::output_peak + 1);
  } else if (id == "temper" && frame.size() >= native_dsp::effects::temper::telemetry::size) {
    namespace temper = native_dsp::effects::temper;
    namespace t = temper::telemetry;
    out[QStringLiteral("input_peak")] = QVariantList{at(t::input_peak), at(t::input_peak + 1)};
    out[QStringLiteral("output_peak")] = QVariantList{at(t::output_peak), at(t::output_peak + 1)};
    out[QStringLiteral("max_cut")] = at(t::max_cut);
    out[QStringLiteral("keyed")] = at(t::keyed) > 0.5;
    out[QStringLiteral("transient")] = at(t::transient);
    out[QStringLiteral("latency_ms")] = at(t::latency_ms);
    const auto run = [&](std::size_t first, std::size_t count) {
      QVariantList values;
      for (std::size_t i = 0; i < count; ++i) {
        values.push_back(at(first + i));
      }
      return values;
    };
    out[QStringLiteral("band_gain")] = run(t::band_gain, temper::bands);
    out[QStringLiteral("band_level")] = run(t::band_level, temper::bands);
    out[QStringLiteral("input_spectrum")] = run(t::input_spectrum, temper::display_bands);
    out[QStringLiteral("output_spectrum")] = run(t::output_spectrum, temper::display_bands);
    out[QStringLiteral("sidechain_spectrum")] = run(t::sidechain_spectrum, temper::display_bands);
    out[QStringLiteral("gain")] = run(t::gain, temper::display_bands);
  } else if (id == "anvil" && frame.size() >= synth::anvil::telemetry::size) {
    // The Anvil panel reads its raw frame (anvil.hpp); these name it too.
    namespace t = synth::anvil::telemetry;
    out[QStringLiteral("position")] = at(t::position);
    out[QStringLiteral("last_note")] = at(t::last_note);
    out[QStringLiteral("last_velocity")] = at(t::last_velocity);
    out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
  } else if (id == "fret" && frame.size() >= synth::fret::telemetry::size) {
    // fret.hpp: the latest note as Feel played it, and a count of notes
    // that changes with each one.
    namespace t = synth::fret::telemetry;
    out[QStringLiteral("library")] = library_state_name(
        static_cast<synth::fret::library::State>(static_cast<int>(at(t::library))));
    out[QStringLiteral("underruns")] = at(t::underruns);
    out[QStringLiteral("notes")] = static_cast<int>(std::lround(at(t::notes)));
    const int last_key = static_cast<int>(std::lround(at(t::last_key)));
    out[QStringLiteral("last_key")] = last_key;
    out[QStringLiteral("last_name")] = key_name(last_key);
    out[QStringLiteral("last_velocity")] = static_cast<int>(std::lround(at(t::last_velocity)));
    out[QStringLiteral("last_articulation")] = articulation_name(
        static_cast<synth::fret::feel::Articulation>(std::lround(at(t::last_articulation))));
    out[QStringLiteral("last_pushed")] = at(t::last_pushed) > 0.5;
    out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
  } else if (id == "wren" && frame.size() >= synth::wren::telemetry::size) {
    // wren.hpp: the latest note as Breath played it, and a count of notes
    // that changes with each one.
    namespace t = synth::wren::telemetry;
    out[QStringLiteral("library")] = library_state_name(
        static_cast<synth::wren::library::State>(static_cast<int>(at(t::library))));
    out[QStringLiteral("notes")] = static_cast<int>(std::lround(at(t::notes)));
    const int last_key = static_cast<int>(std::lround(at(t::last_key)));
    out[QStringLiteral("last_key")] = last_key;
    out[QStringLiteral("last_name")] = key_name(last_key);
    out[QStringLiteral("last_velocity")] = static_cast<int>(std::lround(at(t::last_velocity)));
    out[QStringLiteral("last_tongue")] =
        tongue_name(static_cast<synth::wren::breath::Tongue>(std::lround(at(t::last_tongue))));
    out[QStringLiteral("last_staccato")] = at(t::last_staccato) > 0.5;
    out[QStringLiteral("last_grace")] = at(t::last_grace) > 0.5;
    out[QStringLiteral("vibrato")] = at(t::vibrato) > 0.5;
    out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
  } else if (id == "bow" && frame.size() >= synth::bow::telemetry::size) {
    // bow.hpp: the articulation playing (latched by a keyswitch or not), the
    // wheel as played, the newest note's vibrato, the voices and held keys.
    namespace t = synth::bow::telemetry;
    out[QStringLiteral("library")] = library_state_name(
        static_cast<synth::bow::library::State>(static_cast<int>(at(t::library))));
    out[QStringLiteral("notes")] = static_cast<int>(std::lround(at(t::notes)));
    const auto articulation = static_cast<std::size_t>(
        std::clamp<long>(std::lround(at(t::articulation)), 0,
                         static_cast<long>(synth::bow::articulation_count) - 1));
    out[QStringLiteral("articulation")] = static_cast<int>(articulation);
    out[QStringLiteral("latched")] = at(t::latched) > 0.5;
    out[QStringLiteral("dynamics")] = at(t::dynamics);
    out[QStringLiteral("dynamic_name")] = text(synth::bow::dynamic_name(at(t::dynamics)));
    out[QStringLiteral("vibrato")] = at(t::vibrato);
    out[QStringLiteral("voices")] = static_cast<int>(std::lround(at(t::voices)));
    out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
    QVariantList held;
    const auto count = std::min<std::size_t>(
        t::held_slots, static_cast<std::size_t>(std::max<long>(0, std::lround(at(t::held_count)))));
    for (std::size_t slot = 0; slot < count; ++slot) {
      held.push_back(static_cast<int>(std::lround(at(t::held + slot))));
    }
    out[QStringLiteral("held")] = held;
  } else if (id == "stick") {
    stick_panel::decode(frame, out);
  } else if (id == "knock" && frame.size() >= synth::knock::telemetry::size) {
    // knock.hpp: the latest hit's pad (-1 before any) and the hit count,
    // which the panel watches for new hits; each pad's peak this block.
    namespace t = synth::knock::telemetry;
    out[QStringLiteral("last_pad")] = static_cast<int>(std::lround(at(t::last_pad)));
    out[QStringLiteral("hits")] = static_cast<int>(std::lround(at(t::hits)));
    QVariantList levels;
    for (std::size_t pad = 0; pad < synth::knock::pad_count; ++pad)
      levels.push_back(at(t::first_level + pad));
    out[QStringLiteral("levels")] = levels;
    out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
    // Where each pad's latest hit landed, -1 (left) .. 1 (right).
    QVariantList places;
    for (std::size_t pad = 0; pad < synth::knock::pad_count; ++pad)
      places.push_back(at(t::first_place + pad));
    out[QStringLiteral("places")] = places;
  } else {
    return out; // This engine publishes nothing the panel decodes.
  }
  out[QStringLiteral("live")] = true;
  return out;
}

QVariantList NativeSynthDeviceController::pad_envelope(int pad, double seconds, int points) const {
  if (pad < 0 || points < 2) {
    return {};
  }
  return list(pulse_pad_envelope(engine_, values_, static_cast<std::size_t>(pad), seconds,
                                 static_cast<std::size_t>(std::min(points, 1024))));
}

QVariantList NativeSynthDeviceController::pad_pitch(int pad, double seconds, int points) const {
  if (pad < 0 || points < 2) {
    return {};
  }
  return list(pulse_pad_pitch(engine_, values_, static_cast<std::size_t>(pad), seconds,
                              static_cast<std::size_t>(std::min(points, 1024))));
}

// QML invokes this by two named integer arguments; keep its invokable signature stable.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QVariantList NativeSynthDeviceController::pad_band(int pad, int points) const {
  if (pad < 0 || engine_.id != "pulse") {
    return {};
  }
  points = std::clamp(points, 2, 1024);
  QVariantList response;
  for (int i = 0; i < points; ++i) {
    const double hz = frequency_at(static_cast<double>(i) / (points - 1));
    response.append(pulse_pad_band_db(engine_, values_, static_cast<std::size_t>(pad), hz));
  }
  return response;
}

QVariantMap NativeSynthDeviceController::fm_algorithm(int index) const {
  if (engine_.id != "halo" || index < 0) {
    return {};
  }
  const auto layout = fm_algorithm_layout(static_cast<std::size_t>(index));
  if (!layout.valid) {
    return {};
  }
  QVariantList carriers;
  QVariantList edges;
  QVariantList column;
  QVariantList row;
  for (std::size_t op = 0; op < layout.modulates.size(); ++op) {
    carriers.append(((static_cast<unsigned>(layout.carriers) >> op) & 1U) != 0U);
    column.append(layout.column[op]);
    row.append(layout.row[op]);
    for (std::size_t to = 0; to < op; ++to) {
      if (((static_cast<unsigned>(layout.modulates[op]) >> to) & 1U) != 0U) {
        edges.append(QVariantMap{{QStringLiteral("from"), static_cast<int>(op)},
                                 {QStringLiteral("to"), static_cast<int>(to)}});
      }
    }
  }
  return {{QStringLiteral("carriers"), carriers},
          {QStringLiteral("feedback"), static_cast<int>(layout.feedback)},
          {QStringLiteral("feedback_from"), static_cast<int>(layout.feedback_from)},
          {QStringLiteral("edges"), edges},
          {QStringLiteral("column"), column},
          {QStringLiteral("row"), row},
          {QStringLiteral("columns"), layout.columns},
          {QStringLiteral("rows"), layout.rows}};
}

QVariantMap NativeSynthDeviceController::operator_envelope(int op, int points) const {
  if (op < 0 || points < 2) {
    return {};
  }
  const auto sketch = halo_operator_envelope(engine_, values_, static_cast<std::size_t>(op),
                                             static_cast<std::size_t>(std::min(points, 1024)));
  if (sketch.points.empty()) {
    return {};
  }
  return {{QStringLiteral("points"), list(sketch.points)},
          {QStringLiteral("seconds"), sketch.seconds},
          {QStringLiteral("release"), sketch.release_at}};
}

QVariantList NativeSynthDeviceController::oscillator_cycle(int points) const {
  if (points < 2) {
    return {};
  }
  return list(
      torque_oscillator_cycle(engine_, values_, static_cast<std::size_t>(std::min(points, 1024))));
}

QVariantList NativeSynthDeviceController::pitch_drop(double seconds, int points) const {
  if (points < 2) {
    return {};
  }
  return list(torque_pitch_drop(engine_, values_, seconds,
                                static_cast<std::size_t>(std::min(points, 1024))));
}

QVariantList NativeSynthDeviceController::grain_cloud(double seconds, int max_grains) const {
  QVariantList result;
  if (max_grains <= 0 || !(seconds > 0.0)) {
    return result;
  }
  const auto grains = drift_grain_cloud(engine_, values_, std::min(seconds, 60.0),
                                        static_cast<std::size_t>(std::min(max_grains, 4096)));
  result.reserve(static_cast<qsizetype>(grains.size()));
  for (const auto& grain : grains) {
    result.append(QVariantMap{{QStringLiteral("time"), grain.time},
                              {QStringLiteral("position"), grain.position},
                              {QStringLiteral("semitones"), grain.semitones},
                              {QStringLiteral("length"), grain.length},
                              {QStringLiteral("reverse"), grain.reverse},
                              {QStringLiteral("pan"), grain.pan}});
  }
  return result;
}

QVariantMap NativeSynthDeviceController::concert_library() const {
  namespace concert = native_dsp::synth::concert;
  if (engine_.id != concert::engine().descriptor->id) {
    return {};
  }
  return concert_library_map(concert::library::status());
}

QVariantMap NativeSynthDeviceController::concert_reload_library() {
  namespace concert = native_dsp::synth::concert;
  if (engine_.id != concert::engine().descriptor->id) {
    return {};
  }
  // status() looks again whenever the library is missing or invalid.
  auto status = concert::library::status();
  if (status.state != concert::library::State::ready) {
    status = concert::library::load(concert::library::default_directory());
  }
  return concert_library_map(status);
}

QVariantMap NativeSynthDeviceController::fret_library() const {
  namespace fret = native_dsp::synth::fret;
  if (engine_.id != fret::engine().descriptor->id) {
    return {};
  }
  return fret_library_map(fret::library::status());
}

QVariantMap NativeSynthDeviceController::fret_reload_library() {
  namespace fret = native_dsp::synth::fret;
  if (engine_.id != fret::engine().descriptor->id) {
    return {};
  }
  // status() looks again whenever the library is missing or invalid.
  auto status = fret::library::status();
  if (status.state != fret::library::State::ready) {
    status = fret::library::load(fret::library::default_directory());
  }
  return fret_library_map(status);
}

QVariantMap NativeSynthDeviceController::fret_groove(bool as_drawn) const {
  namespace fret = native_dsp::synth::fret;
  namespace feel = fret::feel;
  if (engine_.id != fret::engine().descriptor->id) {
    return {};
  }
  QVariantMap out{{QStringLiteral("bars"), fret_groove_bars},
                  {QStringLiteral("max_velocity"), feel::max_velocity},
                  {QStringLiteral("notes"), QVariantList{}}};
  const auto window = adapter_.first_clip_window(fret_groove_bars);
  if (!window || window->beats_per_bar <= 0.0) {
    return out;
  }
  out[QStringLiteral("beats_per_bar")] = window->beats_per_bar;
  auto settings = fret::feel_settings(values_);
  if (as_drawn) {
    settings.style = feel::Style::as_drawn;
  }
  const double span = window->beats_per_bar * fret_groove_bars;
  // As the kernel meets them: in order, each after the note before.
  feel::Previous previous;
  QVariantList notes;
  for (const auto& clip_note : window->notes) {
    if (clip_note.key < fret::first_key || clip_note.key > fret::last_key) {
      continue;
    }
    const feel::Note note{clip_note.beat,         clip_note.bar_beat, clip_note.bar,
                          clip_note.length_beats, clip_note.key,      clip_note.velocity};
    const auto decision = feel::decide(note, previous, settings);
    previous = feel::after(note, decision);
    const auto articulation = as_drawn ? decision.articulation
                                       : fret::played_articulation(values_, decision.articulation);
    const auto pitch = text(pitch_names[static_cast<std::size_t>(note.key % semitones_per_octave)]);
    notes.push_back(
        QVariantMap{{QStringLiteral("position"), (note.beat - window->start_beat) / span},
                    {QStringLiteral("length"), note.length_beats / span},
                    {QStringLiteral("key"), note.key},
                    {QStringLiteral("pitch"), pitch},
                    {QStringLiteral("name"), key_name(note.key)},
                    {QStringLiteral("drawn"), note.velocity},
                    {QStringLiteral("velocity"), decision.velocity},
                    {QStringLiteral("articulation"), articulation_name(articulation)},
                    {QStringLiteral("pushed"), decision.pushed},
                    {QStringLiteral("delay_ms"), decision.delay_ms}});
  }
  out[QStringLiteral("notes")] = notes;
  return out;
}

QVariantMap NativeSynthDeviceController::wren_library() const {
  namespace wren = native_dsp::synth::wren;
  if (engine_.id != wren::engine().descriptor->id) {
    return {};
  }
  return wren_library_map(wren::library::status());
}

QVariantMap NativeSynthDeviceController::wren_reload_library() {
  namespace wren = native_dsp::synth::wren;
  if (engine_.id != wren::engine().descriptor->id) {
    return {};
  }
  // status() looks again whenever the library is missing or invalid.
  auto status = wren::library::status();
  if (status.state != wren::library::State::ready) {
    status = wren::library::load(wren::library::default_directory());
  }
  return wren_library_map(status);
}

QVariantMap NativeSynthDeviceController::bow_library() const {
  namespace bow = native_dsp::synth::bow;
  if (engine_.id != bow::engine().descriptor->id) {
    return {};
  }
  return bow_library_map(bow::library::status());
}

QVariantMap NativeSynthDeviceController::bow_reload_library() {
  namespace bow = native_dsp::synth::bow;
  if (engine_.id != bow::engine().descriptor->id) {
    return {};
  }
  // status() looks again whenever the library is missing or invalid.
  auto status = bow::library::status();
  if (status.state == bow::library::State::missing ||
      status.state == bow::library::State::invalid) {
    status = bow::library::load(bow::library::default_directory());
  }
  return bow_library_map(status);
}

QVariantMap NativeSynthDeviceController::bow_layout() const {
  namespace bow = native_dsp::synth::bow;
  if (engine_.id != bow::engine().descriptor->id) {
    return {};
  }
  auto section = bow::Section::violas;
  if (const auto at = native_dsp::find_parameter(*bow::engine().descriptor, "bow.section");
      at && *at < values_.size()) {
    section = static_cast<bow::Section>(
        std::clamp<long>(std::lround(values_[*at]), 0, static_cast<long>(bow::section_count) - 1));
  }
  const auto range = bow::playable(section);
  QVariantList switches;
  for (int key = bow::first_keyswitch; key <= bow::last_keyswitch; ++key) {
    switches.push_back(
        QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("name"), key_name(key)}});
  }
  QVariantList dynamics;
  for (std::size_t i = 0; i < bow::dynamic_names.size(); ++i) {
    dynamics.push_back(QVariantMap{
        {QStringLiteral("name"), text(bow::dynamic_names[i])},
        {QStringLiteral("top"), i < bow::dynamic_tops.size() ? bow::dynamic_tops[i] : 1.0}});
  }
  return {{QStringLiteral("dynamics"), dynamics},
          {QStringLiteral("first_key"), bow::first_key},
          {QStringLiteral("last_key"), bow::last_key},
          {QStringLiteral("low"), range.low},
          {QStringLiteral("high"), range.high},
          {QStringLiteral("low_name"), key_name(range.low)},
          {QStringLiteral("high_name"), key_name(range.high)},
          {QStringLiteral("keyswitches"), switches}};
}

namespace {

// The kit Stick's panel shows: the controller's value.
native_dsp::synth::stick::Kit stick_kit(const native_dsp::EngineDescriptor& engine,
                                        std::span<const double> values) {
  namespace stick = native_dsp::synth::stick;
  const auto at = native_dsp::find_parameter(engine, "stick.kit");
  if (!at || *at >= values.size()) {
    return stick::Kit::drs;
  }
  return static_cast<stick::Kit>(
      std::clamp<long>(std::lround(values[*at]), 0, static_cast<long>(stick::kit_count) - 1));
}

} // namespace

QVariantMap NativeSynthDeviceController::stick_library() const {
  namespace stick = native_dsp::synth::stick;
  if (engine_.id != stick::engine().descriptor->id) {
    return {};
  }
  return stick_panel::library(stick_kit(engine_, values_));
}

QVariantMap NativeSynthDeviceController::stick_reload_library() {
  namespace stick = native_dsp::synth::stick;
  if (engine_.id != stick::engine().descriptor->id) {
    return {};
  }
  const auto kit = stick_kit(engine_, values_);
  // status() looks again whenever the kit is missing or invalid.
  const auto status = stick::library::status(kit);
  if (status.state == stick::library::State::missing ||
      status.state == stick::library::State::invalid) {
    (void)stick::library::load(kit, stick::library::kit_directory(kit));
  }
  return stick_panel::library(kit);
}

QVariantMap NativeSynthDeviceController::stick_layout() const {
  namespace stick = native_dsp::synth::stick;
  if (engine_.id != stick::engine().descriptor->id) {
    return {};
  }
  return stick_panel::layout(stick_kit(engine_, values_));
}

QString NativeSynthDeviceController::library_folder(const QString& directory) const {
  // The library's own folder when it exists, else its nearest existing
  // parent, so the action opens somewhere before an install.
  std::filesystem::path folder(directory.toStdString());
  std::error_code error;
  while (!folder.empty() && !std::filesystem::is_directory(folder, error)) {
    const auto parent = folder.parent_path();
    if (parent == folder) {
      return {};
    }
    folder = parent;
  }
  return QString::fromStdString(folder.string());
}

void NativeSynthDeviceController::show_library_folder(const QString& directory) {
  const auto folder = library_folder(directory);
  if (!folder.isEmpty()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  }
}

QVariantMap NativeSynthDeviceController::wren_phrase() const {
  namespace wren = native_dsp::synth::wren;
  namespace breath = wren::breath;
  // Without a tempo the lane places the bloom as at this one.
  constexpr double fallback_bpm = 120.0;
  constexpr double seconds_per_minute = 60.0;
  constexpr double ms_per_second = 1'000.0;
  if (engine_.id != wren::engine().descriptor->id) {
    return {};
  }
  QVariantMap out{{QStringLiteral("bars"), wren_phrase_bars},
                  {QStringLiteral("max_velocity"), breath::max_velocity},
                  {QStringLiteral("notes"), QVariantList{}}};
  const auto window = adapter_.first_clip_window(wren_phrase_bars);
  if (!window || window->beats_per_bar <= 0.0) {
    return out;
  }
  const double bpm = window->bpm > 0.0 ? window->bpm : fallback_bpm;
  const double beats_per_ms = bpm / seconds_per_minute / ms_per_second;
  out[QStringLiteral("beats_per_bar")] = window->beats_per_bar;
  out[QStringLiteral("bpm")] = bpm;
  const auto settings = wren::breath_settings(values_);
  const auto scale = breath::scale_of(window->key);
  const double span = window->beats_per_bar * wren_phrase_bars;
  const auto across = [&](double beat) { return (beat - window->start_beat) / span; };
  // As the kernel meets them: in order, each after the note before.
  breath::Previous previous;
  QVariantList notes;
  int low_key = wren::last_key;
  int high_key = wren::first_key;
  for (const auto& clip_note : window->notes) {
    if (clip_note.key < wren::first_key || clip_note.key > wren::last_key) {
      continue;
    }
    breath::Context note;
    note.beat = clip_note.beat;
    note.bar_beat = clip_note.bar_beat;
    note.bar = clip_note.bar;
    note.beats_per_bar = window->beats_per_bar;
    note.length_beats = clip_note.length_beats;
    note.key = clip_note.key;
    note.velocity = clip_note.velocity;
    note.scale = scale;
    const auto decision = breath::decide(note, previous, settings);
    previous = breath::after(note, decision);
    const auto heard = wren::played(values_, note.key, note.length_beats, decision);
    const int grace_key = note.key + decision.grace_step;
    low_key = std::min(low_key, note.key);
    high_key = std::max(high_key, heard.grace ? grace_key : note.key);
    // A grace plays on the beat; its note starts, slurred, when it ends.
    const double grace_beats = heard.grace ? decision.grace_ms * beats_per_ms : 0.0;
    const double bloom_from = note.beat + grace_beats + decision.vibrato_delay_ms * beats_per_ms;
    const double bloom_full = bloom_from + decision.bloom_ms * beats_per_ms;
    notes.push_back(QVariantMap{{QStringLiteral("position"), across(note.beat)},
                                {QStringLiteral("length"), heard.length_beats / span},
                                {QStringLiteral("key"), note.key},
                                {QStringLiteral("name"), key_name(note.key)},
                                {QStringLiteral("drawn"), clip_note.velocity},
                                {QStringLiteral("velocity"), decision.velocity},
                                {QStringLiteral("tongue"), tongue_name(heard.tongue)},
                                {QStringLiteral("staccato"), decision.staccato},
                                {QStringLiteral("vibrato"), heard.vibrato},
                                {QStringLiteral("vibrato_from"), across(bloom_from)},
                                {QStringLiteral("vibrato_full"), across(bloom_full)},
                                {QStringLiteral("swell_db"), decision.swell_db},
                                {QStringLiteral("grace"), heard.grace},
                                {QStringLiteral("grace_key"), heard.grace ? grace_key : note.key},
                                {QStringLiteral("grace_length"), grace_beats / span},
                                {QStringLiteral("cents"), decision.cents}});
  }
  if (!notes.isEmpty()) {
    out[QStringLiteral("low_key")] = low_key;
    out[QStringLiteral("high_key")] = high_key;
  }
  out[QStringLiteral("notes")] = notes;
  return out;
}

QVariantList NativeSynthDeviceController::contour_response(int points, double cutoff_hz) const {
  return list(quick::contour_response(
      engine_, static_cast<std::size_t>(std::clamp(points, 0, 1'024)), values_, cutoff_hz));
}

QVariantList NativeSynthDeviceController::shape_response(int points, int band) const {
  return list(quick::shape_response(engine_, static_cast<std::size_t>(std::clamp(points, 0, 1'024)),
                                    values_, band));
}

QVariantList NativeSynthDeviceController::press_transfer(double floor_db, int points) const {
  return list(quick::press_transfer(engine_, floor_db, values_,
                                    static_cast<std::size_t>(std::clamp(points, 0, 1'024))));
}

QVariantList NativeSynthDeviceController::strata_transfer(int band, double floor_db,
                                                          int points) const {
  return list(quick::strata_transfer(engine_, values_, band, floor_db,
                                     static_cast<std::size_t>(std::clamp(points, 0, 1'024))));
}

QVariantList NativeSynthDeviceController::strata_crossovers() const {
  return list(quick::strata_crossovers(engine_, values_, strata_sketch_rate));
}

QVariantList NativeSynthDeviceController::strata_band_shapes(int points) const {
  QVariantList out;
  for (const auto& band :
       quick::strata_band_shapes(engine_, values_, strata_sketch_rate,
                                 static_cast<std::size_t>(std::clamp(points, 0, 1'024)))) {
    out.push_back(list(band));
  }
  return out;
}

QVariantMap NativeSynthDeviceController::strata_ott_values() const {
  QVariantMap out;
  for (const auto& named : quick::strata_ott_values(engine_, values_)) {
    out[QString::fromStdString(named.id)] = named.value;
  }
  return out;
}

QVariantList NativeSynthDeviceController::forge_curve(int points) const {
  if (engine_.id != "forge" || points < 2) {
    return {};
  }
  std::vector<float> curve(static_cast<std::size_t>(std::min(points, 1024)));
  native_dsp::effects::forge::transfer_curve(values_, curve);
  QVariantList result;
  for (const float y : curve) {
    result.append(static_cast<double>(y));
  }
  return result;
}

QVariantList NativeSynthDeviceController::edge_transfer(double from_db, double to_db,
                                                        int points) const {
  return list(quick::edge_transfer(engine_, values_, from_db, to_db,
                                   static_cast<std::size_t>(std::clamp(points, 0, 1'024))));
}

QVariantList NativeSynthDeviceController::cast_irs() const {
  QVariantList out;
  for (const auto& ir : native_dsp::effects::cast::builtin_irs()) {
    out.push_back(QVariantMap{
        {QStringLiteral("id"),
         QString::fromUtf8(ir.id.data(), static_cast<qsizetype>(ir.id.size()))},
        {QStringLiteral("name"),
         QString::fromUtf8(ir.name.data(), static_cast<qsizetype>(ir.name.size()))},
        {QStringLiteral("category"),
         QString::fromUtf8(ir.category.data(), static_cast<qsizetype>(ir.category.size()))}});
  }
  return out;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
bool NativeSynthDeviceController::choose_media(const QString& slot, const QString& title) {
  if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) {
    return false;
  }
  // Formats the build cannot decode are refused by the import with a reason.
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, title, QDir::homePath(),
      QStringLiteral("Audio (*.wav *.wave *.aiff *.aif *.flac);;All files (*)"));
  return !chosen.isEmpty() && import_media(slot, chosen);
}

double NativeSynthDeviceController::edge_latency_ms(double sample_rate) const {
  return quick::edge_latency_ms(engine_, values_, sample_rate);
}

double NativeSynthDeviceController::ceiling_latency_ms(double sample_rate) const {
  return quick::ceiling_latency_ms(engine_, values_, sample_rate);
}

double NativeSynthDeviceController::effect_latency_ms(double sample_rate) const {
  return quick::effect_latency_ms(engine_, values_, sample_rate);
}

QVariantMap NativeSynthDeviceController::keel_layout() const {
  namespace keel = native_dsp::effects::keel;
  if (&engine_ != keel::engine().descriptor) {
    return {};
  }
  QVariantList centres;
  for (std::size_t b = 0; b < keel::bands; ++b) {
    centres.push_back(keel::band_hz(b));
  }
  return {{QStringLiteral("band_hz"), centres},
          {QStringLiteral("reference_band"), static_cast<int>(keel::reference_band)},
          {QStringLiteral("history_slices"), static_cast<int>(keel::history_slices)},
          {QStringLiteral("history_seconds"), keel::history_seconds}};
}

QVariantList NativeSynthDeviceController::keel_target() const {
  namespace keel = native_dsp::effects::keel;
  if (&engine_ != keel::engine().descriptor) {
    return {};
  }
  QVariantList out;
  for (const double db : keel::target_curve(values_)) {
    out.push_back(db);
  }
  return out;
}

bool NativeSynthDeviceController::keel_learn() {
  std::vector<float> frame;
  if (!adapter_.telemetry(frame)) {
    return false;
  }
  const auto learned = quick::keel_learned_values(engine_, values_, frame);
  if (learned.empty()) {
    return false;
  }
  QVariantMap edit;
  for (const auto& [id, value] : learned) {
    edit[QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()))] = value;
  }
  set_values(edit);
  return true;
}

double NativeSynthDeviceController::ceiling_learned_gain(double integrated_lufs) const {
  return quick::ceiling_learned_gain(engine_, values_, integrated_lufs);
}

double NativeSynthDeviceController::forge_makeup_db() const {
  return engine_.id == "forge" ? native_dsp::effects::forge::makeup_db(values_) : 0.0;
}

QVariantList NativeSynthDeviceController::partial_spectrum() const {
  QVariantList result;
  for (const auto& line : drift_partial_spectrum(engine_, values_)) {
    result.append(
        QVariantMap{{QStringLiteral("ratio"), line.ratio}, {QStringLiteral("level"), line.level}});
  }
  return result;
}

QVariantList NativeSynthDeviceController::grain_window(int shape, int points) const {
  if (engine_.id != "drift" || points < 2) {
    return {};
  }
  return list(drift_grain_window(shape, static_cast<std::size_t>(std::min(points, 1024))));
}

QVariantMap NativeSynthDeviceController::amp_envelope(int points) const {
  if (points < 2) {
    return {};
  }
  const auto sketch =
      drift_amp_envelope(engine_, values_, static_cast<std::size_t>(std::min(points, 1024)));
  if (sketch.points.empty()) {
    return {};
  }
  return {{QStringLiteral("points"), list(sketch.points)},
          {QStringLiteral("seconds"), sketch.seconds},
          {QStringLiteral("release"), sketch.release_at}};
}

QVariantList NativeSynthDeviceController::filter_response(int points) const {
  QVariantList response;
  if (visuals_ == nullptr) {
    return response;
  }
  points = std::clamp(points, 2, 1024);
  for (int i = 0; i < points; ++i) {
    const double hz = frequency_at(static_cast<double>(i) / (points - 1));
    response.append(visuals_->filter_response_db(engine_, values_, hz));
  }
  return response;
}

QString NativeSynthDeviceController::engine_id() const { return text(engine_.id); }

QString NativeSynthDeviceController::engine_name() const { return text(engine_.name); }

QString NativeSynthDeviceController::panel_url() const {
  const auto* panel = device_panel(engine_.id);
  return panel != nullptr ? text(panel->url) : QString{};
}

int NativeSynthDeviceController::panel_width() const {
  const auto* panel = device_panel(engine_.id);
  return panel != nullptr ? panel->width : 0;
}

int NativeSynthDeviceController::panel_height() const {
  const auto* panel = device_panel(engine_.id);
  return panel != nullptr ? panel->height : 0;
}

QStringList NativeSynthDeviceController::panel_inputs() const {
  const auto* panel = device_panel(engine_.id);
  return panel != nullptr ? text(panel->inputs).split(QLatin1Char(' '), Qt::SkipEmptyParts)
                          : QStringList{};
}

bool NativeSynthDeviceController::panel_reflows() const {
  const auto* panel = device_panel(engine_.id);
  return panel != nullptr && panel->reflows;
}

} // namespace oscillate::quick
