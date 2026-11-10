#include "stick_library.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace oscillate::native_dsp::synth::stick {
namespace detail {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using sample_stream::parse_number;

// mic <channel> <group> <piece[+piece...]|-> <mono|left|right|centre>
constexpr std::size_t mic_fields = 4;
// hit <piece> <articulation> <stick|brush> <offset> <frames> <scale> <power_db>
constexpr std::size_t hit_fields = 7;
constexpr std::string_view no_piece = "-";
// The resident starts are bounded well above the largest kit's.
constexpr std::uint64_t max_resident_bytes = 512ULL << 20U;
constexpr std::string_view library_folder = "oscillate/instruments/stick";
// The channel count is the kit's own (its `channels` line, read here).
constexpr std::uint32_t placeholder_channels = 1;
constexpr sample_stream::ManifestFormat manifest_format{
    library::manifest_file, library::format_line,   "Stick", "drum kit", library::pack_rate,
    placeholder_channels,   /*channels_line=*/false};
// A process loads at most this many kits; only tests approach it.
constexpr std::uint32_t max_libraries = sample_stream::Registry::max_serials;

constexpr std::array<std::string_view, piece_count> piece_names{
    "kick", "snare",  "hihat",  "tom1", "tom2",   "tom3", "tom4",
    "tom5", "crash1", "crash2", "ride", "splash", "china"};
constexpr std::array<std::string_view, articulation_count> articulation_names{
    "hit", "alt", "sidestick", "bell", "closed", "half", "open", "pedal"};
constexpr std::array<std::string_view, mic_group_count> group_names{
    "kick_in", "kick_out",  "kick_sub", "snare_top", "snare_bottom", "toms",    "hihat",
    "ride",    "overheads", "room",     "far_room",  "mid_side",     "china_oh"};
constexpr std::array<std::string_view, 4> side_names{"mono", "left", "right", "centre"};
constexpr char piece_separator = '+';
constexpr std::array<std::string_view, striker_count> striker_names{"stick", "brush"};

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

[[nodiscard]] constexpr bool is_tom(Piece piece) noexcept {
  return piece >= Piece::tom1 && piece <= Piece::tom5;
}

// "tom1+tom2": the pieces a close mic recorded, the first its own.
bool pieces_of(std::string_view text, Mic& mic) {
  bool first = true;
  while (!text.empty()) {
    const auto end = text.find(piece_separator);
    Piece piece = Piece::kick;
    if (!named(text.substr(0, end), piece_names, piece) || mic.hears(piece)) {
      return false;
    }
    mic.pieces |= piece_bit(piece);
    if (first) {
      mic.piece = piece;
      first = false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    text.remove_prefix(end + 1);
  }
  return false;
}

} // namespace

void Library::index() {
  std::array<std::vector<std::uint32_t>, stroke_count> by_stroke;
  for (std::uint32_t i = 0; i < hits_.size(); ++i) {
    const auto& h = hits_[i];
    by_stroke[stroke_index(h.piece, h.articulation, h.striker)].push_back(i);
  }
  order_.clear();
  order_.reserve(hits_.size());
  for (std::size_t s = 0; s < by_stroke.size(); ++s) {
    auto& members = by_stroke[s];
    std::ranges::stable_sort(members, [this](std::uint32_t a, std::uint32_t b) {
      return hits_[a].power_db < hits_[b].power_db;
    });
    auto& stroke = strokes_[s];
    stroke.first = static_cast<std::uint32_t>(order_.size());
    stroke.count = static_cast<std::uint32_t>(members.size());
    if (!members.empty()) {
      stroke.softest_db = hits_[members.front()].power_db;
      stroke.loudest_db = hits_[members.back()].power_db;
    }
    order_.insert(order_.end(), members.begin(), members.end());
    const auto piece = s / (articulation_count * striker_count);
    const auto articulation = s / striker_count % articulation_count;
    contents_.hits[piece] += stroke.count;
    if (stroke.count > 0) {
      contents_.articulations[piece] |= 1U << articulation;
      contents_.brushes[piece] =
          contents_.brushes[piece] || s % striker_count == static_cast<std::size_t>(Striker::brush);
    }
  }
  for (const auto& mic : mics_) {
    ++contents_.mics[static_cast<std::size_t>(mic.group)];
  }
  contents_.toms = 0;
  for (std::size_t p = 0; p < piece_count; ++p) {
    if (is_tom(static_cast<Piece>(p)) && contents_.hits[p] > 0) {
      ++contents_.toms;
    }
  }
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory) {
  using R = Result<std::unique_ptr<Library>>;
  auto library = std::unique_ptr<Library>(new Library());
  std::vector<sample_stream::Sample> streams;
  std::uint32_t channels = 0;
  std::vector<bool> mic_seen;
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): sample_stream::ManifestLineReader.
  const auto own_line = [&](std::string_view key, std::string_view rest, std::string& error) {
    if (key == "channels") {
      if (channels != 0 || !parse_number(rest, channels) || channels == 0 || channels > max_mics) {
        error = "The drum kit's channel count is not supported";
        return true;
      }
      mic_seen.assign(channels, false);
      library->mics_.assign(channels, Mic{});
      return true;
    }
    if (key == "mic") {
      const auto f = sample_stream::fields(rest);
      std::uint32_t channel = 0;
      Mic mic;
      if (f.size() != mic_fields || !parse_number(f[0], channel) ||
          !named(f[1], group_names, mic.group) || !named(f[3], side_names, mic.side)) {
        error = "A drum kit mic line is malformed";
        return true;
      }
      if (f[2] != no_piece && !pieces_of(f[2], mic)) {
        error = "A drum kit mic line is malformed";
        return true;
      }
      // Close mics hear their pieces in mono; the overheads and rooms hear
      // the kit, as a pair or a single mic in the centre.
      const bool close = close_group(mic.group);
      const bool ok = channel < channels && !mic_seen[channel] && close == (mic.pieces != 0U) &&
                      close == (mic.side == Side::mono);
      if (!ok) {
        error = "A drum kit mic is out of place";
        return true;
      }
      mic_seen[channel] = true;
      library->mics_[channel] = mic;
      return true;
    }
    if (key != "hit") {
      return false;
    }
    const auto f = sample_stream::fields(rest);
    Hit hit;
    sample_stream::Sample stream;
    double scale = 0.0;
    if (f.size() != hit_fields || !named(f[0], piece_names, hit.piece) ||
        !named(f[1], articulation_names, hit.articulation) ||
        !named(f[2], striker_names, hit.striker) || !parse_number(f[3], stream.offset) ||
        !parse_number(f[4], stream.frames) || !parse_number(f[5], scale) ||
        !parse_number(f[6], hit.power_db)) {
      error = "A drum kit hit line is malformed";
      return true;
    }
    const bool ok = plays(hit.piece, hit.articulation) && stream.frames > 0 &&
                    stream.frames <= max_hit_frames && sample_stream::valid_scale(scale) &&
                    sample_stream::valid_level_db(hit.power_db);
    if (!ok) {
      error = "A drum kit hit is out of range";
      return true;
    }
    if (library->hits_.size() >= max_hits) {
      error = "The drum kit has too many hits";
      return true;
    }
    stream.scale = static_cast<float>(scale);
    stream.resident = resident_frames;
    streams.push_back(stream);
    library->hits_.push_back(hit);
    return true;
  };
  auto manifest = sample_stream::read_manifest(directory, manifest_format, own_line);
  if (!manifest) {
    return R::failure(std::move(manifest.error()));
  }
  if (channels == 0 || library->hits_.empty() || !std::ranges::all_of(mic_seen, std::identity{})) {
    return R::failure({ErrorCode::corrupt_data, "The drum kit's manifest is incomplete"});
  }
  manifest.value().channels = channels;
  auto pack = sample_stream::open_pack(directory, manifest.value(), manifest_format,
                                       max_resident_bytes, std::move(streams));
  if (!pack) {
    return R::failure(std::move(pack.error()));
  }
  library->pack_ = std::move(pack).value();
  library->index();
  for (std::size_t s = 0; s < library->strokes_.size(); ++s) {
    if (library->strokes_[s].count > max_hits_per_stroke) {
      return R::failure({ErrorCode::corrupt_data, "A drum kit stroke has too many hits"});
    }
  }

  auto& status = library->status_;
  status.directory = directory;
  status.name = std::move(manifest.value().name);
  status.attribution = std::move(manifest.value().attribution);
  status.state = library::State::ready;
  status.samples = library->hits_.size();
  status.resident_bytes = library->pack_.resident_bytes();
  status.pack_bytes = manifest.value().pack_bytes;
  status.mics = channels;
  return R::success(std::move(library));
}

sample_stream::Registry& registry() {
  // Immortal: kernels may read a library until the process exits.
  static auto* instance =
      new sample_stream::Registry(library::manifest_file, &sample_stream::open_as<Library>,
                                  std::string(manifest_format.noun), max_libraries);
  return *instance;
}

namespace {

// One kit's library and what was last found looking for it. The pointer
// and state are read on the callback; the rest under the mutex.
struct Slot {
  std::atomic<const Library*> library{nullptr};
  std::atomic<library::State> state{library::State::missing};
  bool attempted = false;
  library::Status outcome;
  // What the last failed look saw, so an unchanged folder is not parsed
  // again on every call.
  std::filesystem::path tried;
  bool tried_existed = false;
  std::filesystem::file_time_type tried_time{};
  bool reading = false;
  std::thread reader;
};

// Every kit's slot, and the background reads. Immortal, like the
// libraries: a read may still be running at exit.
class Kits {
public:
  static Kits& instance() {
    static auto* kits = new Kits();
    return *kits;
  }

  [[nodiscard]] Slot& slot(Kit kit) noexcept { return slots_[static_cast<std::size_t>(kit)]; }

  // The ready library, or the last look's outcome, looking again when the
  // folder changed.
  library::Status look(Kit kit, const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    auto& s = slot(kit);
    if (const auto* ready = s.library.load(std::memory_order_acquire)) {
      return ready->status();
    }
    if (s.reading) {
      library::Status loading;
      loading.state = library::State::loading;
      loading.directory = directory;
      return loading;
    }
    if (!s.attempted || !unchanged(s, directory)) {
      attempt(s, directory);
    }
    return s.outcome;
  }

  library::Status load(Kit kit, const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    auto& s = slot(kit);
    attempt(s, directory);
    return s.outcome;
  }

  // Starts reading `kit` from `directory` in the background unless it is
  // loaded, being read, or unchanged since a failed look. True while a
  // read runs.
  bool fetch(Kit kit, const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    auto& s = slot(kit);
    if (s.library.load(std::memory_order_acquire) != nullptr) {
      return false;
    }
    if (s.reading) {
      return true;
    }
    if (s.attempted && unchanged(s, directory)) {
      return false;
    }
    if (s.reader.joinable()) {
      s.reader.join();
    }
    const auto before = s.state.load(std::memory_order_relaxed);
    s.reading = true;
    s.state.store(library::State::loading, std::memory_order_relaxed);
    try {
      s.reader = std::thread([this, &s, directory] {
        // A failed read reports the kit invalid rather than ending the process.
        sample_stream::Outcome outcome;
        try {
          outcome = registry().load(directory);
        } catch (const std::exception& error) {
          outcome.state = sample_stream::State::invalid;
          outcome.message = error.what();
          outcome.directory = directory;
        }
        std::lock_guard done(mutex_);
        record(s, directory, outcome);
        s.reading = false;
        finished_.notify_all();
      });
    } catch (const std::exception&) {
      // No thread: the kit is as it was, and the next look tries again.
      s.reading = false;
      s.state.store(before, std::memory_order_relaxed);
    }
    return s.reading;
  }

  void wait() {
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [this] {
      return std::ranges::none_of(slots_, [](const Slot& s) { return s.reading; });
    });
  }

private:
  Kits() = default;

  static bool unchanged(const Slot& s, const std::filesystem::path& directory) {
    std::error_code error;
    const auto manifest = directory / library::manifest_file;
    const bool exists = std::filesystem::exists(manifest, error);
    if (directory != s.tried || exists != s.tried_existed) {
      return false;
    }
    return !exists || std::filesystem::last_write_time(manifest, error) == s.tried_time;
  }

  static void record(Slot& s, const std::filesystem::path& directory,
                     const sample_stream::Outcome& found) {
    auto outcome = sample_stream::status_as<Library, library::Status>(found);
    std::error_code error;
    const auto manifest = directory / library::manifest_file;
    s.attempted = true;
    s.tried = directory;
    s.tried_existed = std::filesystem::exists(manifest, error);
    s.tried_time = s.tried_existed ? std::filesystem::last_write_time(manifest, error)
                                   : std::filesystem::file_time_type{};
    if (found.library != nullptr) {
      s.library.store(static_cast<const Library*>(found.library), std::memory_order_release);
    }
    s.state.store(outcome.state, std::memory_order_relaxed);
    s.outcome = std::move(outcome);
  }

  static void attempt(Slot& s, const std::filesystem::path& directory) {
    record(s, directory, registry().load(directory));
  }

  std::mutex mutex_;
  std::condition_variable finished_;
  std::array<Slot, kit_count> slots_{};
};

} // namespace

const Library* current(Kit kit) noexcept {
  return Kits::instance().slot(kit).library.load(std::memory_order_acquire);
}

library::State published_state(Kit kit) noexcept {
  return Kits::instance().slot(kit).state.load(std::memory_order_relaxed);
}

void fetch(Kit kit) { (void)Kits::instance().fetch(kit, library::kit_directory(kit)); }

} // namespace detail

namespace library {

std::filesystem::path default_directory() {
  return sample_stream::default_directory(directory_variable, detail::library_folder);
}

std::filesystem::path kit_directory(Kit kit) {
  const auto base = default_directory();
  return base.empty() ? base : base / facts(kit).folder;
}

Status status(Kit kit) {
  auto& kits = detail::Kits::instance();
  const auto directory = kit_directory(kit);
  if (kits.fetch(kit, directory)) {
    Status loading;
    loading.state = State::loading;
    loading.directory = directory;
    return loading;
  }
  return kits.look(kit, directory);
}

Contents contents(Kit kit) {
  const auto* ready = detail::current(kit);
  return ready == nullptr ? Contents{} : ready->contents();
}

Status load(Kit kit, const std::filesystem::path& directory) {
  return detail::Kits::instance().load(kit, directory);
}

void wait() { detail::Kits::instance().wait(); }

} // namespace library

} // namespace oscillate::native_dsp::synth::stick
