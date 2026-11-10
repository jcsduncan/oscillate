#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

// Stick's kit libraries (ADR-0114), on the shared sample core: one pack per
// kit of every mic's channel interleaved, each hit's start resident and the
// rest streamed. Private to the engine.
namespace oscillate::native_dsp::synth::stick::detail {

// Where a mic sits in the stereo image: a close mic is placed by its
// piece's Pan; a pair's channels are its left and right; a single distant
// mic (Naked's close room) sits in the centre.
enum class Side : std::uint8_t { mono, left, right, centre };

struct Mic {
  MicGroup group = MicGroup::overheads;
  // The pieces a close mic recorded (a piece_bit each); none for the
  // overheads and rooms. Naked's toms share one channel, each tom heard
  // only on its own mic.
  std::uint32_t pieces = 0;
  // The piece its bleed pans with: its first.
  Piece piece = Piece::kick;
  Side side = Side::mono;
  [[nodiscard]] bool hears(Piece p) const noexcept { return (pieces & piece_bit(p)) != 0U; }
};

struct Hit {
  Piece piece = Piece::kick;
  Articulation articulation = Articulation::hit;
  Striker striker = Striker::stick;
  // Its recorded power: the RMS of its own close mics (the overheads for a
  // cymbal) over its first 50 ms, in dB as the pack's scale plays it.
  double power_db = 0.0;
};

// The hits of one piece, articulation and striker, softest first.
struct Stroke {
  std::uint32_t first = 0; // Into Library::order().
  std::uint32_t count = 0;
  double softest_db = 0.0;
  double loudest_db = 0.0;
};

// Each hit's first this many frames stay in memory (85 ms at 48 kHz): the
// streamer, which looks every 2 ms, has that long to bring the rest.
inline constexpr std::uint32_t resident_frames = 4'096;
// The longest hit accepted.
inline constexpr std::uint32_t max_hit_seconds = 20;
inline constexpr std::uint32_t max_hit_frames = max_hit_seconds * library::pack_rate;
// Bounds on the untrusted manifest.
inline constexpr std::size_t max_hits_per_stroke = 128;
inline constexpr std::size_t stroke_count = piece_count * articulation_count * striker_count;
inline constexpr std::size_t max_hits = stroke_count * max_hits_per_stroke;

[[nodiscard]] constexpr std::size_t stroke_index(Piece piece, Articulation articulation,
                                                 Striker striker) noexcept {
  return (static_cast<std::size_t>(piece) * articulation_count +
          static_cast<std::size_t>(articulation)) *
             striker_count +
         static_cast<std::size_t>(striker);
}

class Library final : public sample_stream::Library {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<Library>>
  open(const std::filesystem::path& directory);

  [[nodiscard]] std::uint32_t channels() const noexcept { return pack_.channels(); }
  [[nodiscard]] std::span<const Mic> mics() const noexcept { return mics_; }
  [[nodiscard]] const Hit& hit(std::uint32_t index) const noexcept { return hits_[index]; }
  [[nodiscard]] const sample_stream::Sample& stream(std::uint32_t index) const noexcept {
    return pack_.sample(index);
  }
  [[nodiscard]] const std::int16_t* resident(std::uint32_t index) const noexcept {
    return pack_.resident(pack_.sample(index));
  }
  // A stroke's hits; brushes fall back to sticks where none were recorded,
  // and a second drum (`alt`) the kit did not record to the plain stroke, so
  // General MIDI's B1 still plays a kick on Naked Drums.
  [[nodiscard]] const Stroke& stroke(Piece piece, Articulation articulation,
                                     Striker striker) const noexcept {
    const auto& found = recorded(piece, articulation, striker);
    return found.count > 0 || articulation != Articulation::alt
               ? found
               : recorded(piece, Articulation::hit, striker);
  }
  // Hit indices, each stroke's softest first.
  [[nodiscard]] std::uint32_t order(std::uint32_t at) const noexcept { return order_[at]; }
  [[nodiscard]] const library::Contents& contents() const noexcept { return contents_; }
  [[nodiscard]] const library::Status& status() const noexcept { return status_; }

private:
  Library() = default;
  void index();
  [[nodiscard]] const Stroke& recorded(Piece piece, Articulation articulation,
                                       Striker striker) const noexcept {
    const auto& brushed = strokes_[stroke_index(piece, articulation, striker)];
    return brushed.count > 0 ? brushed
                             : strokes_[stroke_index(piece, articulation, Striker::stick)];
  }

  std::vector<Mic> mics_;
  std::vector<Hit> hits_;
  std::vector<std::uint32_t> order_;
  std::array<Stroke, stroke_count> strokes_{};
  library::Contents contents_;
  library::Status status_;
};

[[nodiscard]] sample_stream::Registry& registry();
// The library a kit plays; null while it is not loaded. Callback safe.
[[nodiscard]] const Library* current(Kit kit) noexcept;
// The kit's state as last looked at (loading while a background read
// runs), for kernels without its library to report. Callback safe.
[[nodiscard]] library::State published_state(Kit kit) noexcept;
// Starts reading a kit in the background unless it is loaded or being
// read. Control only.
void fetch(Kit kit);

} // namespace oscillate::native_dsp::synth::stick::detail
