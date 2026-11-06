#include "oscillate/application/spectrum_monitor.hpp"

#include <chrono>

namespace oscillate::application {
using namespace std::chrono_literals;

SpectrumMonitor::SpectrumMonitor() : worker_([this](const std::stop_token& stop) { run(stop); }) {}

void SpectrumMonitor::set_sample_rate_after_quiesce(std::uint32_t sample_rate) noexcept {
  tap_.invalidate_after_quiesce();
  sample_rate_.store(sample_rate, std::memory_order_release);
}

void SpectrumMonitor::invalidate_after_quiesce() noexcept { tap_.invalidate_after_quiesce(); }

void SpectrumMonitor::observe(std::span<const engine::StereoFrame> frames,
                              engine::BlockContext context) noexcept {
  (void)tap_.observe(frames, context);
}

std::optional<SpectrumReading> SpectrumMonitor::latest() const {
  std::lock_guard lock(latest_mutex_);
  if (!latest_ || latest_->epoch != tap_.epoch())
    return std::nullopt;
  return latest_;
}

void SpectrumMonitor::run(const std::stop_token& stop) noexcept {
  SpectrumWindow window;
  while (!stop.stop_requested()) {
    if (!tap_.try_pop(window)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    const auto rate = sample_rate_.load(std::memory_order_acquire);
    auto spectrum = native_dsp::StereoSpectrumAnalyzer::analyze(window.frames, rate);
    if (!spectrum || window.epoch != tap_.epoch())
      continue;
    std::lock_guard lock(latest_mutex_);
    if (window.epoch == tap_.epoch())
      latest_ =
          SpectrumReading{*spectrum, window.end_sample, window.transport_generation, window.epoch};
  }
}

} // namespace oscillate::application
