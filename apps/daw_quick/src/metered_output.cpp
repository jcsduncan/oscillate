#include "metered_output.hpp"

#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/audio_io/render_ahead.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace oscillate::quick {
namespace {

class PipeWirePlaybackOutput final : public application::PlaybackOutput {
public:
  PipeWirePlaybackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor)
      : output_(source, cursor) {}

  [[nodiscard]] foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t latency,
        std::optional<std::uint64_t> target_serial) override {
    return output_.start(rate, latency, target_serial);
  }
  void stop() noexcept override { output_.stop(); }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override { return output_.status(); }

private:
  audio_io::PipeWireOutput output_;
};

class MeteredPlaybackOutput final : public application::PlaybackOutput {
public:
  static std::unique_ptr<application::PlaybackOutput>
  create(audio_io::StereoRenderSource upstream, audio_io::TransportCursor& cursor,
         const application::PlaybackOutputFactory& downstream,
         std::shared_ptr<native_dsp::StereoMeter> meter,
         std::shared_ptr<application::SpectrumMonitor> spectrum,
         std::shared_ptr<LiveTruePeakMonitor> true_peak,
         std::shared_ptr<std::atomic<bool>> mono_listen) {
    auto wrapper = std::unique_ptr<MeteredPlaybackOutput>(
        new MeteredPlaybackOutput(upstream, std::move(meter), std::move(spectrum),
                                  std::move(true_peak), std::move(mono_listen)));
    audio_io::StereoRenderSource observed{&wrapper->context_, &render, &freeze_cursor,
                                          upstream.workers};
    // With render-ahead the meters follow what the device plays, not what
    // is rendered ahead (ADR-0128).
    if (upstream.ahead != nullptr) {
      upstream.ahead->set_tap({&wrapper->context_, &played});
      observed = upstream;
    }
    wrapper->output_ = downstream ? downstream(observed, cursor)
                                  : std::make_unique<PipeWirePlaybackOutput>(observed, cursor);
    if (!wrapper->output_)
      return {};
    return wrapper;
  }

  ~MeteredPlaybackOutput() override { stop(); }

  [[nodiscard]] foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t latency,
        std::optional<std::uint64_t> target_serial) override {
    // A quiesced output may be restarted after Pause. Clear the latch before
    // starting so a failed or partial start can still be stopped on retirement.
    stopped_ = false;
    spectrum_->set_sample_rate_after_quiesce(rate);
    true_peak_->set_sample_rate_after_quiesce(rate);
    return output_->start(rate, latency, target_serial);
  }
  void stop() noexcept override {
    if (stopped_)
      return;
    stopped_ = true;
    if (!output_)
      return;
    output_->stop();
    // Device stop joins the callback. Only then may the writer-owned meter
    // window be reset from this control/retirement thread.
    meter_->invalidate_after_quiesce();
    spectrum_->invalidate_after_quiesce();
    true_peak_->invalidate_after_quiesce();
  }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    return output_->status();
  }

private:
  struct Context {
    audio_io::StereoRenderSource upstream;
    native_dsp::StereoMeter* meter;
    application::SpectrumMonitor* spectrum;
    LiveTruePeakMonitor* true_peak;
    const std::atomic<bool>* mono_listen;
  };

  MeteredPlaybackOutput(audio_io::StereoRenderSource upstream,
                        std::shared_ptr<native_dsp::StereoMeter> meter,
                        std::shared_ptr<application::SpectrumMonitor> spectrum,
                        std::shared_ptr<LiveTruePeakMonitor> true_peak,
                        std::shared_ptr<std::atomic<bool>> mono_listen) noexcept
      : meter_(std::move(meter)), spectrum_(std::move(spectrum)), true_peak_(std::move(true_peak)),
        mono_listen_(std::move(mono_listen)),
        context_{upstream, meter_.get(), spectrum_.get(), true_peak_.get(), mono_listen_.get()} {}

  static bool render(void* opaque, std::span<engine::StereoFrame> frames,
                     engine::BlockContext block) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    const bool rendered =
        context.upstream.render && context.upstream.render(context.upstream.context, frames, block);
    if (!rendered)
      std::ranges::fill(frames, engine::StereoFrame{});
    played(opaque, frames, block);
    return rendered;
  }

  // What the device plays: metered, then folded to mono while the mono
  // check is on.
  static void played(void* opaque, std::span<engine::StereoFrame> frames,
                     engine::BlockContext block) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    context.meter->observe(frames, block);
    context.spectrum->observe(frames, block);
    context.true_peak->observe(frames, block);
    // The mono check folds only what the device hears, after the meters.
    if (context.mono_listen != nullptr && context.mono_listen->load(std::memory_order_relaxed)) {
      for (auto& frame : frames) {
        const auto mono = 0.5F * (frame.left + frame.right);
        frame = {mono, mono};
      }
    }
  }

  static bool freeze_cursor(void* opaque) noexcept {
    const auto& upstream = static_cast<Context*>(opaque)->upstream;
    return upstream.freeze_cursor && upstream.freeze_cursor(upstream.context);
  }

  std::shared_ptr<native_dsp::StereoMeter> meter_;
  std::shared_ptr<application::SpectrumMonitor> spectrum_;
  std::shared_ptr<LiveTruePeakMonitor> true_peak_;
  std::shared_ptr<std::atomic<bool>> mono_listen_;
  Context context_;
  std::unique_ptr<application::PlaybackOutput> output_;
  bool stopped_ = false;
};
} // namespace

application::PlaybackOutputFactory
make_metered_output_factory(application::PlaybackOutputFactory downstream,
                            std::shared_ptr<native_dsp::StereoMeter> meter,
                            std::shared_ptr<application::SpectrumMonitor> spectrum,
                            std::shared_ptr<LiveTruePeakMonitor> true_peak,
                            std::shared_ptr<std::atomic<bool>> mono_listen) {
  return
      [downstream = std::move(downstream), meter = std::move(meter), spectrum = std::move(spectrum),
       true_peak = std::move(true_peak), mono_listen = std::move(mono_listen)](
          audio_io::StereoRenderSource source,
          audio_io::TransportCursor& cursor) -> std::unique_ptr<application::PlaybackOutput> {
        return MeteredPlaybackOutput::create(source, cursor, downstream, meter, spectrum, true_peak,
                                             mono_listen);
      };
}

} // namespace oscillate::quick
