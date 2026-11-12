#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oscillate/audio_io/callback_timing.hpp>
#include <oscillate/audio_io/device_contract.hpp>
#include <oscillate/audio_io/device_reservation.hpp>
#include <oscillate/audio_io/stereo_block.hpp>
#include <oscillate/foundation/error.hpp>

namespace oscillate::audio_io {

// Direct ALSA playback (ADR-0130): the output opens a PCM itself, so a card
// it takes is out of the sound server's graph while it plays.

// A playback PCM of an ALSA card, for the device picker.
struct AlsaDevice {
  std::string id;   // "hw:CARD=<card id>,DEV=<device>"
  std::string name; // "<card name>: <PCM name>"
  int card = -1;
  int device = -1;
};

// Every card's playback PCMs. Reads the control interface only, so it works
// while the sound server holds the cards. Control thread.
[[nodiscard]] foundation::Result<std::vector<AlsaDevice>> list_alsa_playback_devices();

// The card a PCM name addresses, for its reservation: "hw:N", "hw:N,D",
// "hw:CARD=<id or N>,DEV=D" and the plughw forms. Empty for any other PCM
// (null, file, a plug of several cards), which is opened without reserving.
[[nodiscard]] std::optional<int> alsa_card_of(std::string_view pcm);

// The sample formats the output writes, in the order it prefers them.
enum class AlsaSampleFormat : std::uint8_t { s32_le, s24_3le, s16_le };
inline constexpr std::array<AlsaSampleFormat, 3> alsa_preferred_formats{
    AlsaSampleFormat::s32_le, AlsaSampleFormat::s24_3le, AlsaSampleFormat::s16_le};
[[nodiscard]] std::size_t alsa_sample_bytes(AlsaSampleFormat format) noexcept;
[[nodiscard]] std::string_view alsa_format_name(AlsaSampleFormat format) noexcept;

// Writes interleaved stereo float frames as `channels`-channel frames in
// `format`: left and right on the first two channels, silence on the rest.
// Samples are clipped to [-1, 1] and rounded to the nearest step. `device`
// holds frames * channels * alsa_sample_bytes(format) bytes. No allocation.
void convert_stereo_to_device(std::span<const float> stereo, AlsaSampleFormat format,
                              std::uint32_t channels, std::span<std::byte> device) noexcept;

inline constexpr std::uint32_t alsa_default_periods = 2;
inline constexpr std::uint32_t alsa_min_periods = 2;
inline constexpr std::uint32_t alsa_max_periods = 4;

struct AlsaStartConfig {
  std::string device;
  std::uint32_t project_rate = 0;
  std::uint32_t period_frames = 0;
  std::uint32_t periods = alsa_default_periods;
  // Empty: the first of alsa_preferred_formats the device takes.
  std::optional<AlsaSampleFormat> format;
  // Empty: the fewest channels the device takes, at least two.
  std::optional<std::uint32_t> channels;
};

struct AlsaOutputStatus {
  OutputState state = OutputState::stopped;
  std::string device;
  std::uint32_t rate = 0;
  std::uint32_t period_frames = 0;
  std::uint32_t periods = 0;
  std::uint32_t buffer_frames = 0;
  // The blocks the renderer is asked for: the period, split into blocks no
  // longer than the requested period when the device chose a longer one.
  std::uint32_t render_frames = 0;
  std::uint32_t channels = 0;
  std::optional<AlsaSampleFormat> format;
  // The card's reservation was taken over D-Bus (false: no session bus, or
  // a PCM that is not a card).
  bool reserved = false;
  // The I/O thread runs at real-time priority.
  bool realtime = false;
  std::size_t realtime_workers = 0;
  // Underruns the device reported, each recovered with snd_pcm_recover.
  std::uint64_t xruns = 0;
  // Frames between the next frame written and the DAC, as the driver last
  // reported it (snd_pcm_delay): the measured output latency.
  std::optional<std::uint32_t> delay_frames;
  std::uint64_t render_failures = 0;
  std::optional<std::uint64_t> measured_callback_blocks;
  std::optional<std::uint64_t> callback_over_period_blocks;
  std::optional<std::uint64_t> max_callback_duration_ns;
  std::optional<CallbackTimingDistribution> callback_timing_distribution;
  std::optional<CallbackLoad> callback_load;
};

// Fault injection for tests: called on the I/O thread before each period is
// written; a negative errno is handled as if the write had returned it, and
// the period is not written. Null in production.
struct AlsaFaults {
  void* context = nullptr;
  int (*before_write)(void* context, std::uint64_t period) noexcept = nullptr;
};

// Control-thread lifecycle, like the PipeWire and JACK outputs: start()
// reserves the card, opens and configures the PCM and starts the I/O thread
// at real-time priority; stop() joins it, closes the PCM and releases the
// card. The render source and transport must outlive the output.
class AlsaOutput {
public:
  // `reservation` null: the session bus (DbusDeviceReservation).
  AlsaOutput(StereoRenderSource source, TransportCursor& transport,
             TransportPermission permission = {},
             std::shared_ptr<DeviceReservation> reservation = nullptr,
             const AlsaFaults* faults = nullptr);
  AlsaOutput(const AlsaOutput&) = delete;
  AlsaOutput& operator=(const AlsaOutput&) = delete;
  ~AlsaOutput();

  [[nodiscard]] foundation::Result<void> start(AlsaStartConfig config);
  void stop() noexcept;
  [[nodiscard]] AlsaOutputStatus status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oscillate::audio_io
