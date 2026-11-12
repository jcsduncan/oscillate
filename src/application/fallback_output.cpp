#include "oscillate/application/fallback_output.hpp"

#include "oscillate/foundation/log.hpp"

#include <atomic>
#include <utility>

namespace oscillate::application {

void OutputFallbackReport::record(std::string reason) {
  const std::lock_guard lock(mutex_);
  reason_ = std::move(reason);
}

void OutputFallbackReport::clear() {
  const std::lock_guard lock(mutex_);
  reason_.reset();
}

std::optional<std::string> OutputFallbackReport::reason() const {
  const std::lock_guard lock(mutex_);
  return reason_;
}

namespace {

class FallbackOutput final : public PlaybackOutput {
public:
  FallbackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor,
                 PlaybackOutputFactory primary, PlaybackOutputFactory fallback,
                 std::shared_ptr<OutputFallbackReport> report)
      : source_(source), cursor_(cursor), primary_factory_(std::move(primary)),
        fallback_factory_(std::move(fallback)), report_(std::move(report)) {}
  FallbackOutput(const FallbackOutput&) = delete;
  FallbackOutput& operator=(const FallbackOutput&) = delete;
  ~FallbackOutput() override { FallbackOutput::stop(); }

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  foundation::Result<void> start(std::uint32_t rate, std::uint32_t frames,
                                 std::optional<std::uint64_t> target_serial) override {
    if (!primary_ && primary_factory_)
      primary_ = primary_factory_(source_, cursor_);
    if (primary_) {
      auto started = primary_->start(rate, frames, target_serial);
      if (started) {
        active_.store(primary_.get(), std::memory_order_release);
        if (report_)
          report_->clear();
        return started;
      }
      failure_ = started.error().message;
    } else {
      failure_ = "the output is unavailable in this build";
    }
    if (!fallback_ && fallback_factory_)
      fallback_ = fallback_factory_(source_, cursor_);
    if (!fallback_)
      return foundation::Result<void>::failure({foundation::ErrorCode::unavailable, failure_});
    // The fallback plays to its own default device.
    auto fallen = fallback_->start(rate, frames, std::nullopt);
    if (!fallen)
      return foundation::Result<void>::failure(
          {fallen.error().code, failure_ + "; the fallback failed too: " + fallen.error().message});
    active_.store(fallback_.get(), std::memory_order_release);
    foundation::log::warning(foundation::DiagnosticCategory::device,
                             "Playing through the fallback output: {}", failure_);
    if (report_)
      report_->record(failure_);
    return fallen;
  }

  void stop() noexcept override {
    // The output stays alive, so a status() that read it still may call it.
    if (auto* active = active_.exchange(nullptr, std::memory_order_acq_rel))
      active->stop();
  }

  // Any thread, also while another starts or stops: only the atomic is
  // read, and both outputs live as long as this one.
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    auto* active = active_.load(std::memory_order_acquire);
    return active != nullptr ? active->status() : audio_io::OutputStatus{};
  }

private:
  audio_io::StereoRenderSource source_;
  audio_io::TransportCursor& cursor_;
  PlaybackOutputFactory primary_factory_;
  PlaybackOutputFactory fallback_factory_;
  std::shared_ptr<OutputFallbackReport> report_;
  std::unique_ptr<PlaybackOutput> primary_;
  std::unique_ptr<PlaybackOutput> fallback_;
  std::atomic<PlaybackOutput*> active_{nullptr};
  std::string failure_;
};

} // namespace

PlaybackOutputFactory make_fallback_output_factory(PlaybackOutputFactory primary,
                                                   PlaybackOutputFactory fallback,
                                                   std::shared_ptr<OutputFallbackReport> report) {
  return [primary = std::move(primary), fallback = std::move(fallback), report = std::move(report)](
             audio_io::StereoRenderSource source,
             audio_io::TransportCursor& cursor) -> std::unique_ptr<PlaybackOutput> {
    return std::make_unique<FallbackOutput>(source, cursor, primary, fallback, report);
  };
}

} // namespace oscillate::application
