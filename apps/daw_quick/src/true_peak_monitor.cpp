#include "true_peak_monitor.hpp"

#include "oscillate/native_dsp/correlation.hpp"
#include "oscillate/native_dsp/loudness.hpp"

#include <chrono>
#include <limits>
#include <vector>

namespace oscillate::quick {
namespace {
using namespace std::chrono_literals;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace

LiveTruePeakMonitor::LiveTruePeakMonitor()
    : worker_([this](const std::stop_token& stop) { run(stop); }) {}

void LiveTruePeakMonitor::begin_epoch(std::uint64_t generation, std::uint64_t sample,
                                      bool complete) noexcept {
  producer_generation_ = generation;
  expected_sample_ = sample;
  producer_complete_ = complete;
  ++producer_epoch_;
  incomplete_.store(generation != 0 && !complete, std::memory_order_release);
  published_epoch_.store(producer_epoch_, std::memory_order_release);
}

void LiveTruePeakMonitor::set_sample_rate_after_quiesce(std::uint32_t sample_rate) noexcept {
  invalidate_after_quiesce();
  sample_rate_.store(sample_rate, std::memory_order_release);
}

void LiveTruePeakMonitor::invalidate_after_quiesce() noexcept { begin_epoch(0, 0, false); }

void LiveTruePeakMonitor::observe(std::span<const engine::StereoFrame> frames,
                                  engine::BlockContext context) noexcept {
  if (context.transport_generation == 0 ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    begin_epoch(context.transport_generation, context.project_sample_position, false);
    return;
  }
  if (frames.size() > max_callback_frames) {
    dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    begin_epoch(context.transport_generation, context.project_sample_position + frames.size(),
                false);
    return;
  }
  if (producer_generation_ != context.transport_generation) {
    begin_epoch(context.transport_generation, context.project_sample_position, true);
  } else if (expected_sample_ != context.project_sample_position) {
    // A gap without a new transport generation cannot restore a complete
    // peak hold for this transport, even if subsequent blocks are accepted.
    begin_epoch(context.transport_generation, context.project_sample_position, false);
  }

  const auto write = write_sequence_.load(std::memory_order_relaxed);
  const auto read = read_sequence_.load(std::memory_order_acquire);
  if (write - read >= queue_capacity) {
    dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    begin_epoch(context.transport_generation, context.project_sample_position + frames.size(),
                false);
    return;
  }

  auto& block = queue_[write % queue_capacity];
  for (std::size_t index = 0; index < frames.size(); ++index)
    block.frames[index] = frames[index];
  block.count = frames.size();
  block.end_sample = context.project_sample_position + frames.size();
  block.generation = context.transport_generation;
  block.epoch = producer_epoch_;
  block.complete = producer_complete_;
  expected_sample_ = block.end_sample;
  write_sequence_.store(write + 1, std::memory_order_release);
}

std::optional<LiveTruePeakReading> LiveTruePeakMonitor::latest() const {
  std::lock_guard lock(latest_mutex_);
  if (!latest_ || latest_->epoch != published_epoch_.load(std::memory_order_acquire))
    return std::nullopt;
  return latest_;
}

void LiveTruePeakMonitor::run(const std::stop_token& stop) noexcept {
  native_dsp::StereoTruePeakMeter meter;
  native_dsp::StereoCorrelationMeter correlation;
  std::optional<native_dsp::IntegratedLoudnessAnalyzer> loudness;
  native_dsp::LoudnessResult measured;
  std::vector<float> interleaved(max_callback_frames * 2);
  // Gating every block is wasteful; a reading moves every 100 ms anyway.
  std::uint64_t frames_since_loudness = 0;
  std::uint64_t loudness_step = 4'800;
  std::uint64_t seen_reset = loudness_reset_.load(std::memory_order_acquire);
  std::uint64_t active_epoch = 0;
  while (!stop.stop_requested()) {
    const auto read = read_sequence_.load(std::memory_order_relaxed);
    if (read == write_sequence_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    const auto& block = queue_[read % queue_capacity];
    const auto current_epoch = published_epoch_.load(std::memory_order_acquire);
    if (block.epoch == current_epoch) {
      if (active_epoch != block.epoch) {
        active_epoch = block.epoch;
        const auto rate = sample_rate_.load(std::memory_order_acquire);
        if (!meter.prepare(rate)) {
          read_sequence_.store(read + 1, std::memory_order_release);
          continue;
        }
        correlation.prepare(static_cast<double>(rate));
        // Four hours of 100 ms blocks (about 1 MB), so a long DJ mix keeps
        // its integrated reading to the end.
        constexpr std::size_t four_hours = 144'000;
        loudness.emplace(rate, native_dsp::LoudnessLayout::stereo, four_hours);
        measured = {};
        frames_since_loudness = 0;
        loudness_step = rate / 10;
      }
      const auto frames = std::span{block.frames}.first(block.count);
      if (const auto reset = loudness_reset_.load(std::memory_order_acquire);
          loudness && reset != seen_reset) {
        seen_reset = reset;
        loudness->reset();
        measured = {};
        frames_since_loudness = 0;
      }
      for (std::size_t index = 0; index < frames.size(); ++index) {
        interleaved[index * 2] = frames[index].left;
        interleaved[index * 2 + 1] = frames[index].right;
      }
      // A rate the analyser does not support leaves loudness absent; the
      // true peak and correlation still read.
      if (loudness && loudness->status() == native_dsp::LoudnessStatus::ok &&
          loudness->append(std::span{interleaved}.first(frames.size() * 2)) ==
              native_dsp::LoudnessStatus::ok) {
        frames_since_loudness += frames.size();
        if (frames_since_loudness >= loudness_step) {
          frames_since_loudness = 0;
          measured = loudness->finish();
        }
      }
      correlation.process(frames);
      if (meter.process(frames) &&
          block.epoch == published_epoch_.load(std::memory_order_acquire)) {
        std::lock_guard lock(latest_mutex_);
        if (block.epoch == published_epoch_.load(std::memory_order_acquire)) {
          latest_ = LiveTruePeakReading{meter.reading(),
                                        block.end_sample,
                                        block.generation,
                                        block.epoch,
                                        block.complete,
                                        measured.momentary_lkfs,
                                        measured.short_term_lkfs,
                                        measured.integrated_lkfs,
                                        correlation.correlation(),
                                        correlation.mono_loss_db()};
        }
      }
    }
    read_sequence_.store(read + 1, std::memory_order_release);
  }
}

} // namespace oscillate::quick
