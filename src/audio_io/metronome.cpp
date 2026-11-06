#include "oscillate/audio_io/metronome.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

namespace oscillate::audio_io {

namespace {
constexpr std::size_t stereo = 2;
} // namespace

std::uint64_t click_frames(std::uint32_t sample_rate) noexcept {
  return static_cast<std::uint64_t>(std::llround(click_seconds * sample_rate));
}

float click_sample(std::uint64_t frame, bool accent, std::uint32_t sample_rate) noexcept {
  if (sample_rate == 0 || frame >= click_frames(sample_rate))
    return 0.0F;
  const double t = static_cast<double>(frame) / sample_rate;
  const double hz = accent ? accent_click_hz : click_hz;
  const double gain = accent ? accent_click_gain : click_gain;
  return static_cast<float>(gain * std::exp(-t / click_decay_seconds) *
                            std::sin(2.0 * std::numbers::pi * hz * t));
}

Metronome::Metronome() {
  for (auto& schedule : schedules_)
    schedule.beats.resize(max_click_beats);
}

void Metronome::publish(std::span<const ClickBeat> beats, std::uint32_t sample_rate) {
  const auto target = 1U - active_.load(std::memory_order_acquire);
  // The callback may still be finishing a block on this buffer from before
  // the last flip; it never holds one longer than a block.
  while (reading_.load(std::memory_order_seq_cst) == target + 1U)
    std::this_thread::yield();
  auto& schedule = schedules_[target];
  schedule.count = std::min(beats.size(), max_click_beats);
  std::copy_n(beats.begin(), schedule.count, schedule.beats.begin());
  schedule.sample_rate = sample_rate;
  active_.store(target, std::memory_order_seq_cst);
}

void Metronome::request_count_in(std::uint32_t beats, std::uint64_t beat_frames) noexcept {
  count_in_beats_.store(beats, std::memory_order_relaxed);
  count_in_beat_frames_.store(beat_frames, std::memory_order_relaxed);
  counting_in_.store(beats > 0 && beat_frames > 0, std::memory_order_relaxed);
  count_in_sequence_.fetch_add(1, std::memory_order_release);
}

void Metronome::cancel_count_in() noexcept { request_count_in(0, 0); }

bool Metronome::counting_in() const noexcept {
  return counting_in_.load(std::memory_order_acquire);
}

std::size_t Metronome::render_count_in(std::span<float> output) noexcept {
  const auto sequence = count_in_sequence_.load(std::memory_order_acquire);
  if (sequence != adopted_sequence_) {
    adopted_sequence_ = sequence;
    count_in_beat_ = count_in_beat_frames_.load(std::memory_order_relaxed);
    count_in_total_ = count_in_beats_.load(std::memory_order_relaxed) * count_in_beat_;
    count_in_position_ = 0;
  }
  if (count_in_position_ >= count_in_total_)
    return 0;
  const auto rate = schedules_[active_.load(std::memory_order_acquire)].sample_rate;
  const auto frames = static_cast<std::size_t>(
      std::min<std::uint64_t>(output.size() / stereo, count_in_total_ - count_in_position_));
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto at = count_in_position_ + frame;
    const auto beat = at / count_in_beat_;
    const auto value = click_sample(at - beat * count_in_beat_, beat == 0, rate);
    output[frame * stereo] = value;
    output[frame * stereo + 1] = value;
  }
  count_in_position_ += frames;
  if (count_in_position_ >= count_in_total_ &&
      count_in_sequence_.load(std::memory_order_acquire) == adopted_sequence_)
    counting_in_.store(false, std::memory_order_release);
  return frames;
}

void Metronome::add_clicks(std::uint64_t first_sample, std::span<float> output) noexcept {
  if (!enabled())
    return;
  const auto index = active_.load(std::memory_order_seq_cst);
  reading_.store(index + 1U, std::memory_order_seq_cst);
  if (active_.load(std::memory_order_seq_cst) == index) {
    const auto& schedule = schedules_[index];
    const auto length = click_frames(schedule.sample_rate);
    const auto frames = output.size() / stereo;
    const auto end = first_sample + frames;
    const auto earliest = first_sample > length ? first_sample - length : 0;
    const auto begin = schedule.beats.begin();
    auto beat =
        std::lower_bound(begin, begin + static_cast<std::ptrdiff_t>(schedule.count), earliest + 1,
                         [](const ClickBeat& b, std::uint64_t s) { return b.sample < s; });
    if (earliest == 0)
      beat = begin;
    for (; beat != begin + static_cast<std::ptrdiff_t>(schedule.count) && beat->sample < end;
         ++beat) {
      const auto from = beat->sample > first_sample ? beat->sample - first_sample : 0;
      for (auto frame = from; frame < frames; ++frame) {
        const auto since = first_sample + frame - beat->sample;
        if (since >= length)
          break;
        const auto value = click_sample(since, beat->accent, schedule.sample_rate);
        output[frame * stereo] += value;
        output[frame * stereo + 1] += value;
      }
    }
  }
  reading_.store(0, std::memory_order_release);
}

} // namespace oscillate::audio_io
