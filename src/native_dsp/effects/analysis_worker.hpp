#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <memory>
#include <mutex>

// The effect platform's background analysis (ADR-0129). A kernel whose
// telemetry is TelemetryAnalysis::continuous captures on the callback and
// is analysed off it; this worker drains every such kernel's capture a few
// times a second, so readings that accumulate (integrated loudness, the
// highest true peak) see every frame whether or not a panel reads them.
namespace oscillate::native_dsp::effects {

// One runtime's analysis: its kernel, and the lock under which the reader
// and the worker take turns analysing it. The runtime clears `kernel`
// under the lock before the kernel goes, so the worker never analyses a
// kernel that is gone.
struct AnalysisSlot {
  std::mutex mutex;
  EffectKernel* kernel = nullptr;

  // Under the lock: the kernel's analysis brought up to its capture.
  void analyse();
};

namespace analysis_worker {

// Control only. The worker starts with the first slot and sleeps while it
// has none.
void add(std::shared_ptr<AnalysisSlot> slot);
void remove(const AnalysisSlot* slot);

} // namespace analysis_worker

} // namespace oscillate::native_dsp::effects
