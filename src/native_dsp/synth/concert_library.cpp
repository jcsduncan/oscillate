#include "concert_library.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::native_dsp::synth::concert {
namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using sample_stream::fields;
using sample_stream::parse_number;

// Bounds on the untrusted manifest and pack.
constexpr std::size_t max_samples = 4'096;
constexpr std::uint32_t max_sample_frames = 60U * library::pack_rate;
// Release, resonance and pedal samples stay resident whole.
constexpr std::uint32_t max_one_shot_frames = 10U * library::pack_rate;
constexpr std::uint64_t max_resident_bytes = 512ULL << 20U;
constexpr int max_velocity = 127;
constexpr double max_decay_db_per_second = 60.0;
constexpr std::string_view library_folder = "oscillate/instruments/concert/salamander-grand-v3";
// Provenance lines the installer writes and the engine does not read.
constexpr std::array<std::string_view, 4> provenance_keys{"instrument", "licence", "source",
                                                          "commit"};
constexpr sample_stream::ManifestFormat manifest_format{
    library::manifest_file, library::format_line, "Concert", "piano library",
    library::pack_rate,     pack_channels,        false,     provenance_keys};
// Sample lines carry this many fields.
constexpr std::size_t sample_fields = 13;

bool kind_of(std::string_view text, SampleKind& kind) {
  if (text == "note") {
    kind = SampleKind::note;
  } else if (text == "release") {
    kind = SampleKind::release;
  } else if (text == "resonance") {
    kind = SampleKind::resonance;
  } else if (text == "pedal_down") {
    kind = SampleKind::pedal_down;
  } else if (text == "pedal_up") {
    kind = SampleKind::pedal_up;
  } else {
    return false;
  }
  return true;
}

bool keyed(SampleKind kind) noexcept {
  return kind == SampleKind::note || kind == SampleKind::release || kind == SampleKind::resonance;
}

} // namespace

std::span<const std::uint32_t> Library::notes(int key) const noexcept {
  if (key < first_key || key > last_key) {
    return {};
  }
  return keys_[static_cast<std::size_t>(key - first_key)].notes;
}

std::span<const std::uint32_t> Library::releases(int key) const noexcept {
  if (key < first_key || key > last_key) {
    return {};
  }
  return keys_[static_cast<std::size_t>(key - first_key)].releases;
}

std::span<const std::uint32_t> Library::resonances(int key) const noexcept {
  if (key < first_key || key > last_key) {
    return {};
  }
  return keys_[static_cast<std::size_t>(key - first_key)].resonances;
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory) {
  using R = Result<std::unique_ptr<Library>>;
  const auto fail = [](std::string message) {
    return R::failure({ErrorCode::corrupt_data, std::move(message)});
  };
  auto library = std::unique_ptr<Library>(new Library());
  bool damped_set = false;
  std::vector<sample_stream::Sample> streams;
  const auto own_line = [&](std::string_view key, std::string_view rest, std::string& error) {
    if (key == "last_damped_key") {
      if (!parse_number(rest, library->last_damped_key_) ||
          library->last_damped_key_ < first_key - 1 || library->last_damped_key_ > last_key) {
        error = "The piano library's damper range is invalid";
      }
      damped_set = true;
      return true;
    }
    if (key != "sample") {
      return false;
    }
    const auto f = fields(rest);
    Sample sample;
    sample_stream::Sample stream;
    double scale = 0.0;
    if (f.size() < sample_fields || !kind_of(f[0], sample.kind) ||
        !parse_number(f[1], sample.first_key) || !parse_number(f[2], sample.last_key) ||
        !parse_number(f[3], sample.root_key) || !parse_number(f[4], sample.low_velocity) ||
        !parse_number(f[5], sample.high_velocity) || !parse_number(f[6], stream.offset) ||
        !parse_number(f[7], stream.frames) || !parse_number(f[8], scale) ||
        !parse_number(f[9], sample.peak_db) || !parse_number(f[10], sample.gain_db) ||
        !parse_number(f[11], sample.velocity_track) ||
        !parse_number(f[12], sample.decay_db_per_second)) {
      error = "A piano library sample line is malformed";
      return true;
    }
    stream.scale = static_cast<float>(scale);
    const bool keys_ok =
        !keyed(sample.kind) ||
        (sample.first_key >= first_key && sample.last_key <= last_key &&
         sample.first_key <= sample.last_key &&
         std::abs(sample.first_key - sample.root_key) <= library::max_repitch_semitones &&
         std::abs(sample.last_key - sample.root_key) <= library::max_repitch_semitones);
    const bool ok =
        keys_ok && sample.low_velocity >= 1 && sample.high_velocity <= max_velocity &&
        sample.low_velocity <= sample.high_velocity && stream.frames > 0 &&
        stream.frames <= max_sample_frames &&
        (sample.kind == SampleKind::note || stream.frames <= max_one_shot_frames) &&
        sample_stream::valid_scale(scale) && sample_stream::valid_level_db(sample.peak_db) &&
        sample_stream::valid_level_db(sample.gain_db) && std::isfinite(sample.velocity_track) &&
        sample.velocity_track >= 0.0 && sample.velocity_track <= 1.0 &&
        std::isfinite(sample.decay_db_per_second) && sample.decay_db_per_second >= 0.0 &&
        sample.decay_db_per_second <= max_decay_db_per_second;
    if (!ok) {
      error = "A piano library sample is out of range";
    } else if (library->samples_.size() >= max_samples) {
      error = "The piano library has too many samples";
    } else {
      // A note keeps its start in memory; the mechanism's samples stay whole.
      stream.resident = sample.kind == SampleKind::note ? library::resident_frames : stream.frames;
      library->samples_.push_back(sample);
      streams.push_back(stream);
    }
    return true;
  };
  auto manifest = sample_stream::read_manifest(directory, manifest_format, own_line);
  if (!manifest) {
    return R::failure(std::move(manifest.error()));
  }
  if (!damped_set || library->samples_.empty()) {
    return fail("The piano library's manifest is incomplete");
  }

  // Open and check the pack, then read the resident starts.
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

  // Index the keys.
  for (std::uint32_t index = 0; index < library->samples_.size(); ++index) {
    const auto& sample = library->samples_[index];
    const auto& stream = library->stream(index);
    switch (sample.kind) {
    case SampleKind::note:
    case SampleKind::release:
    case SampleKind::resonance:
      for (int key = sample.first_key; key <= sample.last_key; ++key) {
        auto& entry = library->keys_[static_cast<std::size_t>(key - first_key)];
        (sample.kind == SampleKind::note      ? entry.notes
         : sample.kind == SampleKind::release ? entry.releases
                                              : entry.resonances)
            .push_back(index);
      }
      break;
    case SampleKind::pedal_down:
      library->pedal_down_.push_back(index);
      break;
    case SampleKind::pedal_up:
      library->pedal_up_.push_back(index);
      break;
    }
    if (sample.kind == SampleKind::note) {
      if (sample.last_key > library->last_damped_key_) {
        library->longest_undamped_note_ = std::max(library->longest_undamped_note_, stream.frames);
      }
    } else if (sample.kind != SampleKind::pedal_down) {
      library->longest_one_shot_ = std::max(library->longest_one_shot_, stream.frames);
    }
  }
  for (std::size_t k = 0; k < key_count; ++k) {
    auto& notes = library->keys_[k].notes;
    if (notes.empty()) {
      return fail("The piano library leaves a key without a note");
    }
    std::ranges::sort(notes, [&](std::uint32_t a, std::uint32_t b) {
      const auto& x = library->samples_[a];
      const auto& y = library->samples_[b];
      return x.low_velocity != y.low_velocity ? x.low_velocity < y.low_velocity
                                              : x.high_velocity < y.high_velocity;
    });
  }

  status.state = library::State::ready;
  status.samples = library->samples_.size();
  status.resident_bytes = library->pack().resident_bytes();
  status.last_damped_key = library->last_damped_key_;
  status.ring_seconds.reserve(key_count);
  for (std::size_t k = 0; k < key_count; ++k) {
    const auto& notes = library->keys_[k].notes;
    const auto& middle = library->stream(notes[notes.size() / 2]);
    status.ring_seconds.push_back(static_cast<double>(middle.frames) / library::pack_rate);
  }
  return R::success(std::move(library));
}

// Immortal: kernels may read a library until the process exits.
sample_stream::Registry& registry() {
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

} // namespace oscillate::native_dsp::synth::concert
