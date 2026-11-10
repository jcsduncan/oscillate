#include "lens_match.hpp"

#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/native_dsp/effects/lens.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::quick {
namespace {

namespace tonal = native_dsp::tonal;
namespace match_eq = native_dsp::match_eq;
namespace lens = native_dsp::effects::lens;
namespace shape = native_dsp::effects::shape;

constexpr std::size_t input = 0;
constexpr std::size_t sidechain = 1;
constexpr const char* user_prefix = "user/";
const auto chunk_windows =
    static_cast<std::uint64_t>(std::lround(LensMatch::chunk_seconds / tonal::meter_hop_seconds));
const auto ready_windows =
    static_cast<std::uint64_t>(std::ceil(LensMatch::ready_seconds / tonal::meter_hop_seconds));

template <typename Values> QVariantList list(const Values& values) {
  QVariantList out;
  out.reserve(static_cast<qsizetype>(values.size()));
  for (const double value : values) {
    out.append(value);
  }
  return out;
}

[[nodiscard]] bool audible(const tonal::FineCurve& power) {
  return std::ranges::any_of(power, [](double p) { return p > 0.0; });
}

[[nodiscard]] tonal::FineCurve mean_of(const tonal::FineCurve& sum, std::uint64_t windows) {
  tonal::FineCurve out{};
  if (windows == 0) {
    return out;
  }
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    out[i] = sum[i] / static_cast<double>(windows);
  }
  return out;
}

[[nodiscard]] QString kind_name(native_dsp::EqBandKind kind) {
  switch (kind) {
  case native_dsp::EqBandKind::low_shelf:
    return QStringLiteral("Low Shelf");
  case native_dsp::EqBandKind::high_shelf:
    return QStringLiteral("High Shelf");
  default:
    return QStringLiteral("Peak");
  }
}

} // namespace

LensMatch::LensMatch(NativeSynthDeviceController& controller, NativeSynthPanelAdapter& adapter,
                     QString targets_directory, QObject* parent)
    : QObject(parent), controller_(controller), adapter_(adapter),
      directory_(std::move(targets_directory)) {
  load_targets();
  if (!tonal::factory_targets().empty()) {
    target_id_ = QString::fromStdString(tonal::factory_targets().front().id);
  }
  // The match settings are Lens's parameters: a change to them redraws the
  // match; Lens's other values leave it alone.
  connect(&controller_, &NativeSynthDeviceController::values_changed, this, [this] {
    const auto settings = lens::match_settings(controller_.current_values());
    const auto& f = fit_settings_;
    if (settings.amount == f.amount && settings.smoothing_octaves == f.smoothing_octaves &&
        settings.low_hz == f.low_hz && settings.high_hz == f.high_hz &&
        settings.max_boost_db == f.max_boost_db && settings.max_cut_db == f.max_cut_db &&
        matches_target() == fit_to_target_) {
      return;
    }
    refit();
    bump();
  });
  since_fit_.start();
}

QString LensMatch::default_targets_directory() {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty()) {
    data = QDir::homePath() + QStringLiteral("/.local/share");
  }
  return data + QStringLiteral("/oscillate/tonal-targets");
}

double LensMatch::seconds() const noexcept {
  return static_cast<double>(windows_) * tonal::meter_hop_seconds;
}

bool LensMatch::matches_target() const {
  return lens::matches_target(controller_.current_values());
}

bool LensMatch::has_sidechain() const noexcept { return audible(sum_[sidechain]); }

bool LensMatch::ready() const noexcept { return fitted_; }

QVariantList LensMatch::fine_hz() const {
  QVariantList out;
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    out.append(tonal::fine_hz(i));
  }
  return out;
}

QVariantList LensMatch::band_hz() const {
  QVariantList out;
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    out.append(tonal::band_hz(b));
  }
  return out;
}

QVariantList LensMatch::correction() const { return fitted_ ? list(correction_) : QVariantList{}; }

QVariantList LensMatch::response() const {
  return fitted_ ? list(fit_.response_db) : QVariantList{};
}

QVariantList LensMatch::bands() const {
  QVariantList out;
  if (!fitted_) {
    return out;
  }
  for (std::size_t b = 0; b < fit_.size; ++b) {
    const auto& band = fit_.band[b];
    out.append(QVariantMap{{QStringLiteral("kind"), kind_name(band.kind)},
                           {QStringLiteral("hz"), band.hz},
                           {QStringLiteral("shape"), band.shape},
                           {QStringLiteral("gain"), band.gain_db}});
  }
  return out;
}

QVariantList LensMatch::targets() const {
  QVariantList out;
  const auto add = [&](const tonal::Target& t, bool user) {
    out.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(t.id)},
                           {QStringLiteral("name"), QString::fromStdString(t.name)},
                           {QStringLiteral("references"), static_cast<int>(t.references)},
                           {QStringLiteral("user"), user}});
  };
  for (const auto& t : tonal::factory_targets()) {
    add(t, false);
  }
  for (const auto& t : user_targets_) {
    add(t, true);
  }
  return out;
}

const tonal::Target* LensMatch::target() const {
  const auto id = target_id_.toStdString();
  if (const auto* factory = tonal::find_factory_target(id)) {
    return factory;
  }
  const auto found = std::ranges::find(user_targets_, id, &tonal::Target::id);
  return found == user_targets_.end() ? nullptr : &*found;
}

void LensMatch::set_target_id(const QString& id) {
  if (id == target_id_) {
    return;
  }
  target_id_ = id;
  refit();
  bump();
}

QString LensMatch::target_name() const {
  const auto* t = target();
  return t == nullptr ? QString() : QString::fromStdString(t->name);
}

void LensMatch::step_target(int direction) {
  const auto all = targets();
  if (all.isEmpty()) {
    return;
  }
  qsizetype at = 0;
  for (qsizetype i = 0; i < all.size(); ++i) {
    if (all[i].toMap().value(QStringLiteral("id")).toString() == target_id_) {
      at = i;
    }
  }
  const auto next = (at + (direction < 0 ? all.size() - 1 : 1)) % all.size();
  set_target_id(all[next].toMap().value(QStringLiteral("id")).toString());
}

QVariantList LensMatch::tonal() const {
  if (windows_ == 0 || !audible(sum_[input])) {
    return {};
  }
  return list(tonal::normalised(tonal::grid_levels(sum_[input])));
}

QVariantList LensMatch::target_low() const {
  const auto* t = target();
  return t == nullptr ? QVariantList{} : list(t->low);
}

QVariantList LensMatch::target_median() const {
  const auto* t = target();
  return t == nullptr ? QVariantList{} : list(t->median);
}

QVariantList LensMatch::target_high() const {
  const auto* t = target();
  return t == nullptr ? QVariantList{} : list(t->high);
}

QVariantList LensMatch::placements() const {
  const auto* t = target();
  if (t == nullptr || windows_ == 0 || !audible(sum_[input])) {
    return {};
  }
  QVariantList out;
  for (const auto placement :
       tonal::assess(tonal::normalised(tonal::grid_levels(sum_[input])), *t)) {
    out.append(static_cast<int>(placement));
  }
  return out;
}

int LensMatch::in_range() const {
  const auto placed = placements();
  return static_cast<int>(std::ranges::count_if(placed, [](const QVariant& p) {
    return p.toInt() == static_cast<int>(tonal::Placement::inside);
  }));
}

void LensMatch::poll() {
  std::vector<float> frame;
  if (adapter_.telemetry(frame)) {
    ingest(frame);
  }
}

void LensMatch::ingest(std::span<const float> frame) {
  namespace t = lens::telemetry;
  if (frame.size() < t::size) {
    return;
  }
  const auto windows = static_cast<std::uint64_t>(frame[t::tonal_windows]);
  if (windows < last_windows_) {
    // The meter started again (a reset): what it holds now is all new.
    last_windows_ = 0;
    last_mean_ = {};
  }
  if (windows == last_windows_) {
    return;
  }
  const auto added = windows - last_windows_;
  for (std::size_t s = 0; s < 2; ++s) {
    const auto offset = s == input ? t::tonal_input : t::tonal_sidechain;
    for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
      const double mean = frame[offset + i];
      // The new windows' total: the mean over all of them, less what the
      // earlier ones held.
      const double total = std::max(0.0, mean * static_cast<double>(windows) -
                                             last_mean_[s][i] * static_cast<double>(last_windows_));
      sum_[s][i] += total;
      chunk_[s][i] += total;
      last_mean_[s][i] = mean;
    }
  }
  last_windows_ = windows;
  windows_ += added;
  chunk_windows_ += added;
  if (chunk_windows_ >= chunk_windows) {
    for (std::size_t s = 0; s < 2; ++s) {
      if (audible(chunk_[s]) && chunks_[s].size() < max_chunks) {
        chunks_[s].push_back(tonal::normalised(tonal::grid_levels(chunk_[s])));
      }
      chunk_[s] = {};
    }
    chunk_windows_ = 0;
  }
  if (!fitted_ || since_fit_.elapsed() >= refit_ms) {
    refit();
  }
  bump();
}

void LensMatch::reset() {
  sum_ = {};
  chunk_ = {};
  chunks_ = {};
  windows_ = 0;
  chunk_windows_ = 0;
  fitted_ = false;
  apply_message_.clear();
  message_.clear();
  bump();
}

void LensMatch::refit() {
  since_fit_.restart();
  fitted_ = false;
  fit_settings_ = lens::match_settings(controller_.current_values());
  fit_to_target_ = matches_target();
  if (windows_ < ready_windows || !audible(sum_[input])) {
    return;
  }
  const auto mix = tonal::fine_levels(mean_of(sum_[input], windows_));
  match_eq::FineCurve reference{};
  if (matches_target()) {
    const auto* t = target();
    if (t == nullptr) {
      return;
    }
    reference = match_eq::target_levels(*t);
  } else {
    if (!has_sidechain()) {
      return;
    }
    reference = tonal::fine_levels(mean_of(sum_[sidechain], windows_));
  }
  correction_ = match_eq::correction(mix, reference, fit_settings_);
  fit_ = match_eq::fit(correction_, fit_settings_);
  fitted_ = true;
}

bool LensMatch::apply() {
  if (!fitted_) {
    apply_message_ = matches_target() || has_sidechain()
                         ? QStringLiteral("Play a little longer first")
                         : QStringLiteral("Feed the sidechain a reference, or match to a target");
    bump();
    return false;
  }
  const auto values = shape::band_values(std::span(fit_.band.data(), fit_.size));
  if (!adapter_.write_next_effect(shape::engine().descriptor->id, values)) {
    apply_message_ = QString::fromStdString(adapter_.status().message);
    bump();
    return false;
  }
  apply_message_ = QStringLiteral("Wrote %1 bands to Shape").arg(fit_.size);
  bump();
  return true;
}

bool LensMatch::save_target() {
  const std::size_t from = chunks_[sidechain].size() >= min_chunks ? sidechain : input;
  if (chunks_[from].size() < min_chunks) {
    message_ = QStringLiteral("Play at least %1 s first")
                   .arg(static_cast<double>(min_chunks) * chunk_seconds);
    bump();
    return false;
  }
  const auto now = QDateTime::currentDateTime();
  const QString name =
      QStringLiteral("Reference ") + now.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
  const QString stem =
      QStringLiteral("reference-") + now.toString(QStringLiteral("yyyyMMdd-HHmmss"));
  auto built =
      tonal::build_target(user_prefix + stem.toStdString(), name.toStdString(), chunks_[from]);
  if (!built || !QDir().mkpath(directory_)) {
    message_ = QStringLiteral("The target could not be saved");
    bump();
    return false;
  }
  QSaveFile file(directory_ + QLatin1Char('/') + stem + QStringLiteral(".json"));
  const auto text = tonal::serialise_target(built.value());
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(text.data(), static_cast<qint64>(text.size())) !=
          static_cast<qint64>(text.size()) ||
      !file.commit()) {
    message_ = QStringLiteral("The target could not be saved");
    bump();
    return false;
  }
  load_targets();
  target_id_ = QString::fromStdString(built.value().id);
  message_ =
      QStringLiteral("Saved %1 from the %2")
          .arg(name, from == sidechain ? QStringLiteral("sidechain") : QStringLiteral("input"));
  refit();
  bump();
  return true;
}

void LensMatch::load_targets() {
  user_targets_ = read_user_targets(directory_);
  emit targets_changed();
}

std::vector<tonal::Target> LensMatch::read_user_targets(const QString& directory_path) {
  std::vector<tonal::Target> out;
  const QDir directory(directory_path);
  const auto files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const auto& info : files) {
    if (out.size() >= max_user_targets) {
      break;
    }
    if (info.size() > static_cast<qint64>(tonal::max_file_bytes)) {
      continue;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
      continue;
    }
    const auto bytes = file.readAll();
    auto parsed = tonal::parse_target(
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
        user_prefix + info.completeBaseName().toStdString());
    if (parsed) {
      out.push_back(std::move(parsed).value());
    }
  }
  return out;
}

void LensMatch::bump() {
  ++revision_;
  emit changed();
}

} // namespace oscillate::quick
