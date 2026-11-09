#include "bow_library.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace oscillate::native_dsp::synth::bow {
namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using sample_stream::parse_number;

constexpr std::uint32_t no_sample = sample_stream::no_sample;
// Bounds on the untrusted manifest and packs.
constexpr std::size_t max_samples = set_count * key_count * max_layers;
constexpr std::uint32_t max_sample_frames = max_sample_seconds * library::pack_rate;
constexpr std::uint64_t max_resident_bytes = 768ULL << 20U;
constexpr std::uint32_t stereo = 2;
constexpr std::uint32_t mono = 1;
// A recording further than a semitone from its key is not that key.
constexpr double max_cents = 100.0;
// channels mode instrument articulation key layer round_robin offset frames
// attack_end loop_start loop_end cents scale level_db
constexpr std::size_t sample_fields = 15;
// mono_pack <file> <bytes>
constexpr std::size_t mono_pack_fields = 2;
constexpr std::string_view library_folder = "oscillate/instruments/bow/strings";
constexpr sample_stream::ManifestFormat manifest_format{
    library::manifest_file, library::format_line, "Bow",
    "strings library",      library::pack_rate,   stereo};
// A process loads at most this many string libraries; only tests approach it.
constexpr std::uint32_t max_libraries = sample_stream::Registry::max_serials;

template <class Value, std::size_t N>
bool named(std::string_view text, const std::array<std::string_view, N>& names, Value& value) {
  for (std::size_t i = 0; i < N; ++i) {
    if (text == names[i]) {
      value = static_cast<Value>(i);
      return true;
    }
  }
  return false;
}

constexpr std::array<std::string_view, mode_count> mode_names{"solo", "section"};
constexpr std::array<std::string_view, instrument_count> instrument_names{"violin", "viola",
                                                                          "cello", "bass"};
constexpr std::array<std::string_view, recorded_count> recorded_names{"sustain", "staccato",
                                                                      "pizzicato", "tremolo"};

[[nodiscard]] constexpr std::size_t set_of(Mode mode, Instrument instrument,
                                           Recorded recorded) noexcept {
  return (static_cast<std::size_t>(mode) * instrument_count +
          static_cast<std::size_t>(instrument)) *
             recorded_count +
         static_cast<std::size_t>(recorded);
}

[[nodiscard]] constexpr std::size_t cell_of(std::size_t set, std::size_t key,
                                            std::uint32_t layer) noexcept {
  return (set * key_count + key) * max_layers + layer;
}

[[nodiscard]] constexpr Mode other(Mode mode) noexcept {
  return mode == Mode::solo ? Mode::section : Mode::solo;
}

[[nodiscard]] bool plain_file_name(std::string_view name) noexcept {
  return !name.empty() && name.find('/') == std::string_view::npos && name != "." && name != "..";
}

} // namespace

const Pick& Library::pick(Mode mode, Instrument instrument, Recorded recorded, int key,
                          std::uint32_t layer) const noexcept {
  const auto k = static_cast<std::size_t>(std::clamp(key, first_key, last_key) - first_key);
  const auto set = set_of(mode, instrument, recorded);
  const std::uint32_t top = std::max<std::uint32_t>(layers_[set], 1) - 1;
  return picks_[cell_of(set, k, std::min(layer, top))];
}

std::uint32_t Library::layers(Mode mode, Instrument instrument, Recorded recorded) const noexcept {
  return layers_[set_of(mode, instrument, recorded)];
}

// Each set's picks come from its own samples when it has any, else from the
// other mode's (a solo viola's tremolo plays the viola section's).
void Library::fill_picks() {
  // The round robins recorded per set, key and layer.
  std::vector<Pick> own(set_count * key_count * max_layers);
  std::array<std::uint32_t, set_count> own_layers{};
  for (std::uint32_t index = 0; index < samples_.size(); ++index) {
    const auto& s = samples_[index];
    const auto set = set_of(s.mode, s.instrument, s.recorded);
    auto& cell = own[cell_of(set, static_cast<std::size_t>(s.key - first_key), s.layer)];
    if (cell.count < max_round_robins) {
      cell.samples[cell.count++] = index;
    }
    own_layers[set] = std::max(own_layers[set], s.layer + 1);
  }
  const auto nearest_key = [&](std::size_t set, std::size_t key, std::uint32_t layer) {
    for (std::size_t distance = 0; distance < key_count; ++distance) {
      if (key >= distance && own[cell_of(set, key - distance, layer)].count > 0) {
        return own[cell_of(set, key - distance, layer)];
      }
      if (key + distance < key_count && own[cell_of(set, key + distance, layer)].count > 0) {
        return own[cell_of(set, key + distance, layer)];
      }
    }
    return Pick{};
  };
  picks_.assign(own.size(), Pick{});
  for (std::size_t m = 0; m < mode_count; ++m) {
    for (std::size_t i = 0; i < instrument_count; ++i) {
      for (std::size_t r = 0; r < recorded_count; ++r) {
        const auto mode = static_cast<Mode>(m);
        const auto instrument = static_cast<Instrument>(i);
        const auto recorded = static_cast<Recorded>(r);
        const auto set = set_of(mode, instrument, recorded);
        auto from = set;
        if (own_layers[set] == 0) {
          from = set_of(other(mode), instrument, recorded);
        }
        layers_[set] = own_layers[from];
        for (std::size_t key = 0; key < key_count; ++key) {
          for (std::uint32_t layer = 0; layer < layers_[set]; ++layer) {
            Pick found{};
            for (std::uint32_t distance = 0; distance < layers_[set] && found.count == 0;
                 ++distance) {
              if (layer >= distance) {
                found = nearest_key(from, key, layer - distance);
              }
              if (found.count == 0 && layer + distance < layers_[set]) {
                found = nearest_key(from, key, layer + distance);
              }
            }
            picks_[cell_of(set, key, layer)] = found;
          }
        }
      }
    }
  }
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory) {
  using R = Result<std::unique_ptr<Library>>;
  auto library = std::unique_ptr<Library>(new Library());
  std::vector<sample_stream::Sample> stereo_streams;
  std::vector<sample_stream::Sample> mono_streams;
  std::vector<bool> taken(set_count * key_count * max_layers * max_round_robins, false);
  std::string mono_name;
  std::uint64_t mono_bytes = 0;
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): sample_stream::ManifestLineReader.
  const auto own_line = [&](std::string_view key, std::string_view rest, std::string& error) {
    if (key == "mono_pack") {
      const auto f = sample_stream::fields(rest);
      if (f.size() != mono_pack_fields || !plain_file_name(f[0]) ||
          !parse_number(f[1], mono_bytes)) {
        error = "The strings library names its mono pack badly";
        return true;
      }
      mono_name = f[0];
      return true;
    }
    if (key != "sample") {
      return false;
    }
    const auto f = sample_stream::fields(rest);
    Sample sample;
    sample_stream::Sample stream;
    double scale = 0.0;
    if (f.size() != sample_fields || !parse_number(f[0], sample.channels) ||
        !named(f[1], mode_names, sample.mode) ||
        !named(f[2], instrument_names, sample.instrument) ||
        !named(f[3], recorded_names, sample.recorded) || !parse_number(f[4], sample.key) ||
        !parse_number(f[5], sample.layer) || !parse_number(f[6], sample.round_robin) ||
        !parse_number(f[7], stream.offset) || !parse_number(f[8], stream.frames) ||
        !parse_number(f[9], sample.attack_end) || !parse_number(f[10], sample.loop_start) ||
        !parse_number(f[11], sample.loop_end) || !parse_number(f[12], sample.cents) ||
        !parse_number(f[13], scale) || !parse_number(f[14], sample.level_db)) {
      error = "A strings library sample line is malformed";
      return true;
    }
    const bool one_shot = sample.loop_start == 0 && sample.loop_end == 0;
    const bool loop_ok = looped(sample.recorded)
                             ? sample.loop_start < sample.loop_end &&
                                   sample.loop_end <= stream.frames &&
                                   sample.loop_end - sample.loop_start >= min_loop_frames
                             : one_shot;
    const bool ok = (sample.channels == mono || sample.channels == stereo) &&
                    sample.key >= first_key && sample.key <= last_key &&
                    sample.layer < max_layers && sample.round_robin < max_round_robins &&
                    stream.frames > 0 && stream.frames <= max_sample_frames &&
                    sample.attack_end < stream.frames && loop_ok && std::isfinite(sample.cents) &&
                    std::fabs(sample.cents) <= max_cents && sample_stream::valid_scale(scale) &&
                    sample_stream::valid_level_db(sample.level_db);
    if (!ok) {
      error = "A strings library sample is out of range";
      return true;
    }
    if (library->samples_.size() >= max_samples) {
      error = "The strings library has too many samples";
      return true;
    }
    const auto set = set_of(sample.mode, sample.instrument, sample.recorded);
    const auto slot = cell_of(set, static_cast<std::size_t>(sample.key - first_key), sample.layer) *
                          max_round_robins +
                      sample.round_robin;
    if (taken[slot]) {
      error = "The strings library lists a sample twice";
      return true;
    }
    taken[slot] = true;
    stream.scale = static_cast<float>(scale);
    // Every sample stays in memory whole.
    stream.resident = stream.frames;
    auto& streams = sample.channels == mono ? mono_streams : stereo_streams;
    library->local_.push_back(static_cast<std::uint32_t>(streams.size()));
    streams.push_back(stream);
    library->samples_.push_back(sample);
    return true;
  };
  auto manifest = sample_stream::read_manifest(directory, manifest_format, own_line);
  if (!manifest) {
    return R::failure(std::move(manifest.error()));
  }
  if (library->samples_.empty() || mono_name.empty()) {
    return R::failure({ErrorCode::corrupt_data, "The strings library's manifest is incomplete"});
  }
  auto pack = sample_stream::open_pack(directory, manifest.value(), manifest_format,
                                       max_resident_bytes, std::move(stereo_streams));
  if (!pack) {
    return R::failure(std::move(pack.error()));
  }
  library->pack_ = std::move(pack).value();
  sample_stream::PackSpec mono_spec;
  mono_spec.path = directory / mono_name;
  mono_spec.bytes = mono_bytes;
  mono_spec.channels = mono;
  mono_spec.rate = library::pack_rate;
  mono_spec.max_resident_bytes = max_resident_bytes;
  mono_spec.noun = manifest_format.noun;
  auto mono_pack = sample_stream::Pack::open(mono_spec, std::move(mono_streams));
  if (!mono_pack) {
    return R::failure(std::move(mono_pack.error()));
  }
  library->mono_pack_ = std::move(mono_pack).value();
  library->fill_picks();

  auto& status = library->status_;
  status.directory = directory;
  status.name = std::move(manifest.value().name);
  status.attribution = std::move(manifest.value().attribution);
  status.state = library::State::ready;
  status.samples = library->samples_.size();
  status.resident_bytes = library->pack_.resident_bytes() + library->mono_pack_.resident_bytes();
  return R::success(std::move(library));
}

sample_stream::Registry& registry() {
  // Immortal: kernels may read a library until the process exits.
  static auto* instance =
      new sample_stream::Registry(library::manifest_file, &sample_stream::open_as<Library>,
                                  std::string(manifest_format.noun), max_libraries);
  return *instance;
}

const Library* current() noexcept { return static_cast<const Library*>(registry().current()); }

namespace {

// The first look's background read: the library is a few hundred
// megabytes, so the panel and the first Bow never wait for it.
class Loader {
public:
  Loader() = default;
  Loader(const Loader&) = delete;
  Loader& operator=(const Loader&) = delete;
  ~Loader() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // True while the read runs; starts it on the first call.
  bool loading(const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    if (!started_) {
      started_ = true;
      running_ = true;
      thread_ = std::thread([this, directory] {
        (void)registry().status(directory);
        {
          std::lock_guard done(mutex_);
          running_ = false;
        }
        finished_.notify_all();
      });
    }
    return running_;
  }

  void wait() {
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [this] { return !running_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable finished_;
  std::thread thread_;
  bool started_ = false;
  bool running_ = false;
};

Loader& loader() {
  static Loader instance;
  return instance;
}

} // namespace

} // namespace detail

namespace library {

std::filesystem::path default_directory() {
  return sample_stream::default_directory(directory_variable, detail::library_folder);
}

Status status() {
  if (const auto* ready = detail::current(); ready != nullptr) {
    return ready->status();
  }
  const auto directory = default_directory();
  if (detail::loader().loading(directory)) {
    Status loading;
    loading.state = State::loading;
    loading.directory = directory;
    return loading;
  }
  return sample_stream::status_as<detail::Library, Status>(detail::registry().status(directory));
}

Status load(const std::filesystem::path& directory) {
  return sample_stream::status_as<detail::Library, Status>(detail::registry().load(directory));
}

void wait() { detail::loader().wait(); }

} // namespace library

} // namespace oscillate::native_dsp::synth::bow
