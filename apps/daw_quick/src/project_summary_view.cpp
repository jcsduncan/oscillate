#include "project_summary_view.hpp"

namespace oscillate::quick {

void ProjectSummaryView::refresh(const project::Snapshot& snapshot) {
  master_level_ = snapshot.master_level;
  const auto& meter_events = snapshot.meter.events();
  const QString meter = meter_events.empty() ? QStringLiteral("—")
                                             : QStringLiteral("%1/%2")
                                                   .arg(meter_events.front().numerator)
                                                   .arg(meter_events.front().denominator);
  format_ = QStringLiteral("%1 kHz   /   %2")
                .arg(static_cast<double>(snapshot.sample_rate) / 1000.0, 0, 'f', 1)
                .arg(meter);
}

} // namespace oscillate::quick
