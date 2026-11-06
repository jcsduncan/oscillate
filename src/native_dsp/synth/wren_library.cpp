#include "wren_library.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::synth::wren {
namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using sample_stream::parse_number;

constexpr std::uint32_t no_sample = sample_stream::no_sample;
// Bounds on the untrusted manifest and pack.
// One sample per cell at most.
constexpr std::size_t max_samples = kind_count * key_count * layer_count;
constexpr std::uint32_t max_sample_seconds = 10;
constexpr std::uint32_t max_sample_frames = max_sample_seconds * library::pack_rate;
constexpr std::uint64_t max_resident_bytes = 256ULL << 20U;
// A recording further than a semitone from its key is not that key.
constexpr double max_cents = 100.0;
// kind key layer offset frames attack_end loop_start loop_end cents scale peak_db
constexpr std::size_t sample_fields = 11;
constexpr std::string_view library_folder = "oscillate/instruments/wren/iowa-flute";
constexpr sample_stream::ManifestFormat manifest_format{
    library::manifest_file, library::format_line, "Wren",
    "flute library",        library::pack_rate,   library::pack_channels};
// A process loads at most this many flute libraries; only tests approach it.
constexpr std::uint32_t max_libraries = sample_stream::Registry::max_serials;

bool kind_named(std::string_view text, Kind& kind) {
  constexpr std::array<std::pair<std::string_view, Kind>, kind_count> names{{
      {"vibrato", Kind::vibrato},
      {"plain", Kind::plain},
  }};
  for (const auto& [name, value] : names) {
    if (text == name) {
      kind = value;
      return true;
    }
  }
  return false;
}

[[nodiscard]] Kind other(Kind kind) noexcept {
  return kind == Kind::plain ? Kind::vibrato : Kind::plain;
}

} // namespace

bool Library::has(Kind kind, int key, std::uint32_t layer) const noexcept {
  if (key < first_key || key > last_key || layer >= layer_count) {
    return false;
  }
  return cells_[static_cast<std::size_t>(kind)][static_cast<std::size_t>(key - first_key)][layer] !=
         no_sample;
}

// Ties go to the lower key, then the lower layer: deterministic, and a
// recording repitched up keeps its own timbre's brightness.
void Library::fill_picks() noexcept {
  const auto nearest_key = [&](Kind kind, std::size_t key, std::uint32_t layer) {
    const auto& by_key = cells_[static_cast<std::size_t>(kind)];
    for (std::size_t distance = 0; distance < key_count; ++distance) {
      if (key >= distance && by_key[key - distance][layer] != no_sample) {
        return by_key[key - distance][layer];
      }
      if (key + distance < key_count && by_key[key + distance][layer] != no_sample) {
        return by_key[key + distance][layer];
      }
    }
    return no_sample;
  };
  const auto nearest_layer = [&](Kind kind, std::size_t key, std::uint32_t layer) {
    for (std::uint32_t distance = 0; distance < layer_count; ++distance) {
      if (layer >= distance) {
        if (const auto found = nearest_key(kind, key, layer - distance); found != no_sample) {
          return found;
        }
      }
      if (layer + distance < layer_count) {
        if (const auto found = nearest_key(kind, key, layer + distance); found != no_sample) {
          return found;
        }
      }
    }
    return no_sample;
  };
  for (std::size_t k = 0; k < kind_count; ++k) {
    const auto kind = static_cast<Kind>(k);
    for (std::size_t key = 0; key < key_count; ++key) {
      for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
        auto found = nearest_layer(kind, key, layer);
        if (found == no_sample) {
          found = nearest_layer(other(kind), key, layer);
        }
        picks_[k][key][layer] = found;
      }
    }
  }
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory) {
  using R = Result<std::unique_ptr<Library>>;
  auto library = std::unique_ptr<Library>(new Library());
  for (auto& by_key : library->cells_) {
    for (auto& by_layer : by_key) {
      by_layer.fill(no_sample);
    }
  }
  std::vector<sample_stream::Sample> streams;
  const auto sample_line = [&](std::string_view key, std::string_view rest, std::string& error) {
    if (key != "sample") {
      return false;
    }
    const auto f = sample_stream::fields(rest);
    Sample sample;
    sample_stream::Sample stream;
    double scale = 0.0;
    if (f.size() != sample_fields || !kind_named(f[0], sample.kind) ||
        !parse_number(f[1], sample.key) || !parse_number(f[2], sample.layer) ||
        !parse_number(f[3], stream.offset) || !parse_number(f[4], stream.frames) ||
        !parse_number(f[5], sample.attack_end) || !parse_number(f[6], sample.loop_start) ||
        !parse_number(f[7], sample.loop_end) || !parse_number(f[8], sample.cents) ||
        !parse_number(f[9], scale) || !parse_number(f[10], sample.peak_db)) {
      error = "A flute library sample line is malformed";
      return true;
    }
    const bool ok = sample.key >= first_key && sample.key <= last_key &&
                    sample.layer < layer_count && stream.frames > 0 &&
                    stream.frames <= max_sample_frames && sample.attack_end < stream.frames &&
                    sample.loop_start < sample.loop_end && sample.loop_end <= stream.frames &&
                    sample.loop_end - sample.loop_start >= min_loop_frames &&
                    std::isfinite(sample.cents) && std::fabs(sample.cents) <= max_cents &&
                    sample_stream::valid_scale(scale) &&
                    sample_stream::valid_level_db(sample.peak_db);
    if (!ok) {
      error = "A flute library sample is out of range";
      return true;
    }
    if (library->samples_.size() >= max_samples) {
      error = "The flute library has too many samples";
      return true;
    }
    auto& cell = library->cells_[static_cast<std::size_t>(sample.kind)]
                                [static_cast<std::size_t>(sample.key - first_key)][sample.layer];
    if (cell != no_sample) {
      error = "The flute library lists a sample twice";
      return true;
    }
    cell = static_cast<std::uint32_t>(library->samples_.size());
    stream.scale = static_cast<float>(scale);
    // Every sample stays in memory whole.
    stream.resident = stream.frames;
    library->samples_.push_back(sample);
    streams.push_back(stream);
    return true;
  };
  auto manifest = sample_stream::read_manifest(directory, manifest_format, sample_line);
  if (!manifest) {
    return R::failure(std::move(manifest.error()));
  }
  if (library->samples_.empty()) {
    return R::failure({ErrorCode::corrupt_data, "The flute library's manifest is incomplete"});
  }
  auto pack = sample_stream::open_pack(directory, manifest.value(), manifest_format,
                                       max_resident_bytes, std::move(streams));
  if (!pack) {
    return R::failure(std::move(pack.error()));
  }
  library->pack_ = std::move(pack).value();
  auto& status = library->status_;
  status.directory = directory;
  status.name = std::move(manifest.value().name);
  status.attribution = std::move(manifest.value().attribution);
  library->fill_picks();

  status.state = library::State::ready;
  status.samples = library->samples_.size();
  status.resident_bytes = library->pack_.resident_bytes();
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

} // namespace detail

namespace library {

std::filesystem::path default_directory() {
  return sample_stream::default_directory(directory_variable, detail::library_folder);
}

Status status() {
  return sample_stream::status_as<detail::Library, Status>(
      detail::registry().status(default_directory()));
}

Status load(const std::filesystem::path& directory) {
  return sample_stream::status_as<detail::Library, Status>(detail::registry().load(directory));
}

} // namespace library

} // namespace oscillate::native_dsp::synth::wren
