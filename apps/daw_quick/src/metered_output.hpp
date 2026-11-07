#pragma once

#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/application/spectrum_monitor.hpp"
#include "oscillate/native_dsp/meter.hpp"
#include "true_peak_monitor.hpp"

#include <atomic>
#include <memory>

namespace oscillate::quick {

// Wraps either Quick playback output at its final stereo render boundary.
// The returned output owns the callback context until its device is stopped.
// While `mono_listen` is set the device hears the mix folded to mono,
// (L + R) / 2 on both sides; the meters still read the stereo mix, and
// export never passes here.
[[nodiscard]] application::PlaybackOutputFactory
make_metered_output_factory(application::PlaybackOutputFactory downstream,
                            std::shared_ptr<native_dsp::StereoMeter> meter,
                            std::shared_ptr<application::SpectrumMonitor> spectrum,
                            std::shared_ptr<LiveTruePeakMonitor> true_peak,
                            std::shared_ptr<std::atomic<bool>> mono_listen = {});

} // namespace oscillate::quick
