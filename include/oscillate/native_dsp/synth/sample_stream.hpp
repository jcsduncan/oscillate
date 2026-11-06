#pragma once

#include "oscillate/foundation/error.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The sample-streaming core shared by the sampled instruments (ADR-0071,
// ADR-0088): a raw int16 pack read with positioned reads, each sample's
// start kept resident, the rest streamed into per-voice rings by one
// immortal process-wide streamer thread, generation-tagged atomic words so
// the callback never waits, immortal per-instrument library registries and
// the windowed-sinc resampler.
namespace oscillate::native_dsp::synth::sample_stream {

// ---- Untrusted manifests ---------------------------------------------------------------

// The whole of `text` as a number; false for anything else.
template <class T> [[nodiscard]] bool parse_number(std::string_view text, T& out) {
  const auto* end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, out);
  return error == std::errc{} && last == end;
}

// A line's space-separated fields.
[[nodiscard]] std::vector<std::string_view> fields(std::string_view line);

// Reads exactly `bytes` at `offset` with positioned reads.
[[nodiscard]] bool read_exactly(int descriptor, void* buffer, std::size_t bytes,
                                std::uint64_t offset) noexcept;

// ---- The pack ----------------------------------------------------------------------------

inline constexpr std::uint32_t max_channels = 2;

// One sample's place in the pack, in frames.
struct Sample {
  std::uint64_t offset = 0; // Frames into the pack.
  std::uint32_t frames = 0;
  std::uint32_t resident = 0;  // Frames kept in memory from the start.
  std::size_t resident_at = 0; // First int16 in the resident store.
  float scale = 0.0F;          // int16 to full scale.
};

// What a manifest says of its pack.
struct PackSpec {
  std::filesystem::path path;
  std::uint64_t bytes = 0; // The pack's size: the file must match.
  std::uint32_t channels = 0;
  std::uint32_t rate = 0;
  std::uint64_t max_resident_bytes = 0;
  // Names the library in messages: "piano library".
  std::string_view noun;
};

// An open pack of interleaved little-endian int16 frames, its samples and
// their resident starts. Move-only; it owns the descriptor.
class Pack {
public:
  Pack() = default;
  Pack(Pack&& other) noexcept;
  Pack& operator=(Pack&& other) noexcept;
  Pack(const Pack&) = delete;
  Pack& operator=(const Pack&) = delete;
  ~Pack();

  // Opens the pack read-only and checks it against the manifest (untrusted
  // input): its size, each sample inside it, the resident total bounded.
  // Each sample's `resident` on entry is how many frames the engine wants
  // in memory (clamped to its length); then reads them.
  [[nodiscard]] static foundation::Result<Pack> open(const PackSpec& spec,
                                                     std::vector<Sample> samples);

  [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
  [[nodiscard]] std::size_t bytes_per_frame() const noexcept {
    return std::size_t{channels_} * sizeof(std::int16_t);
  }
  [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
  [[nodiscard]] std::span<const Sample> samples() const noexcept { return samples_; }
  [[nodiscard]] const Sample& sample(std::uint32_t index) const noexcept { return samples_[index]; }
  [[nodiscard]] const std::int16_t* resident(const Sample& sample) const noexcept {
    return resident_.data() + sample.resident_at;
  }
  [[nodiscard]] std::uint64_t resident_bytes() const noexcept {
    return resident_.size() * sizeof(std::int16_t);
  }
  // For the streamer's positioned reads.
  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
  std::vector<Sample> samples_;
  std::vector<std::int16_t> resident_;
  int descriptor_ = -1;
  std::uint32_t channels_ = 0;
  std::uint32_t rate_ = 0;
};

// ---- Libraries and their registries ----------------------------------------------------

enum class State : std::uint8_t { missing, invalid, ready };

// An engine's loaded library: its pack plus whatever the engine's manifest
// adds (the engine derives from this).
class Library {
public:
  Library() = default;
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  virtual ~Library() = default;

  [[nodiscard]] const Pack& pack() const noexcept { return pack_; }
  // Its slot in its registry (1..Registry::max_serials).
  [[nodiscard]] std::uint32_t serial() const noexcept { return serial_; }

protected:
  Pack pack_;

private:
  friend class Registry;
  std::uint32_t serial_ = 0;
};

// The result of looking for a library: a ready one, or why not.
struct Outcome {
  State state = State::missing;
  std::string message;
  std::filesystem::path directory;
  const Library* library = nullptr; // Set when ready.
};

// One instrument's loaded libraries, keyed by serial. Each engine creates
// one and never frees it: kernels may read a library until the process
// exits. current() and by_serial() are callback and streamer safe; the rest
// is control-thread only.
class Registry {
public:
  // A request word carries the serial in 8 bits.
  static constexpr std::uint32_t max_serials = 255;
  using Opener = foundation::Result<std::unique_ptr<Library>> (*)(const std::filesystem::path&);

  // `manifest_file` is watched for changes; `open` parses and loads a
  // library directory; `noun` names it in messages.
  Registry(std::string_view manifest_file, Opener open, std::string noun,
           std::uint32_t max_libraries);
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // The library new and idle kernels use; null while missing.
  [[nodiscard]] const Library* current() const noexcept {
    return current_.load(std::memory_order_acquire);
  }
  // A loaded library by serial (null for 0 or an unused serial).
  [[nodiscard]] const Library* by_serial(std::uint32_t serial) const noexcept;
  [[nodiscard]] std::uint32_t max_libraries() const noexcept { return max_libraries_; }

  // The ready library, or the last attempt's outcome. While none is ready
  // it looks in `directory` again whenever that or its manifest changed.
  [[nodiscard]] Outcome status(const std::filesystem::path& directory);
  // Loads `directory` and makes it current when it is ready.
  [[nodiscard]] Outcome load(const std::filesystem::path& directory);

private:
  Outcome attempt(const std::filesystem::path& directory);
  [[nodiscard]] bool unchanged(const std::filesystem::path& directory) const;

  std::string manifest_file_;
  Opener open_;
  std::string noun_;
  std::uint32_t max_libraries_;
  std::mutex mutex_;
  std::array<std::atomic<const Library*>, max_serials + 1> by_serial_{};
  std::atomic<const Library*> current_{nullptr};
  std::uint32_t next_serial_ = 1;
  bool attempted_ = false;
  Outcome outcome_;
  // What the last failed attempt saw, so an unchanged directory is not
  // parsed again on every call.
  std::filesystem::path tried_;
  bool tried_existed_ = false;
  std::filesystem::file_time_type tried_time_{};
};

// ---- Streaming ---------------------------------------------------------------------------

inline constexpr std::uint32_t ring_frames = 16'384; // A power of two: 0.34 s at 48 kHz.
inline constexpr std::uint32_t ring_mask = ring_frames - 1;
inline constexpr std::uint32_t read_frames = 4'096; // One positioned read.
inline constexpr std::uint32_t no_sample = 0xFF'FFFFU;
inline constexpr std::uint32_t serial_mask = 0xFFU;
inline constexpr unsigned serial_shift = 24U;
inline constexpr unsigned generation_shift = 32U;
inline constexpr std::uint64_t frames_mask = 0xFFFF'FFFFU;

// The request word: generation (32 bits), library serial (8), sample (24).
[[nodiscard]] constexpr std::uint64_t request_word(std::uint32_t generation, std::uint32_t serial,
                                                   std::uint32_t sample) noexcept {
  return (static_cast<std::uint64_t>(generation) << generation_shift) |
         (static_cast<std::uint64_t>(serial & serial_mask) << serial_shift) | (sample & no_sample);
}
[[nodiscard]] constexpr std::uint32_t serial_of(std::uint64_t request) noexcept {
  return static_cast<std::uint32_t>((request >> serial_shift) & serial_mask);
}
[[nodiscard]] constexpr std::uint32_t sample_of(std::uint64_t request) noexcept {
  return static_cast<std::uint32_t>(request & no_sample);
}
// Progress and consumption words: generation (32 bits), frames (32).
[[nodiscard]] constexpr std::uint64_t tagged(std::uint32_t generation,
                                             std::uint32_t frames) noexcept {
  return (static_cast<std::uint64_t>(generation) << generation_shift) | frames;
}
[[nodiscard]] constexpr std::uint32_t generation_of(std::uint64_t word) noexcept {
  return static_cast<std::uint32_t>(word >> generation_shift);
}
[[nodiscard]] constexpr std::uint32_t frames_of(std::uint64_t word) noexcept {
  return static_cast<std::uint32_t>(word & frames_mask);
}

// One voice's stream. Frames are counted from the end of the sample's
// resident part; the ring holds frame f at (f & ring_mask).
struct StreamSlot {
  // Callback writes, streamer reads: which sample to stream.
  std::atomic<std::uint64_t> request{request_word(0, 0, no_sample)};
  // Streamer writes, callback reads: frames written for a generation.
  std::atomic<std::uint64_t> progress{tagged(0, 0)};
  // Callback writes, streamer reads: frames the voice no longer needs.
  std::atomic<std::uint64_t> consumed{tagged(0, 0)};
  // Serviced under Streams' mutex only.
  std::uint32_t served = 0;
  std::uint32_t filled = 0;
  bool failed = false;
};

// A kernel's stream slots and rings, streaming from its instrument's
// registry's packs of `channels` channels. Created and destroyed on
// control; it registers with the process's streamer for its lifetime.
class Streams {
public:
  Streams(const Registry& registry, std::uint32_t channels, std::size_t slots);
  ~Streams();
  Streams(const Streams&) = delete;
  Streams& operator=(const Streams&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
  [[nodiscard]] StreamSlot& slot(std::size_t index) noexcept { return slots_[index]; }
  // ring_frames interleaved frames of channels() int16s.
  [[nodiscard]] const std::int16_t* ring(std::size_t index) const noexcept {
    return rings_.data() + index * ring_frames * channels_;
  }
  // Fills every slot's ring as far as its voice allows. Never on the
  // callback: the streamer thread calls it, and offline renders between
  // blocks (SynthKernel::await_content).
  void service() noexcept;
  // Streamer reads that failed (cumulative), including a pack whose
  // channel count is not this one's.
  [[nodiscard]] std::uint64_t failures() const noexcept {
    return failures_.load(std::memory_order_relaxed);
  }

private:
  void service_slot(std::size_t index) noexcept;

  const Registry& registry_;
  std::uint32_t channels_;
  std::mutex mutex_;
  std::unique_ptr<StreamSlot[]> slots_;
  std::size_t count_;
  std::vector<std::int16_t> rings_;
  std::atomic<std::uint64_t> failures_{0};
};

// Source frames [first, first + count) of a voice's sample as floats: the
// resident start, then its ring (`streamed` frames of it on hand; null for
// none). Frames outside the sample are silence. A mono pack writes `left`
// and copies it to `right`. True when a streamed frame had not arrived.
[[nodiscard]] bool gather(const Sample& sample, std::uint32_t channels,
                          const std::int16_t* resident, const std::int16_t* ring,
                          std::uint32_t streamed, std::int64_t first, std::size_t count,
                          float* left, float* right) noexcept;

// ---- Resampling --------------------------------------------------------------------------

// A Kaiser-windowed sinc: `taps` taps, `phases` phases linearly
// interpolated. Built off the callback; reading it is callback safe.
class SincTable {
public:
  static constexpr std::size_t taps = 32;
  // Taps before the frame read at: tap t reads frame base - left + t.
  static constexpr std::ptrdiff_t left = static_cast<std::ptrdiff_t>(taps / 2) - 1;
  static constexpr std::size_t phases = 256;
  static constexpr double kaiser_beta = 8.0;
  // The cutoff for reading at or below the recorded rate, relative to the
  // source's Nyquist frequency.
  static constexpr double passband = 0.97;

  // (phases + 1) rows of taps coefficients, each row summing to one; row r
  // reads at fraction r / phases past a source frame.
  explicit SincTable(double cutoff);

  // One channel at `fraction` (0..1) past x[left]; `x` points at the first
  // tap's frame.
  [[nodiscard]] float read(const float* x, double fraction) const noexcept {
    const double phase = fraction * static_cast<double>(phases);
    const auto row = static_cast<std::size_t>(phase);
    const auto blend = static_cast<float>(phase - static_cast<double>(row));
    const float* a = table_.data() + row * taps;
    const float* b = a + taps;
    float sum = 0.0F;
    for (std::size_t t = 0; t < taps; ++t) {
      const float c = a[t] + blend * (b[t] - a[t]);
      sum += c * x[t];
    }
    return sum;
  }

  // Two channels with the same coefficients.
  void read(const float* x_left, const float* x_right, double fraction, float& out_left,
            float& out_right) const noexcept {
    const double phase = fraction * static_cast<double>(phases);
    const auto row = static_cast<std::size_t>(phase);
    const auto blend = static_cast<float>(phase - static_cast<double>(row));
    const float* a = table_.data() + row * taps;
    const float* b = a + taps;
    float sum_left = 0.0F;
    float sum_right = 0.0F;
    for (std::size_t t = 0; t < taps; ++t) {
      const float c = a[t] + blend * (b[t] - a[t]);
      sum_left += c * x_left[t];
      sum_right += c * x_right[t];
    }
    out_left = sum_left;
    out_right = sum_right;
  }

  [[nodiscard]] std::span<const float> coefficients() const noexcept { return table_; }

private:
  std::vector<float> table_;
};

// ---- Library manifests and directories --------------------------------------------------

// Every instrument's manifest is a format line, then lines of a key and its
// value: the name, attribution, pack rate, channels and pack shared here,
// and sample lines (and anything else) that are the engine's own.

// Bounds every library shares on its untrusted manifest and samples.
inline constexpr std::uintmax_t max_manifest_bytes = 1U << 20U;
inline constexpr double min_level_db = -120.0;
inline constexpr double max_level_db = 24.0;

// A pack scale is a gain in (0, 1]; a level in dB lies in the shared bounds.
[[nodiscard]] inline bool valid_scale(double scale) noexcept {
  return std::isfinite(scale) && scale > 0.0 && scale <= 1.0;
}
[[nodiscard]] inline bool valid_level_db(double db) noexcept {
  return std::isfinite(db) && db >= min_level_db && db <= max_level_db;
}

// How one instrument's manifest reads.
struct ManifestFormat {
  std::string_view manifest_file;
  std::string_view format_line;
  std::string_view engine; // "Fret", for "Not a Fret library".
  std::string_view noun;   // "bass library", in messages.
  std::uint32_t rate = 0;  // The one pack rate the engine plays.
  std::uint32_t channels = 0;
  // Whether the manifest states the channel count (checked against
  // `channels`) or the engine fixes it.
  bool channels_line = true;
  // Keys the engine accepts and ignores.
  std::span<const std::string_view> ignored_keys{};
};

// What a manifest's shared lines said.
struct Manifest {
  std::string name;
  std::string attribution;
  std::uint32_t rate = 0;
  std::uint32_t channels = 0;
  std::uint64_t pack_bytes = 0;
  std::string pack_name;
};

// Reads one of the engine's own lines: false for a key it does not know;
// otherwise true, with a message in `error` when the line is bad.
using ManifestLineReader =
    std::function<bool(std::string_view key, std::string_view rest, std::string& error)>;

// Reads `directory`'s manifest: its shared lines here, the rest through
// `lines`. Fails not_found when there is no manifest and corrupt_data for a
// bad one, naming the library by the format's noun.
[[nodiscard]] foundation::Result<Manifest> read_manifest(const std::filesystem::path& directory,
                                                         const ManifestFormat& format,
                                                         const ManifestLineReader& lines);

// Opens the pack a manifest names, holding at most `max_resident_bytes`.
[[nodiscard]] foundation::Result<Pack> open_pack(const std::filesystem::path& directory,
                                                 const Manifest& manifest,
                                                 const ManifestFormat& format,
                                                 std::uint64_t max_resident_bytes,
                                                 std::vector<Sample> samples);

// Where an instrument's library is installed: `$variable` when set,
// otherwise `$XDG_DATA_HOME/<folder>` (~/.local/share when XDG_DATA_HOME is
// unset or not absolute), as the install scripts use; empty when neither
// is known.
[[nodiscard]] std::filesystem::path default_directory(std::string_view variable,
                                                      std::string_view folder);

// An engine's library State from the registry's.
template <class EngineState> [[nodiscard]] constexpr EngineState state_as(State state) noexcept {
  switch (state) {
  case State::ready:
    return EngineState::ready;
  case State::invalid:
    return EngineState::invalid;
  case State::missing:
    break;
  }
  return EngineState::missing;
}

// An engine's library Status from a registry outcome: the ready library's
// own, or why there is none.
template <class EngineLibrary, class EngineStatus>
[[nodiscard]] EngineStatus status_as(const Outcome& outcome) {
  if (outcome.library != nullptr) {
    return static_cast<const EngineLibrary*>(outcome.library)->status();
  }
  EngineStatus status;
  status.state = state_as<decltype(status.state)>(outcome.state);
  status.message = outcome.message;
  status.directory = outcome.directory;
  return status;
}

// A Registry::Opener for an engine's Library::open.
template <class EngineLibrary>
[[nodiscard]] foundation::Result<std::unique_ptr<Library>>
open_as(const std::filesystem::path& directory) {
  using R = foundation::Result<std::unique_ptr<Library>>;
  auto opened = EngineLibrary::open(directory);
  if (!opened) {
    return R::failure(std::move(opened.error()));
  }
  return R::success(std::move(opened).value());
}

} // namespace oscillate::native_dsp::synth::sample_stream
