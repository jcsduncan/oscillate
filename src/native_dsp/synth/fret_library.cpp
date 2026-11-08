#include "fret_library.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::synth::fret {
namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using sample_stream::parse_number;

// Bounds on the untrusted manifest and pack.
constexpr std::size_t max_samples = 4'096;
constexpr std::uint32_t max_sample_seconds = 30;
constexpr std::uint32_t max_sample_frames = max_sample_seconds * library::pack_rate;
// Release samples stay resident whole.
constexpr std::uint32_t max_release_seconds = 5;
constexpr std::uint32_t max_release_frames = max_release_seconds * library::pack_rate;
constexpr std::uint64_t max_resident_bytes = 256ULL << 20U;
// articulation key layer layers round_robin offset frames scale peak_db
constexpr std::size_t sample_fields = 9;
constexpr std::string_view library_folder = "oscillate/instruments/fret/black-and-blue-darkblack";
constexpr sample_stream::ManifestFormat manifest_format{
    library::manifest_file, library::format_line, "Fret",
    "bass library",         library::pack_rate,   library::pack_channels};
// A process loads at most this many bass libraries; only tests approach it.
constexpr std::uint32_t max_libraries = sample_stream::Registry::max_serials;

bool kind_named(std::string_view text, Kind& kind) {
  constexpr std::array<std::pair<std::string_view, Kind>, kind_count> names{{
      {"pluck", Kind::pluck},
      {"staccato", Kind::staccato},
      {"ghost", Kind::ghost},
      {"bridge", Kind::bridge},
      {"release", Kind::release},
  }};
  for (const auto& [name, value] : names) {
    if (text == name) {
      kind = value;
      return true;
    }
  }
  return false;
}

} // namespace

std::span<const std::uint32_t> Library::round_robins(Kind kind, int key,
                                                     std::uint32_t layer) const noexcept {
  if (key < first_key || key > last_key || layer >= max_layers) {
    return {};
  }
  return cells_[static_cast<std::size_t>(kind)][static_cast<std::size_t>(key - first_key)]
      .layers[layer];
}

std::uint32_t Library::layers(Kind kind, int key) const noexcept {
  if (key < first_key || key > last_key) {
    return 0;
  }
  return cells_[static_cast<std::size_t>(kind)][static_cast<std::size_t>(key - first_key)].count;
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory) {
  using R = Result<std::unique_ptr<Library>>;
  const auto fail = [](std::string message) {
    return R::failure({ErrorCode::corrupt_data, std::move(message)});
  };
  auto library = std::unique_ptr<Library>(new Library());
  std::vector<sample_stream::Sample> streams;
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): sample_stream::ManifestLineReader.
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
        !parse_number(f[3], sample.layers) || !parse_number(f[4], sample.round_robin) ||
        !parse_number(f[5], stream.offset) || !parse_number(f[6], stream.frames) ||
        !parse_number(f[7], scale) || !parse_number(f[8], sample.peak_db)) {
      error = "A bass library sample line is malformed";
      return true;
    }
    const bool release = sample.kind == Kind::release;
    const bool ok = sample.key >= first_key && sample.key <= last_key && sample.layers >= 1 &&
                    sample.layers <= max_layers && sample.layer < sample.layers &&
                    sample.round_robin < max_round_robins && stream.frames > 0 &&
                    stream.frames <= (release ? max_release_frames : max_sample_frames) &&
                    sample_stream::valid_scale(scale) &&
                    sample_stream::valid_level_db(sample.peak_db);
    if (!ok) {
      error = "A bass library sample is out of range";
    } else if (library->samples_.size() >= max_samples) {
      error = "The bass library has too many samples";
    } else {
      stream.scale = static_cast<float>(scale);
      stream.resident = release ? stream.frames : library::resident_frames;
      library->samples_.push_back(sample);
      streams.push_back(stream);
    }
    return true;
  };
  auto manifest = sample_stream::read_manifest(directory, manifest_format, sample_line);
  if (!manifest) {
    return R::failure(std::move(manifest.error()));
  }
  if (library->samples_.empty()) {
    return fail("The bass library's manifest is incomplete");
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

  // Index each kind's keys by layer and round robin; every layer of a key
  // agrees on how many layers it has.
  for (std::uint32_t index = 0; index < library->samples_.size(); ++index) {
    const auto& sample = library->samples_[index];
    auto& cell = library->cells_[static_cast<std::size_t>(sample.kind)]
                                [static_cast<std::size_t>(sample.key - first_key)];
    if (cell.count != 0 && cell.count != sample.layers) {
      return fail("A bass library key disagrees on its layers");
    }
    cell.count = sample.layers;
    cell.layers[sample.layer].push_back(index);
    if (sample.kind == Kind::release) {
      library->longest_release_ =
          std::max(library->longest_release_, library->stream(index).frames);
    }
  }
  for (auto& kind : library->cells_) {
    for (auto& cell : kind) {
      for (std::uint32_t layer = 0; layer < cell.count; ++layer) {
        auto& robins = cell.layers[layer];
        if (robins.empty()) {
          return fail("A bass library key leaves a layer empty");
        }
        std::ranges::sort(robins, {},
                          [&](std::uint32_t i) { return library->samples_[i].round_robin; });
      }
    }
  }
  for (int key = first_key; key <= last_key; ++key) {
    if (library->layers(Kind::pluck, key) == 0) {
      return fail("The bass library leaves a key without a pluck");
    }
  }

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

} // namespace oscillate::native_dsp::synth::fret
