#pragma once

// Crate's chopping (ADR-0063 §7, spec 09 §3): pure, deterministic functions
// over a decoded sample. They allocate and may take milliseconds, so they run
// on control or a worker, never on the audio callback.

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::application::crate {

// Two banks of sixteen pads.
inline constexpr std::size_t max_crate_chops = 32;
// Starts closer than this merge into the earlier one.
inline constexpr double chop_merge_seconds = 0.010;
// The media slot every Crate region plays from.
inline constexpr std::string_view crate_sample_media = "sample";
// detect_root's answer when the sample has no stable pitch (C4).
inline constexpr std::uint8_t default_root_key = 60;
// Grid divisions: notes per whole bar of 4/4 (quarters, eighths, sixteenths).
inline constexpr std::uint32_t grid_division_quarter = 4;
inline constexpr std::uint32_t grid_division_eighth = 8;
inline constexpr std::uint32_t grid_division_sixteenth = 16;
inline constexpr double minimum_chop_bpm = 20.0;
inline constexpr double maximum_chop_bpm = 999.0;
// chop_transients' sensitivity for a newly loaded sample.
inline constexpr double default_chop_sensitivity = 0.5;
// The sample's root key parameter (spec 09 §2.1).
inline constexpr std::string_view root_parameter = "crate.root";

using SampleAudio = native_dsp::synth::SynthMediaAsset;
// Start frames in the sample's source frames: at most max_crate_chops,
// strictly increasing, the first at frame 0, none closer than
// chop_merge_seconds to its neighbour.
using ChopStarts = std::vector<std::uint64_t>;

enum class ChopTag : std::uint8_t { kick, snare, hat, other };
// The region tag stored in the document: "kick", "snare", "hat", "other".
[[nodiscard]] std::string_view chop_tag_id(ChopTag tag) noexcept;
// The default pad name: "Kick", "Snare", "Hat", "Chop".
[[nodiscard]] std::string_view chop_tag_name(ChopTag tag) noexcept;

// Detected onsets. Sensitivity 0..1: higher finds more (quieter) onsets.
// When there are more than max_crate_chops the strongest are kept.
[[nodiscard]] foundation::Result<ChopStarts> chop_transients(const SampleAudio& audio,
                                                             float sensitivity);
// Grid lines every `division`th of a 4/4 bar at `bpm`, through
// `first_beat_frame` (lines before it are kept too). The earliest
// max_crate_chops lines are kept.
[[nodiscard]] foundation::Result<ChopStarts> chop_grid(const SampleAudio& audio, double bpm,
                                                       std::uint32_t division,
                                                       std::uint64_t first_beat_frame);
// `count` (1..max_crate_chops) equal chops: start k = floor(k * frames / count).
[[nodiscard]] foundation::Result<ChopStarts> chop_equal(const SampleAudio& audio,
                                                        std::size_t count);

// The chops for the part [first, end) of `audio` an audio clip plays
// (Convert to sampler). `hits` are starts found in that part alone, relative
// to it, as chop_transients gives them for a copy of the part. Returns
// normalised starts in the sample's frames: frame 0 (a lead-in chop the clip
// never plays, unless `first` is within chop_merge_seconds of it), the hits
// shifted by `first`, and `end` when the sample goes on past it, so the last
// played chop ends with the clip. Hits within chop_merge_seconds of a kept
// start or of `end` are dropped and, past max_crate_chops, the latest ones.
// Fails for a part that is empty or outside the sample.
[[nodiscard]] foundation::Result<ChopStarts> range_chop_starts(const ChopStarts& hits,
                                                               std::uint64_t first,
                                                               std::uint64_t end,
                                                               const SampleAudio& audio);

// Manual edits. Each takes normalised starts and returns normalised starts;
// a request that cannot apply (full, too close, index 0, out of range)
// returns `starts` unchanged. Frames clamp into the sample.
[[nodiscard]] ChopStarts add_chop_start(const ChopStarts& starts, std::uint64_t frame,
                                        const SampleAudio& audio);
// Clamps between the neighbours, keeping chop_merge_seconds from each. The
// first start stays at frame 0.
[[nodiscard]] ChopStarts move_chop_start(const ChopStarts& starts, std::size_t index,
                                         std::uint64_t frame, const SampleAudio& audio);
// The first start cannot be removed.
[[nodiscard]] ChopStarts remove_chop_start(const ChopStarts& starts, std::size_t index);

// Tags the chop [start, end) from its band energies (below 150 Hz, 150 Hz to
// 2 kHz, above 6 kHz), its share above 2 kHz and its decay to -20 dB.
[[nodiscard]] ChopTag classify_chop(const SampleAudio& audio, std::uint64_t start,
                                    std::uint64_t end);

// MIDI key of the median voiced pitch, else default_root_key.
[[nodiscard]] std::uint8_t detect_root(const SampleAudio& audio);

struct Rechop {
  std::vector<project::NativeRegion> regions;
  // Slots of previous regions that no longer exist: their crate.chop.NN.*
  // parameters reset. A freed slot may be reused by a new region.
  std::vector<std::uint32_t> freed_slots;
};

// New regions for `starts` (normalised) in media crate_sample_media. A start
// shared with a previous region keeps its slot and, if the user renamed it,
// its name; new starts take the lowest free slots. Every region is tagged
// anew because its end may have moved.
[[nodiscard]] foundation::Result<Rechop>
make_regions(std::span<const project::NativeRegion> previous, const ChopStarts& starts,
             const SampleAudio& audio);

// The starts of `regions` in media crate_sample_media, in time order.
[[nodiscard]] ChopStarts region_starts(std::span<const project::NativeRegion> regions);

// A marker drag: region `index` (in time order) moves to `frame`, clamped as
// move_chop_start. It keeps its slot (and so its parameters and automation)
// and, if the user renamed it, its name; it and the region before it are
// re-tagged because their ends moved. Returns the regions in time order,
// unchanged when the move cannot apply or a region is outside
// crate_sample_media.
[[nodiscard]] std::vector<project::NativeRegion>
move_region(std::span<const project::NativeRegion> regions, std::size_t index, std::uint64_t frame,
            const SampleAudio& audio);

// Chop to MIDI (spec 09 §4): every chop as a note at the sample's tempo.
inline constexpr std::uint8_t chop_note_velocity = 100;
struct ChopClip {
  // Clip-local, in time order; the ids are placeholders the session replaces.
  std::vector<project::MidiNote> notes;
  // Whole 4/4 bars covering the sample.
  timeline::TickPosition length{0};
};
// Region i of `regions` (in time order, cutting a sample of `frames` frames
// at `source_rate`) becomes key first_pad_key + i, from its start converted
// to ticks at `bpm` until the next region's start or the sample's end, at
// chop_note_velocity. Fails without regions, frames or a rate, with a tempo
// out of range, or when the regions are not in time order inside the sample.
[[nodiscard]] foundation::Result<ChopClip>
chop_to_midi(std::span<const project::NativeRegion> regions, std::uint64_t frames,
             std::uint32_t source_rate, double bpm);

// At most `bytes` bytes of UTF-8 `text`, never splitting a character.
[[nodiscard]] std::string utf8_prefix(std::string text, std::size_t bytes);

// Every per-chop parameter of `chop_slots` back at its default, in `values`
// (dense, in the engine's table order): a chop that is gone, or new in a
// slot, starts plain (spec 09 §3).
void reset_chop_slots(const native_dsp::EngineDescriptor& engine, std::vector<double>& values,
                      std::span<const std::uint32_t> chop_slots);

// A newly loaded sample's patch state (spec 09 §3): regions for `starts`
// with none carried over, the chop slots of the previous regions
// (`previous_slots`) and the new ones reset, root_parameter detected from
// `audio`, and the labels replaced: the tempo when `bpm` is in range and the
// sample's name (cut to project::max_native_label_length) when not empty.
struct SampleState {
  std::vector<project::NativeRegion> regions;
  std::vector<double> values; // Dense, in the engine's table order.
  std::vector<project::NativeLabel> labels;
};
[[nodiscard]] foundation::Result<SampleState>
new_sample_state(const native_dsp::EngineDescriptor& engine, std::vector<double> values,
                 std::span<const std::uint32_t> previous_slots, const ChopStarts& starts,
                 const SampleAudio& audio, std::optional<double> bpm, std::string name);

} // namespace oscillate::application::crate
