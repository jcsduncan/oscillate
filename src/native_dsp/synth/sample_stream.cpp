#include "oscillate/native_dsp/synth/sample_stream.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <numbers>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace oscillate::native_dsp::synth::sample_stream {
namespace {

using foundation::ErrorCode;
using foundation::Result;

// The streamer looks at every registered kernel this often.
constexpr auto stream_poll = std::chrono::milliseconds(2);
// Kaiser window: the Bessel series stops past this many terms or below this
// relative size; a sinc argument this small is taken as zero.
constexpr int bessel_terms = 64;
constexpr double bessel_tolerance = 1e-14;
constexpr double sinc_zero = 1e-12;

static_assert(std::endian::native == std::endian::little,
              "packs are little-endian int16; add a byte swap for this platform");

[[nodiscard]] double bessel_i0(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  for (int k = 1; k < bessel_terms && term > bessel_tolerance * sum; ++k) {
    const double half = x / (2.0 * static_cast<double>(k));
    term *= half * half;
    sum += term;
  }
  return sum;
}

template <std::uint32_t Channels>
bool gather_frames(const Sample& sample, const std::int16_t* resident, const std::int16_t* ring,
                   std::uint32_t streamed, std::int64_t first, std::size_t count, float* left,
                   float* right) noexcept {
  const float scale = sample.scale;
  bool starved = false;
  for (std::size_t j = 0; j < count; ++j) {
    const std::int64_t i = first + static_cast<std::int64_t>(j);
    float l = 0.0F;
    float r = 0.0F;
    if (i >= 0 && i < static_cast<std::int64_t>(sample.frames)) {
      if (i < static_cast<std::int64_t>(sample.resident)) {
        const auto at = static_cast<std::size_t>(i) * Channels;
        l = static_cast<float>(resident[at]) * scale;
        r = Channels == 1 ? l : static_cast<float>(resident[at + Channels - 1]) * scale;
      } else {
        const auto f = static_cast<std::uint32_t>(i - static_cast<std::int64_t>(sample.resident));
        if (ring != nullptr && f < streamed) {
          const auto at = static_cast<std::size_t>(f & ring_mask) * Channels;
          l = static_cast<float>(ring[at]) * scale;
          r = Channels == 1 ? l : static_cast<float>(ring[at + Channels - 1]) * scale;
        } else {
          starved = true;
        }
      }
    }
    left[j] = l;
    right[j] = r;
  }
  return starved;
}

// One thread services every kernel's streams, whatever their instrument.
// Immortal, like the libraries.
class Streamer {
public:
  static Streamer& instance() {
    static auto* streamer = new Streamer();
    return *streamer;
  }

  void add(Streams* streams) {
    std::lock_guard lock(mutex_);
    streams_.push_back(streams);
    if (!started_) {
      std::thread([this] { run(); }).detach();
      started_ = true;
    }
    wake_.notify_one();
  }

  // Returns once no service pass can still be using `streams`.
  void remove(Streams* streams) {
    std::lock_guard lock(mutex_);
    std::erase(streams_, streams);
  }

private:
  Streamer() = default;

  [[noreturn]] void run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return !streams_.empty(); });
      for (auto* streams : streams_) {
        streams->service();
      }
      lock.unlock();
      std::this_thread::sleep_for(stream_poll);
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Streams*> streams_;
  bool started_ = false;
};

} // namespace

// ---- Untrusted manifests ---------------------------------------------------------------

std::vector<std::string_view> fields(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && line[at] == ' ') {
      ++at;
    }
    const std::size_t start = at;
    while (at < line.size() && line[at] != ' ') {
      ++at;
    }
    if (at > start) {
      out.push_back(line.substr(start, at - start));
    }
  }
  return out;
}

bool read_exactly(int descriptor, void* buffer, std::size_t bytes, std::uint64_t offset) noexcept {
  auto* out = static_cast<char*>(buffer);
  while (bytes > 0) {
    const auto got = ::pread(descriptor, out, bytes, static_cast<off_t>(offset));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      return false;
    }
    out += got;
    bytes -= static_cast<std::size_t>(got);
    offset += static_cast<std::uint64_t>(got);
  }
  return true;
}

// ---- Library manifests and directories --------------------------------------------------

Result<Manifest> read_manifest(const std::filesystem::path& directory, const ManifestFormat& format,
                               const ManifestLineReader& lines) {
  using R = Result<Manifest>;
  const std::string noun(format.noun);
  const auto fail = [](std::string message) {
    return R::failure({ErrorCode::corrupt_data, std::move(message)});
  };
  std::error_code error;
  const auto manifest_path = directory / format.manifest_file;
  const auto manifest_size = std::filesystem::file_size(manifest_path, error);
  if (error) {
    return R::failure({ErrorCode::not_found, "No " + noun + " in " + directory.string()});
  }
  if (manifest_size > max_manifest_bytes) {
    return fail("The " + noun + "'s manifest is too large");
  }
  std::ifstream input(manifest_path);
  if (!input) {
    return R::failure({ErrorCode::not_found, "Cannot read " + manifest_path.string()});
  }
  std::string line;
  if (!std::getline(input, line) || line != format.format_line) {
    return fail("Not a " + std::string(format.engine) +
                " library, or made by another version of the install script");
  }
  Manifest manifest;
  if (!format.channels_line) {
    manifest.channels = format.channels;
  }
  std::string own_error;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto space = line.find(' ');
    const std::string_view key = std::string_view(line).substr(0, space);
    const std::string_view rest =
        space == std::string::npos ? std::string_view{} : std::string_view(line).substr(space + 1);
    if (key == "name") {
      manifest.name = rest;
    } else if (key == "attribution") {
      manifest.attribution = rest;
    } else if (std::ranges::find(format.ignored_keys, key) != format.ignored_keys.end()) {
      continue;
    } else if (key == "rate") {
      if (!parse_number(rest, manifest.rate) || manifest.rate != format.rate) {
        return fail("The " + noun + "'s sample rate is not supported");
      }
    } else if (key == "channels" && format.channels_line) {
      if (!parse_number(rest, manifest.channels) || manifest.channels != format.channels) {
        return fail("The " + noun + "'s channel count is not supported");
      }
    } else if (key == "pack") {
      const auto parts = fields(rest);
      if (parts.size() != 2 || !parse_number(parts[1], manifest.pack_bytes) ||
          parts[0].find('/') != std::string_view::npos || parts[0] == "." || parts[0] == "..") {
        return fail("The " + noun + " names its pack badly");
      }
      manifest.pack_name = parts[0];
    } else if (lines(key, rest, own_error)) {
      if (!own_error.empty()) {
        return fail(std::move(own_error));
      }
    } else {
      return fail("The " + noun + "'s manifest has an unknown line");
    }
  }
  if (manifest.rate == 0 || manifest.channels == 0 || manifest.pack_name.empty()) {
    return fail("The " + noun + "'s manifest is incomplete");
  }
  return R::success(std::move(manifest));
}

Result<Pack> open_pack(const std::filesystem::path& directory, const Manifest& manifest,
                       const ManifestFormat& format, std::uint64_t max_resident_bytes,
                       std::vector<Sample> samples) {
  PackSpec spec;
  spec.path = directory / manifest.pack_name;
  spec.bytes = manifest.pack_bytes;
  spec.channels = manifest.channels;
  spec.rate = manifest.rate;
  spec.max_resident_bytes = max_resident_bytes;
  spec.noun = format.noun;
  return Pack::open(spec, std::move(samples));
}

std::filesystem::path default_directory(std::string_view variable, std::string_view folder) {
  if (const char* override = std::getenv(std::string(variable).c_str());
      override != nullptr && override[0] != '\0') {
    return override;
  }
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
    return std::filesystem::path(data) / folder;
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
    return std::filesystem::path(home) / ".local" / "share" / folder;
  }
  return {};
}

// ---- The pack ----------------------------------------------------------------------------

Pack::Pack(Pack&& other) noexcept
    : samples_(std::move(other.samples_)), resident_(std::move(other.resident_)),
      descriptor_(std::exchange(other.descriptor_, -1)), channels_(other.channels_),
      rate_(other.rate_) {}

Pack& Pack::operator=(Pack&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    samples_ = std::move(other.samples_);
    resident_ = std::move(other.resident_);
    descriptor_ = std::exchange(other.descriptor_, -1);
    channels_ = other.channels_;
    rate_ = other.rate_;
  }
  return *this;
}

Pack::~Pack() {
  if (descriptor_ >= 0) {
    ::close(descriptor_);
  }
}

Result<Pack> Pack::open(const PackSpec& spec, std::vector<Sample> samples) {
  using R = Result<Pack>;
  const std::string noun(spec.noun);
  const auto fail = [](std::string message) {
    return R::failure({ErrorCode::corrupt_data, std::move(message)});
  };
  if (spec.channels == 0 || spec.channels > max_channels) {
    return fail("The " + noun + "'s channel count is not supported");
  }
  Pack pack;
  pack.channels_ = spec.channels;
  pack.rate_ = spec.rate;
  pack.descriptor_ = ::open(spec.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (pack.descriptor_ < 0) {
    return R::failure({ErrorCode::not_found, "Cannot open " + spec.path.string()});
  }
  struct stat info{};
  if (::fstat(pack.descriptor_, &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<std::uint64_t>(info.st_size) != spec.bytes) {
    return fail("The " + noun + "'s pack is incomplete: reinstall it");
  }
  const std::size_t bytes_per_frame = pack.bytes_per_frame();
  const std::uint64_t pack_frames = spec.bytes / bytes_per_frame;

  std::uint64_t resident_total = 0;
  for (auto& sample : samples) {
    if (sample.offset > pack_frames || sample.frames > pack_frames - sample.offset) {
      return fail("A " + noun + " sample lies outside its pack");
    }
    sample.resident = std::min(sample.frames, sample.resident);
    sample.resident_at = static_cast<std::size_t>(resident_total) * spec.channels;
    resident_total += sample.resident;
    if (resident_total * bytes_per_frame > spec.max_resident_bytes) {
      return fail("The " + noun + " is too large to keep its starts in memory");
    }
  }

  pack.resident_.resize(static_cast<std::size_t>(resident_total) * spec.channels);
  for (const auto& sample : samples) {
    if (!read_exactly(pack.descriptor_, pack.resident_.data() + sample.resident_at,
                      std::size_t{sample.resident} * bytes_per_frame,
                      sample.offset * bytes_per_frame)) {
      return R::failure({ErrorCode::io_failure, "Cannot read the " + noun + "'s pack"});
    }
  }
  pack.samples_ = std::move(samples);
  return R::success(std::move(pack));
}

// ---- Registries --------------------------------------------------------------------------

Registry::Registry(std::string_view manifest_file, Opener open, std::string noun,
                   std::uint32_t max_libraries)
    : manifest_file_(manifest_file), open_(open), noun_(std::move(noun)),
      max_libraries_(std::min(max_libraries, max_serials)) {}

const Library* Registry::by_serial(std::uint32_t serial) const noexcept {
  if (serial == 0 || serial > max_libraries_) {
    return nullptr;
  }
  return by_serial_[serial].load(std::memory_order_acquire);
}

Outcome Registry::attempt(const std::filesystem::path& directory) {
  attempted_ = true;
  std::error_code error;
  const auto manifest = directory / manifest_file_;
  tried_ = directory;
  tried_existed_ = std::filesystem::exists(manifest, error);
  tried_time_ = tried_existed_ ? std::filesystem::last_write_time(manifest, error)
                               : std::filesystem::file_time_type{};
  Outcome outcome;
  outcome.directory = directory;
  if (directory.empty()) {
    outcome.message = "No home directory to look for the " + noun_ + " in";
    return outcome;
  }
  auto opened = open_(directory);
  if (!opened) {
    outcome.state = opened.error().code == ErrorCode::not_found ? State::missing : State::invalid;
    outcome.message = opened.error().message;
    return outcome;
  }
  if (next_serial_ > max_libraries_) {
    outcome.state = State::invalid;
    outcome.message = "Too many " + noun_ + " loads: restart to load another";
    return outcome;
  }
  auto library = std::move(opened).value();
  library->serial_ = next_serial_++;
  const Library* published = library.release(); // Never freed: kernels may read it.
  by_serial_[published->serial()].store(published, std::memory_order_release);
  current_.store(published, std::memory_order_release);
  outcome.state = State::ready;
  outcome.library = published;
  return outcome;
}

bool Registry::unchanged(const std::filesystem::path& directory) const {
  std::error_code error;
  const auto manifest = directory / manifest_file_;
  const bool exists = std::filesystem::exists(manifest, error);
  if (directory != tried_ || exists != tried_existed_) {
    return false;
  }
  return !exists || std::filesystem::last_write_time(manifest, error) == tried_time_;
}

Outcome Registry::status(const std::filesystem::path& directory) {
  std::lock_guard lock(mutex_);
  if (const auto* ready = current()) {
    Outcome outcome;
    outcome.state = State::ready;
    outcome.library = ready;
    return outcome;
  }
  if (!attempted_ || !unchanged(directory)) {
    outcome_ = attempt(directory);
  }
  return outcome_;
}

Outcome Registry::load(const std::filesystem::path& directory) {
  std::lock_guard lock(mutex_);
  auto result = attempt(directory);
  if (result.state == State::ready || current() == nullptr) {
    outcome_ = result;
  }
  return result;
}

// ---- Streaming ---------------------------------------------------------------------------

Streams::Streams(const Registry& registry, std::uint32_t channels, std::size_t slots)
    : registry_(registry), channels_(std::clamp<std::uint32_t>(channels, 1, max_channels)),
      slots_(std::make_unique<StreamSlot[]>(slots)), count_(slots),
      rings_(slots * ring_frames * channels_, 0) {
  Streamer::instance().add(this);
}

Streams::~Streams() { Streamer::instance().remove(this); }

void Streams::service() noexcept {
  std::lock_guard lock(mutex_);
  for (std::size_t index = 0; index < count_; ++index) {
    service_slot(index);
  }
}

void Streams::service_slot(std::size_t index) noexcept {
  auto& slot = slots_[index];
  const auto request = slot.request.load(std::memory_order_acquire);
  const auto generation = generation_of(request);
  if (generation != slot.served) {
    slot.served = generation;
    slot.filled = 0;
    slot.failed = false;
    slot.progress.store(tagged(generation, 0), std::memory_order_release);
  }
  const auto sample_index = sample_of(request);
  const Library* library = registry_.by_serial(serial_of(request));
  if (slot.failed || sample_index == no_sample || library == nullptr ||
      sample_index >= library->pack().samples().size()) {
    return;
  }
  const auto& pack = library->pack();
  if (pack.channels() != channels_) {
    slot.failed = true;
    failures_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto& sample = pack.sample(sample_index);
  const std::size_t bytes_per_frame = pack.bytes_per_frame();
  const std::uint32_t total = sample.frames - sample.resident;
  auto* ring = rings_.data() + index * ring_frames * channels_;
  while (slot.filled < total) {
    const auto consumed_word = slot.consumed.load(std::memory_order_acquire);
    const std::uint32_t consumed =
        generation_of(consumed_word) == generation ? frames_of(consumed_word) : 0;
    const std::uint32_t space = ring_frames - (slot.filled - std::min(consumed, slot.filled));
    const std::uint32_t remaining = total - slot.filled;
    // Read whole blocks unless the sample ends first.
    if (space < std::min(read_frames, remaining)) {
      return;
    }
    const std::uint32_t at = slot.filled & ring_mask;
    const std::uint32_t frames = std::min({read_frames, remaining, ring_frames - at});
    if (!read_exactly(pack.descriptor(), ring + std::size_t{at} * channels_,
                      std::size_t{frames} * bytes_per_frame,
                      (sample.offset + sample.resident + slot.filled) * bytes_per_frame)) {
      slot.failed = true;
      failures_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    slot.filled += frames;
    slot.progress.store(tagged(generation, slot.filled), std::memory_order_release);
    // A new note in this slot abandons the rest.
    if (generation_of(slot.request.load(std::memory_order_acquire)) != generation) {
      return;
    }
  }
}

bool gather(const Sample& sample, std::uint32_t channels, const std::int16_t* resident,
            const std::int16_t* ring, std::uint32_t streamed, std::int64_t first, std::size_t count,
            float* left, float* right) noexcept {
  return channels == 1
             ? gather_frames<1>(sample, resident, ring, streamed, first, count, left, right)
             : gather_frames<2>(sample, resident, ring, streamed, first, count, left, right);
}

// ---- Resampling --------------------------------------------------------------------------

SincTable::SincTable(double cutoff) : table_((phases + 1) * taps) {
  const double half = static_cast<double>(taps) / 2.0;
  for (std::size_t row = 0; row <= phases; ++row) {
    const double fraction = static_cast<double>(row) / static_cast<double>(phases);
    double sum = 0.0;
    std::array<double, taps> coefficients{};
    for (std::size_t t = 0; t < taps; ++t) {
      const double x = static_cast<double>(t) - static_cast<double>(left) - fraction;
      const double y = cutoff * x;
      const double sinc =
          std::fabs(y) < sinc_zero ? 1.0 : std::sin(std::numbers::pi * y) / (std::numbers::pi * y);
      const double r = std::clamp(x / half, -1.0, 1.0);
      const double window =
          bessel_i0(kaiser_beta * std::sqrt(1.0 - r * r)) / bessel_i0(kaiser_beta);
      coefficients[t] = cutoff * sinc * window;
      sum += coefficients[t];
    }
    for (std::size_t t = 0; t < taps; ++t) {
      table_[row * taps + t] = static_cast<float>(coefficients[t] / sum);
    }
  }
}

} // namespace oscillate::native_dsp::synth::sample_stream
