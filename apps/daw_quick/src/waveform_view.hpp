#pragma once

#include "oscillate/analysis/waveform.hpp"
#include "oscillate/project/model.hpp"

#include <QVariantList>

#include <cstddef>

namespace oscillate::quick {

// UI-only, bounded envelope for the visible half-open source interval.
[[nodiscard]] QVariantList waveform_view(const analysis::WaveformSummary& summary,
                                         project::SourceRange source_range, std::size_t columns);

} // namespace oscillate::quick
