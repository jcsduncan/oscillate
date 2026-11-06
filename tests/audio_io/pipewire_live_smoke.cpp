#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/audio_io/pipewire_sinks.hpp>

#include <oscillate/engine/graph.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

namespace {
bool silence(void* context, std::span<oscillate::engine::StereoFrame> stereo,
             oscillate::engine::BlockContext block) noexcept {
  static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
  std::ranges::fill(stereo, oscillate::engine::StereoFrame{0.0F, 0.0F});
  return block.transport_generation != 0;
}

bool exercise(std::uint32_t requested_frames,
              std::optional<std::uint64_t> target_serial = std::nullopt) {
  std::atomic<std::uint64_t> callback_count{0};
  std::atomic<bool> transport_allowed{false};
  oscillate::audio_io::TransportCursor transport;
  const oscillate::audio_io::TransportPermission permission{
      &transport_allowed, [](const void* context) noexcept {
        return static_cast<const std::atomic<bool>*>(context)->load(std::memory_order_acquire);
      }};
  oscillate::audio_io::PipeWireOutput output({&callback_count, &silence}, transport, permission);
  const auto started = output.start(48000, requested_frames, target_serial);
  if (!started) {
    std::fprintf(stderr, "PipeWire start failed: %s\n", started.error().message.c_str());
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  oscillate::audio_io::OutputStatus status;
  do {
    status = output.status();
    if (status.state == oscillate::audio_io::OutputState::error ||
        status.state == oscillate::audio_io::OutputState::disconnected) {
      break;
    }
    if (status.state == oscillate::audio_io::OutputState::streaming &&
        status.negotiated_client_rate && status.observed_graph_rate &&
        status.observed_block_frames) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);

  std::printf("request=%u state=%u client=%u graph=%u frames=%u render_failures=%llu "
              "invalid_buffers=%llu\n",
              requested_frames, static_cast<unsigned>(status.state),
              status.negotiated_client_rate.value_or(0), status.observed_graph_rate.value_or(0),
              status.observed_block_frames.value_or(0),
              static_cast<unsigned long long>(status.render_failures),
              static_cast<unsigned long long>(status.invalid_buffers));
  const bool stayed_paused =
      callback_count.load(std::memory_order_relaxed) == 0 && status.render_failures == 0;
  transport_allowed.store(true, std::memory_order_release);
  const auto resume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (callback_count.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < resume_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto played = output.status();
  std::printf("request=%u graph_xrun_recovery_events=", requested_frames);
  if (played.graph_xrun_recovery_events)
    std::printf("%llu", static_cast<unsigned long long>(*played.graph_xrun_recovery_events));
  else
    std::printf("unavailable");
  std::printf(" stream_xruns=%s\n", played.xruns ? "reported" : "unavailable");
  const bool valid =
      stayed_paused && callback_count.load(std::memory_order_relaxed) != 0 &&
      status.state == oscillate::audio_io::OutputState::streaming &&
      status.selected_target_serial == target_serial && status.negotiated_client_rate == 48000 &&
      status.observed_graph_rate == 48000 && status.observed_block_frames == requested_frames &&
      status.invalid_buffers == 0;
  output.stop();
  const auto stopped = output.status();
  const auto stopped_count = callback_count.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return valid && stopped.state == oscillate::audio_io::OutputState::stopped &&
         !stopped.negotiated_client_rate && !stopped.observed_graph_rate &&
         !stopped.observed_block_frames &&
         callback_count.load(std::memory_order_relaxed) == stopped_count;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--target-serial") {
      std::uint64_t serial = 0;
      const std::string_view value(argv[2]);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), serial);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || serial == 0) {
        std::fputs("Invalid target serial\n", stderr);
        return 2;
      }
      return exercise(64, serial) && exercise(128, serial) ? 0 : 1;
    }
    if (argc != 1) {
      std::fputs("Usage: oscillate_audio_io_live_smoke [--target-serial SERIAL]\n", stderr);
      return 2;
    }
    const auto sinks = oscillate::audio_io::list_pipewire_sinks();
    if (!sinks) {
      std::fprintf(stderr, "PipeWire sink discovery failed: %s\n", sinks.error().message.c_str());
      return 1;
    }
    std::printf("PipeWire sink discovery found %zu outputs\n", sinks.value().size());
    if (exercise(64) && exercise(128) &&
        (sinks.value().empty() || exercise(128, sinks.value().front().serial))) {
      return 0;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "PipeWire live smoke raised an exception: %s\n", error.what());
  } catch (...) {
    std::fputs("PipeWire live smoke raised an unknown exception\n", stderr);
  }
  std::fputs("PipeWire 64/128 live smoke failed\n", stderr);
  return 1;
}
