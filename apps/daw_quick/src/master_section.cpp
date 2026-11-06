#include "master_section.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace oscillate::quick {
namespace {

double or_nan(const std::optional<double>& value) {
  return value ? *value : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

MasterSection::MasterSection(std::shared_ptr<LiveTruePeakMonitor> analysis,
                             std::shared_ptr<std::atomic<bool>> mono_listen, QObject* parent)
    : QObject(parent), analysis_(std::move(analysis)), mono_listen_(std::move(mono_listen)) {
  poll();
}

bool MasterSection::mono_listen() const noexcept {
  return mono_listen_ && mono_listen_->load(std::memory_order_relaxed);
}

void MasterSection::set_mono_listen(bool on) {
  if (!mono_listen_ || mono_listen() == on)
    return;
  mono_listen_->store(on, std::memory_order_relaxed);
  emit monoListenChanged();
}

void MasterSection::reset_loudness() {
  if (analysis_)
    analysis_->reset_loudness();
}

void MasterSection::poll() {
  const auto latest = analysis_ ? analysis_->latest() : std::nullopt;
  const bool valid = latest && latest->transport_generation != 0;
  const auto peak =
      valid ? std::max(latest->peak.left_true_peak, latest->peak.right_true_peak) : 0.0F;
  QVariantMap next{
      {"valid", valid},
      {"momentary", valid ? or_nan(latest->momentary_lufs) : or_nan({})},
      {"shortTerm", valid ? or_nan(latest->short_term_lufs) : or_nan({})},
      {"integrated", valid ? or_nan(latest->integrated_lufs) : or_nan({})},
      {"truePeakDb",
       valid && peak > 0.0F ? 20.0 * std::log10(static_cast<double>(peak)) : or_nan({})},
      {"correlation", valid ? or_nan(latest->correlation) : or_nan({})},
      {"monoLossDb", valid ? or_nan(latest->mono_loss_db) : or_nan({})},
  };
  // NaN never equals itself, so compare what QML would show.
  const auto same = [&] {
    if (next.size() != reading_.size())
      return false;
    for (auto it = next.cbegin(); it != next.cend(); ++it) {
      const auto old = reading_.value(it.key());
      if (it.value().typeId() == QMetaType::Double) {
        const auto a = it.value().toDouble();
        const auto b = old.toDouble();
        if (!(a == b || (std::isnan(a) && std::isnan(b))))
          return false;
      } else if (it.value() != old) {
        return false;
      }
    }
    return true;
  }();
  if (same)
    return;
  reading_ = std::move(next);
  emit changed();
}

} // namespace oscillate::quick
