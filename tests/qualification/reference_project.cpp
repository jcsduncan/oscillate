// The light, heavy, pad and drums reference projects for performance qualification
// (docs/performance/reference-projects.md). Builds the project, saves and
// reopens it, then plays it through the paced fake callback or an explicit
// PipeWire target, measuring callback time and whole-process CPU. Driven by
// qualify_reference_projects.py.

#include "allocation_probe.hpp"
#include "bow_test_library.hpp"
#include "stick_test_library.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/project/mix_roles.hpp"

#if defined(OSCILLATE_QUALIFICATION_VST3)
#include "oscillate/application/vst3/stage_provider.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace synth = oscillate::native_dsp::synth;
#if defined(OSCILLATE_QUALIFICATION_VST3)
namespace plugins = oscillate::plugins;
#endif
namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::uint32_t sample_rate = 48'000;
constexpr std::size_t source_tracks = 32;
constexpr std::size_t heavy_audio_tracks = 16;
constexpr std::size_t max_frames = 1'024;
constexpr std::array<std::size_t, 5> block_sizes{64, 128, 256, 512, 1'024};
constexpr std::size_t max_seconds = 120;
constexpr std::size_t default_seconds = 20;
constexpr std::size_t default_warmup_blocks = 100;
constexpr std::size_t prefetch_frames_per_track = 4'096;
constexpr double microseconds_per_second = 1'000'000.0;
constexpr double percent = 100.0;
constexpr float audible_sample = 0.001F;
constexpr double send_gain = 0.125;
constexpr auto cpu_window = 100ms;
constexpr auto live_ready_timeout = 5s;
constexpr auto live_poll = 10ms;
constexpr double live_timeout_factor = 3.0;
constexpr auto live_timeout_margin = 3s;
constexpr std::uint64_t bus_queue_frames = 4'096;
constexpr std::uint64_t graph_limit_bytes = 1ULL << 20;
constexpr std::uint64_t buffer_limit_bytes = 16ULL << 20;

// A master device: an engine at a factory preset, or at its defaults.
struct MasterDevice {
  std::string_view engine;
  std::string_view preset;
};
// The master chain every reference shares by default: EQ, compressor,
// reverb, limiter.
constexpr std::array<MasterDevice, 4> default_master{
    {{"shape", {}}, {"press", {}}, {"expanse", {}}, {"ceiling", {}}}};
// --master heavy (ADR-0125): a finished master's chain, as a user's
// mastered project runs it: resonance suppression, the limiter with its
// clipper, the exciter at 4x, the analyser, tonal balance, an EQ and the
// loudness meter.
constexpr std::array<MasterDevice, 7> heavy_master{{{"temper", "Default"},
                                                    {"ceiling", "Loud Club"},
                                                    {"gild", "Warm Mids"},
                                                    {"lens", "Mix"},
                                                    {"keel", "Gentle Polish"},
                                                    {"shape", "Presence"},
                                                    {"loudness", "Streaming (Spotify, YouTube)"}}};
enum class Master : std::uint8_t { standard, heavy };
constexpr std::array<std::string_view, 2> master_names{"default", "heavy"};
// Heavy audio tracks each carry an EQ and a compressor; synth tracks a
// saturator and a delay.
constexpr std::array<std::string_view, 2> audio_track_chain{"shape", "press"};
constexpr std::array<std::string_view, 2> synth_track_chain{"forge", "relay"};
// The heavy project's built-in synths: Nova (the main synth) on six tracks,
// then one of each other self-contained engine. Sample-library engines
// (Concert, Fret, Wren) are left out so the project needs no downloads.
constexpr std::array<std::string_view, 16> heavy_synths{
    "nova",  "nova", "nova",  "nova",   "nova",  "nova",  "prism", "ember",
    "drift", "halo", "pulse", "torque", "anvil", "knock", "prism", "ember"};

// The MIDI the synths play: a bar-long chord on even tracks, an eighth-note
// line on odd ones, at the project's 120 BPM (one bar = two seconds).
constexpr std::int64_t ticks_per_bar = 4 * timeline::ticks_per_quarter;
constexpr std::size_t seconds_per_bar = 2;
constexpr std::array<std::uint8_t, 4> chord{48, 55, 60, 64};
constexpr std::array<std::uint8_t, 8> line{60, 63, 67, 70, 72, 70, 67, 63};
constexpr std::uint8_t note_velocity = 100;
constexpr std::int64_t eighth_ticks = timeline::ticks_per_quarter / 2;
constexpr std::int64_t staccato_ticks = eighth_ticks * 3 / 4;

// Generated audio: two partials per track plus a click every beat.
constexpr double base_hz = 110.0;
constexpr double step_hz = 27.5;
constexpr double partial_ratio = 1.5;
constexpr double partial_level = 6'000.0;
constexpr double click_level = 9'000.0;
constexpr std::size_t click_period_frames = sample_rate / 2;
constexpr std::size_t click_frames = 64;
constexpr std::uint16_t pcm_channels = 2;
constexpr std::uint16_t pcm_bytes_per_sample = 2;
constexpr std::uint16_t pcm_bits = 16;
constexpr std::uint32_t riff_format_bytes = 16;
constexpr std::uint32_t riff_header_bytes = 36;
constexpr std::size_t wav_chunk_frames = 4'096;
constexpr std::uint16_t pcm_format = 1;
constexpr std::size_t u16_bytes = 2;
constexpr std::size_t u32_bytes = 4;
constexpr std::size_t stereo_channels = 2;
constexpr std::size_t median = 50;
constexpr std::size_t tail = 99;
constexpr std::size_t hundred = 100;
constexpr double nanoseconds_per_second = 1e9;
constexpr double nanoseconds_per_microsecond = 1'000.0;
constexpr std::size_t bus_count = 2; // groups, and returns

// The pad: Bow's strings in four parts, one held chord note each, a fresh
// bow every bar, as Warm Section plays them (Ensemble, twelve players).
constexpr std::string_view pad_engine = "bow";
constexpr std::size_t pad_preset = 1;
constexpr std::size_t pad_parts = 4;

// The drums: Stick's kit in two parts, a busy funk groove (sixteenth hats
// with an open one, ghost notes, a crash every four bars and a tom fill
// before it) as Natural and House Tight (its production chain on) play it.
constexpr std::string_view drums_engine = "stick";
constexpr std::array<std::size_t, 2> drums_presets{0, 1};

enum class Reference : std::uint8_t { light, heavy, pad, drums };
constexpr std::array<std::string_view, 4> reference_names{"light", "heavy", "pad", "drums"};

struct Options {
  Reference reference = Reference::light;
  std::size_t frames = 64;
  std::size_t seconds = default_seconds;
  std::size_t warmup = default_warmup_blocks;
  std::optional<std::uint64_t> pipewire_target;
  std::optional<fs::path> trace;
  std::optional<fs::path> vst3_effect;
  std::optional<fs::path> vst3_instrument;
  // Audio helpers; the live playback default when not given.
  std::optional<std::size_t> workers;
  fs::path scratch;
  // The drums reference: an installed kit to play as the studio kit, in
  // place of the synthetic one.
  std::optional<fs::path> stick_kit;
  // Every role of the master rebalance set (ADR-0123).
  bool rebalance = false;
  Master master = Master::standard;
  // How blocks are cut into parts (ADR-0127): adaptive, off, or fixed parts
  // of subblock_frames.
  engine::RoutedSubblocks subblocks = engine::RoutedSubblocks::adaptive;
  std::size_t subblock_frames = 0;
};

template <typename T> T take(foundation::Result<T> result, std::string_view what) {
  if (!result)
    throw std::runtime_error(std::string(what) + ": " + result.error().message);
  return std::move(result).value();
}

void take(foundation::Result<void> result, std::string_view what) {
  if (!result)
    throw std::runtime_error(std::string(what) + ": " + result.error().message);
}

std::uint64_t number(std::string_view text) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    throw std::runtime_error("expected a positive number, got " + std::string(text));
  return value;
}

Options parse(int argc, char** argv) {
  constexpr std::string_view usage =
      "usage: oscillate_reference_project --reference light|heavy|pad|drums "
      "--frames 64|128|256|512|1024 --scratch DIR [--seconds N] [--warmup BLOCKS] "
      "[--pipewire-target SERIAL] [--trace FILE] [--vst3-effect BUNDLE] "
      "[--vst3-instrument BUNDLE] [--workers N] [--stick-kit DIR] [--rebalance on|off] "
      "[--master default|heavy] [--subblocks adaptive|off|FRAMES]";
  if (argc % 2 == 0)
    throw std::runtime_error(std::string(usage));
  Options options;
  for (int index = 1; index < argc; index += 2) {
    const std::string_view key(argv[index]);
    const std::string_view value(argv[index + 1]);
    if (key == "--reference") {
      const auto found = std::ranges::find(reference_names, value);
      if (found == reference_names.end())
        throw std::runtime_error("--reference is light, heavy, pad or drums");
      options.reference = static_cast<Reference>(found - reference_names.begin());
    } else if (key == "--frames") {
      options.frames = number(value);
    } else if (key == "--seconds") {
      options.seconds = number(value);
    } else if (key == "--warmup") {
      options.warmup = number(value);
    } else if (key == "--pipewire-target") {
      options.pipewire_target = number(value);
    } else if (key == "--trace") {
      options.trace = fs::path(value);
    } else if (key == "--scratch") {
      options.scratch = fs::path(value);
    } else if (key == "--vst3-effect") {
      options.vst3_effect = fs::path(value);
    } else if (key == "--vst3-instrument") {
      options.vst3_instrument = fs::path(value);
    } else if (key == "--stick-kit") {
      options.stick_kit = fs::path(value);
    } else if (key == "--rebalance") {
      if (value != "on" && value != "off")
        throw std::runtime_error("--rebalance is on or off");
      options.rebalance = value == "on";
    } else if (key == "--master") {
      const auto found = std::ranges::find(master_names, value);
      if (found == master_names.end())
        throw std::runtime_error("--master is default or heavy");
      options.master = static_cast<Master>(found - master_names.begin());
    } else if (key == "--subblocks") {
      if (value == "adaptive") {
        options.subblocks = engine::RoutedSubblocks::adaptive;
      } else if (value == "off") {
        options.subblocks = engine::RoutedSubblocks::off;
      } else {
        options.subblocks = engine::RoutedSubblocks::fixed;
        options.subblock_frames = number(value);
      }
    } else if (key == "--workers") {
      std::size_t workers = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), workers);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          workers > engine::AudioWorkerPool::max_workers)
        throw std::runtime_error("--workers is 0 to " +
                                 std::to_string(engine::AudioWorkerPool::max_workers));
      options.workers = workers;
    } else {
      throw std::runtime_error(std::string(usage));
    }
  }
  if (std::ranges::find(block_sizes, options.frames) == block_sizes.end())
    throw std::runtime_error("--frames is 64, 128, 256, 512 or 1024");
  if (options.seconds > max_seconds)
    throw std::runtime_error("--seconds is at most 120");
  if (options.scratch.empty() || !fs::is_directory(options.scratch))
    throw std::runtime_error("--scratch must name an existing directory");
  if ((options.vst3_effect || options.vst3_instrument) && options.reference != Reference::heavy)
    throw std::runtime_error("VST3 plug-ins belong to the heavy reference");
#if !defined(OSCILLATE_QUALIFICATION_VST3)
  if (options.vst3_effect || options.vst3_instrument)
    throw std::runtime_error("this build has no VST3 host");
#endif
  return options;
}

std::span<const MasterDevice> master_chain(Master master) {
  if (master == Master::heavy)
    return heavy_master;
  return default_master;
}

// The factory preset a master device names (none: the defaults).
std::optional<std::size_t> preset_index(const MasterDevice& device) {
  if (device.preset.empty())
    return std::nullopt;
  const auto* engine = oscillate::native_dsp::effects::find_engine(device.engine);
  if (engine == nullptr)
    throw std::runtime_error("no built-in effect " + std::string(device.engine));
  const auto presets = engine->descriptor->presets;
  const auto found =
      std::ranges::find(presets, device.preset, &oscillate::native_dsp::Preset::name);
  if (found == presets.end())
    throw std::runtime_error("no preset " + std::string(device.preset) + " for " +
                             std::string(device.engine));
  return static_cast<std::size_t>(found - presets.begin());
}

// Seconds of generated media: warmup and measured blocks, rounded up, plus one.
std::size_t source_seconds(const Options& options) {
  const auto frames = options.warmup * options.frames + options.seconds * sample_rate;
  return (frames + sample_rate - 1) / sample_rate + 1;
}

void put(std::ofstream& out, std::uint32_t value, std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index)
    out.put(static_cast<char>((value >> (8U * index)) & 0xffU));
}

fs::path write_wav(const fs::path& directory, std::size_t track, std::size_t seconds) {
  const auto path = directory / ("track-" + std::to_string(track) + ".wav");
  const auto frames = seconds * sample_rate;
  const auto data_bytes = static_cast<std::uint32_t>(frames * pcm_channels * pcm_bytes_per_sample);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "RIFF";
  put(out, riff_header_bytes + data_bytes, u32_bytes);
  out << "WAVEfmt ";
  put(out, riff_format_bytes, u32_bytes);
  put(out, pcm_format, u16_bytes);
  put(out, pcm_channels, u16_bytes);
  put(out, sample_rate, u32_bytes);
  put(out, sample_rate * pcm_channels * pcm_bytes_per_sample, u32_bytes);
  put(out, pcm_channels * pcm_bytes_per_sample, u16_bytes);
  put(out, pcm_bits, u16_bytes);
  out << "data";
  put(out, data_bytes, u32_bytes);
  const double hz = base_hz + step_hz * static_cast<double>(track);
  std::vector<char> chunk;
  chunk.reserve(wav_chunk_frames * pcm_channels * pcm_bytes_per_sample);
  for (std::size_t first = 0; first < frames; first += wav_chunk_frames) {
    chunk.clear();
    for (std::size_t frame = first; frame < std::min(frames, first + wav_chunk_frames); ++frame) {
      const double time = static_cast<double>(frame) / sample_rate;
      double value = partial_level * (std::sin(2.0 * std::numbers::pi * hz * time) +
                                      std::sin(2.0 * std::numbers::pi * hz * partial_ratio * time));
      if (frame % click_period_frames < click_frames)
        value += click_level;
      const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value)));
      for (std::uint16_t channel = 0; channel < pcm_channels; ++channel) {
        chunk.push_back(static_cast<char>(sample & 0xffU));
        chunk.push_back(static_cast<char>(sample >> 8U));
      }
    }
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  }
  if (!out)
    throw std::runtime_error("cannot write a generated WAV");
  return path;
}

std::vector<project::MidiNote> synth_notes(std::size_t track, std::size_t bars) {
  std::vector<project::MidiNote> notes;
  std::uint64_t serial = 1;
  const auto next_id = [&serial] {
    return take(project::NoteId::from_bits({0, serial++}), "note id");
  };
  for (std::size_t bar = 0; bar < bars; ++bar) {
    const auto start = static_cast<std::int64_t>(bar) * ticks_per_bar;
    if (track % 2 == 0) {
      for (const auto key : chord)
        notes.push_back({next_id(), {start}, {start + ticks_per_bar}, key, note_velocity});
    } else {
      for (std::size_t step = 0; step < line.size(); ++step) {
        const auto at = start + static_cast<std::int64_t>(step) * eighth_ticks;
        notes.push_back({next_id(), {at}, {at + staccato_ticks}, line[step], note_velocity});
      }
    }
  }
  return notes;
}

// The drums' groove, a bar of sixteenths: the key of each step's hand and
// its velocity (0 rests), the kick under it, and a crash and a tom fill
// every four bars.
constexpr std::int64_t sixteenth_ticks = timeline::ticks_per_quarter / 4;
constexpr std::size_t groove_steps = 16;
constexpr std::uint8_t kick_key = 36;
constexpr std::uint8_t snare_key = 38;
constexpr std::uint8_t closed_hat_key = 42;
constexpr std::uint8_t open_hat_key = 46;
constexpr std::uint8_t crash_key = 49;
constexpr std::array<std::uint8_t, 4> fill_keys{50, 48, 45, 43};
constexpr std::uint8_t accent = 112;
constexpr std::uint8_t ghost = 38;
constexpr std::uint8_t hat_soft = 70;
constexpr std::array<std::uint8_t, groove_steps> hand_keys{
    closed_hat_key, closed_hat_key, closed_hat_key, snare_key,      snare_key,      closed_hat_key,
    closed_hat_key, snare_key,      closed_hat_key, closed_hat_key, closed_hat_key, snare_key,
    snare_key,      closed_hat_key, open_hat_key,   snare_key};
constexpr std::array<std::uint8_t, groove_steps> hand_velocities{
    accent, hat_soft, accent, ghost, accent, hat_soft, accent, ghost,
    accent, hat_soft, accent, ghost, accent, hat_soft, accent, ghost};
constexpr std::array<bool, groove_steps> kick_steps{true,  false, false, false, false, false,
                                                    true,  false, true,  false, false, true,
                                                    false, false, false, false};
constexpr std::size_t bars_per_phrase = 4;
constexpr std::size_t fill_from_step = 12;

std::vector<project::MidiNote> groove_notes(std::size_t bars) {
  std::vector<project::MidiNote> notes;
  std::uint64_t serial = 1;
  const auto add = [&](std::int64_t at, std::uint8_t key, std::uint8_t velocity) {
    notes.push_back({take(project::NoteId::from_bits({0, serial++}), "note id"),
                     {at},
                     {at + sixteenth_ticks},
                     key,
                     velocity});
  };
  for (std::size_t bar = 0; bar < bars; ++bar) {
    const bool fill = bar % bars_per_phrase == bars_per_phrase - 1;
    for (std::size_t step = 0; step < groove_steps; ++step) {
      const auto at = static_cast<std::int64_t>(bar) * ticks_per_bar +
                      static_cast<std::int64_t>(step) * sixteenth_ticks;
      if (fill && step >= fill_from_step) {
        add(at, fill_keys[step - fill_from_step], accent);
      } else {
        add(at, hand_keys[step], hand_velocities[step]);
      }
      if (kick_steps[step]) {
        add(at, kick_key, accent);
      }
      if (step == 0 && bar % bars_per_phrase == 0) {
        add(at, crash_key, accent);
      }
    }
  }
  return notes;
}

#if defined(OSCILLATE_QUALIFICATION_VST3)
// The VST3 instrument plays middle C on every eighth note: the synthetic
// fixture instrument accepts no other key.
constexpr std::uint8_t vst3_pitch = 60;

std::vector<project::MidiNote> pulse_notes(std::size_t bars) {
  std::vector<project::MidiNote> notes;
  std::uint64_t serial = 1;
  for (std::size_t bar = 0; bar < bars; ++bar) {
    for (std::size_t step = 0; step < line.size(); ++step) {
      const auto at = static_cast<std::int64_t>(bar) * ticks_per_bar +
                      static_cast<std::int64_t>(step) * eighth_ticks;
      notes.push_back({take(project::NoteId::from_bits({0, serial++}), "note id"),
                       {at},
                       {at + staccato_ticks},
                       vst3_pitch,
                       note_velocity});
    }
  }
  return notes;
}

struct ScannedPlugin {
  plugins::ScanResult scan;
  plugins::Vst3Class klass;
};

ScannedPlugin scan_plugin(const fs::path& bundle, bool instrument) {
  plugins::Vst3Scanner scanner(VST3_QUALIFICATION_HELPER);
  auto scan = scanner.scan(bundle);
  if (!scan.ok())
    throw std::runtime_error("VST3 scan failed: " + scan.diagnostic);
  const auto klass = std::ranges::find_if(scan.metadata.classes, [instrument](const auto& found) {
    return found.category == "Audio Module Class" &&
           (found.subcategories.find("Instrument") != std::string::npos) == instrument;
  });
  if (klass == scan.metadata.classes.end())
    throw std::runtime_error("the bundle has no matching VST3 class");
  const auto chosen = *klass;
  return {std::move(scan), chosen};
}

app::Vst3InsertIdentity identity_from(const ScannedPlugin& plugin) {
  return {.class_id = plugin.klass.id,
          .vendor = plugin.klass.vendor,
          .name = plugin.klass.name,
          .version = plugin.klass.version,
          .canonical_bundle_path = plugin.scan.binary.canonical_bundle.string(),
          .binary_sha256 = plugin.scan.binary.sha256};
}
#endif

struct Vst3Parts {
#if defined(OSCILLATE_QUALIFICATION_VST3)
  std::optional<ScannedPlugin> effect;
  std::optional<ScannedPlugin> instrument;
#endif
};

// The master rebalance roles (ADR-0123) the references must have, by the
// engines these projects use: Stick, Pulse, Anvil and Knock play drums,
// Torque bass, a recording and every other synth music. A group bus takes
// the role all it sums share, else none; a return none. Each source track
// carries its role's stage; buses none.
constexpr std::array<std::string_view, 4> reference_drum_engines{"stick", "pulse", "anvil",
                                                                 "knock"};
constexpr std::string_view reference_bass_engine = "torque";

void check_roles(const project::Snapshot& snapshot, std::span<const project::TrackId> groups) {
  const auto roles = project::mix_roles(snapshot);
  const auto expected = [](const project::Track& track) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (patch == nullptr)
      return project::MixRole::music;
    if (std::ranges::find(reference_drum_engines, patch->engine) != reference_drum_engines.end())
      return project::MixRole::drums;
    return patch->engine == reference_bass_engine ? project::MixRole::bass
                                                  : project::MixRole::music;
  };
  std::size_t staged = 0;
  for (std::size_t index = 0; index < snapshot.tracks.size(); ++index) {
    const auto& track = snapshot.tracks[index];
    auto want = project::MixRole::none;
    if (track.kind == project::TrackKind::audio || track.kind == project::TrackKind::instrument) {
      want = expected(track);
    } else if (std::ranges::find(groups, track.id) != groups.end()) {
      std::optional<project::MixRole> shared;
      bool mixed = false;
      for (const auto& member : snapshot.tracks) {
        if (member.main_output != track.id)
          continue;
        mixed = mixed || (shared && *shared != expected(member));
        shared = expected(member);
      }
      want = shared && !mixed ? *shared : project::MixRole::none;
    }
    if (roles[index].role != want)
      throw std::runtime_error("a reference track has the wrong rebalance role");
    const bool source =
        track.kind == project::TrackKind::audio || track.kind == project::TrackKind::instrument;
    if (roles[index].stage != (source ? want : project::MixRole::none))
      throw std::runtime_error("a reference track has the wrong rebalance stage");
    staged += source ? 1U : 0U;
  }
  std::cout << "roles checked: " << staged << " rebalance stages\n";
}

// Every role moved: a gain and all three bands, so every stage runs its EQ.
project::MasterRebalance rebalance_all_roles() {
  constexpr double gain_db = 1.5;
  constexpr double low_db = 2.0;
  constexpr double mid_db = -1.5;
  constexpr double high_db = 1.0;
  project::MasterRebalance rebalance;
  rebalance.enabled = true;
  for (auto& role : rebalance.roles) {
    role.gain_db = gain_db;
    role.low_db = low_db;
    role.mid_db = mid_db;
    role.high_db = high_db;
  }
  return rebalance;
}

struct Workload {
  std::size_t audio_tracks = 0;
  std::size_t native_synths = 0;
  std::size_t native_inserts = 0;
  std::size_t vst3_effects = 0;
  std::size_t vst3_instruments = 0;
  std::size_t notes = 0;
};

Workload build_project(const fs::path& root, const fs::path& media, const Options& options,
                       const Vst3Parts& vst3) {
  auto session = take(app::ProjectSession::create(root), "project create");
  const auto group_a = take(session->tracks().add_bus_track(project::TrackKind::group_bus), "bus");
  const auto group_b = take(session->tracks().add_bus_track(project::TrackKind::group_bus), "bus");
  const auto return_a =
      take(session->tracks().add_bus_track(project::TrackKind::return_bus), "bus");
  const auto return_b =
      take(session->tracks().add_bus_track(project::TrackKind::return_bus), "bus");
  const bool heavy = options.reference == Reference::heavy;
  const bool pad = options.reference == Reference::pad;
  const bool drums = options.reference == Reference::drums;
  bool vst3_instrument = false;
#if defined(OSCILLATE_QUALIFICATION_VST3)
  vst3_instrument = vst3.instrument.has_value();
#endif
  const auto seconds = source_seconds(options);
  const auto bars = (seconds + seconds_per_bar - 1) / seconds_per_bar;
  Workload workload;
  const auto route = [&](project::TrackId track, std::size_t index) {
    const bool first_half = index < source_tracks / 2;
    take(session->routing().set_main_output(track, first_half ? group_a : group_b), "route");
    (void)take(session->routing().add_send(track, first_half ? return_a : return_b, send_gain),
               "send");
  };
  const auto chain = [&](project::TrackId track, std::span<const std::string_view> engines) {
    for (std::size_t slot = 0; slot < engines.size(); ++slot) {
      (void)take(session->effects().add_effect_engine(track, slot, engines[slot]),
                 "track effect " + std::string(engines[slot]));
      ++workload.native_inserts;
    }
  };
  const auto audio_count = pad || drums ? 0 : heavy ? heavy_audio_tracks : source_tracks;
  for (std::size_t index = 0; index < audio_count; ++index) {
    const auto wav = write_wav(media, index, seconds);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
      throw std::runtime_error("cannot open a generated WAV");
    auto imported = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    const auto track = take(std::move(imported), "WAV import");
    fs::remove(wav);
    route(track, index);
    ++workload.audio_tracks;
    if (heavy)
      chain(track, audio_track_chain);
  }
  if (heavy) {
    const auto synth_count = heavy_synths.size() - (vst3_instrument ? 1U : 0U);
    for (std::size_t index = 0; index < synth_count; ++index) {
      const auto engine_id = heavy_synths[index];
      if (synth::find_engine(engine_id) == nullptr)
        throw std::runtime_error("synth engine " + std::string(engine_id) + " is not registered");
      const auto track = take(
          session->instruments().add_native_instrument_track(engine_id, std::nullopt), "synth");
      auto notes = synth_notes(index, bars);
      workload.notes += notes.size();
      (void)take(
          session->midi_clips().add_midi_clip_with_notes(
              track, {0}, {static_cast<std::int64_t>(bars) * ticks_per_bar}, std::move(notes)),
          "MIDI clip");
      route(track, audio_count + index);
      chain(track, synth_track_chain);
      ++workload.native_synths;
    }
  }
  if (pad) {
    // Bow plays its synthetic test library: reading it costs what reading
    // the installed one does, and the run needs no download.
    (void)bow_test::install();
    for (std::size_t part = 0; part < pad_parts; ++part) {
      const auto track =
          take(session->instruments().add_native_instrument_track(pad_engine, pad_preset), "Bow");
      std::vector<project::MidiNote> notes;
      for (std::size_t bar = 0; bar < bars; ++bar) {
        const auto start = static_cast<std::int64_t>(bar) * ticks_per_bar;
        notes.push_back({take(project::NoteId::from_bits({0, bar + 1}), "note id"),
                         {start},
                         {start + ticks_per_bar},
                         chord[part],
                         note_velocity});
      }
      workload.notes += notes.size();
      (void)take(
          session->midi_clips().add_midi_clip_with_notes(
              track, {0}, {static_cast<std::int64_t>(bars) * ticks_per_bar}, std::move(notes)),
          "MIDI clip");
      route(track, part);
      ++workload.native_synths;
    }
  }
  if (drums) {
    // Stick plays its synthetic kit, whose hits stream as the installed
    // ones do, unless a run names an installed kit.
    if (options.stick_kit) {
      const auto status = synth::stick::library::load(synth::stick::Kit::naked, *options.stick_kit);
      if (status.state != synth::stick::library::State::ready)
        throw std::runtime_error("the Stick kit did not load: " + status.message);
    } else {
      (void)stick_test::install();
    }
    for (std::size_t part = 0; part < drums_presets.size(); ++part) {
      const auto track = take(
          session->instruments().add_native_instrument_track(drums_engine, drums_presets[part]),
          "Stick");
      auto notes = groove_notes(bars);
      workload.notes += notes.size();
      (void)take(
          session->midi_clips().add_midi_clip_with_notes(
              track, {0}, {static_cast<std::int64_t>(bars) * ticks_per_bar}, std::move(notes)),
          "MIDI clip");
      route(track, part);
      ++workload.native_synths;
    }
  }
#if defined(OSCILLATE_QUALIFICATION_VST3)
  if (vst3.instrument) {
    const auto track =
        take(session->instruments().create_vst3_instrument_track(identity_from(*vst3.instrument)),
             "VST3 instrument");
    auto notes = pulse_notes(bars);
    workload.notes += notes.size();
    (void)take(session->midi_clips().add_midi_clip_with_notes(
                   track, {0}, {static_cast<std::int64_t>(bars) * ticks_per_bar}, std::move(notes)),
               "VST3 MIDI clip");
    route(track, source_tracks - 1);
    ++workload.vst3_instruments;
  }
  if (vst3.effect) {
    // On the first audio track, after its native chain.
    const auto& first = session->snapshot().tracks;
    const auto audio = std::ranges::find(first, project::TrackKind::audio, &project::Track::kind);
    if (audio == first.end())
      throw std::runtime_error("no audio track for the VST3 effect");
    (void)take(session->effects().insert_vst3_plugin(audio->id, audio_track_chain.size(),
                                                     identity_from(*vst3.effect)),
               "VST3 effect");
    ++workload.vst3_effects;
  }
#else
  (void)vst3;
#endif
  const auto master = master_chain(options.master);
  for (std::size_t slot = 0; slot < master.size(); ++slot)
    (void)take(session->effects().add_effect_engine(project::MasterSignalPath{}, slot,
                                                    master[slot].engine,
                                                    preset_index(master[slot])),
               "master effect");
  check_roles(session->snapshot(), std::array{group_a, group_b});
  if (options.rebalance)
    take(session->tracks().set_master_rebalance(rebalance_all_roles()), "rebalance");
  take(session->saving().save(), "save");
  return workload;
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

double process_cpu_seconds() {
  timespec now{};
  ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) +
         static_cast<double>(now.tv_nsec) / nanoseconds_per_second;
}

// Whole-process CPU in fixed windows, from its own thread: 100% is one core.
class CpuSampler {
public:
  CpuSampler()
      : thread_([this](const std::stop_token& stop) {
          auto last_cpu = process_cpu_seconds();
          auto last_wall = std::chrono::steady_clock::now();
          while (!stop.stop_requested()) {
            std::this_thread::sleep_for(cpu_window);
            const auto cpu = process_cpu_seconds();
            const auto wall = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(wall - last_wall).count();
            windows_.push_back(percent * (cpu - last_cpu) / seconds);
            last_cpu = cpu;
            last_wall = wall;
          }
        }) {}
  // Joins the sampler; the windows are then stable.
  std::vector<double> finish() {
    thread_.request_stop();
    thread_.join();
    std::ranges::sort(windows_);
    return windows_;
  }

private:
  std::vector<double> windows_;
  std::jthread thread_;
};

double percentile(std::span<const double> sorted, std::size_t numerator) {
  if (sorted.empty())
    return 0.0;
  const auto index = (sorted.size() * numerator + hundred - 1) / hundred - 1;
  return sorted[std::min(index, sorted.size() - 1)];
}

void print_cpu(std::span<const double> windows, double cpu_seconds, double wall_seconds) {
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
  std::cout << " process_cpu_percent=" << percent * cpu_seconds / wall_seconds
            << " cpu_window_p50=" << percentile(windows, median)
            << " cpu_window_p99=" << percentile(windows, tail)
            << " cpu_window_max=" << (windows.empty() ? 0.0 : windows.back())
            << " cpu_windows=" << windows.size() << " max_rss_kib=" << usage.ru_maxrss
            << " cores=" << std::thread::hardware_concurrency();
}

void measure(const Options& options, const fs::path& root, const Vst3Parts& vst3) {
  auto session = take(app::ProjectSession::open(root), "reopen");
  const auto& snapshot = session->snapshot();
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate), "model");
  engine::RoutedMixerLimits limits{
      {bus_queue_frames, bus_queue_frames}, max_frames, graph_limit_bytes, buffer_limit_bytes};
  limits.subblocks = options.subblocks;
  limits.fixed_subblock_frames = options.subblock_frames;
#if defined(OSCILLATE_QUALIFICATION_VST3)
  plugins::Vst3InstrumentTrackProvider instruments;
  plugins::Vst3TrackInsertProvider effects;
  std::unique_ptr<app::TrackInsertProvider> provider;
  for (const auto& track : snapshot.tracks) {
    if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
      instruments.set_scan_receipt(source->id, vst3.instrument->scan);
    for (const auto& slot : track.effects) {
      if (const auto* insert = std::get_if<project::Vst3PluginInstance>(&slot.processor))
        effects.set_scan_receipt(insert->id, vst3.effect->scan);
    }
  }
  provider = std::make_unique<plugins::Vst3StageProvider>(instruments, effects);
#else
  std::unique_ptr<app::TrackInsertProvider> provider;
  (void)vst3;
#endif
  if (options.trace && !foundation::trace::start())
    throw std::runtime_error("cannot start the performance trace");
  // The helpers live playback renders with (ADR-0048): a sink starts them at
  // the callback's priority, the fake callback as ordinary threads.
  engine::AudioWorkerPool workers(
      options.workers.value_or(engine::AudioWorkerPool::default_worker_count()));
  app::RoutedPlaybackControl control(model, limits, provider.get());
  take(control.submit_current(), "graph");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control,
                                                     source_tracks * prefetch_frames_per_track),
                     "source");
  audio::TransportCursor cursor;
  const auto total_blocks = options.warmup + options.seconds * sample_rate / options.frames;
  const auto target_frames = total_blocks * options.frames;
  const double period_us =
      static_cast<double>(options.frames) * microseconds_per_second / sample_rate;
  std::cout << std::fixed << std::setprecision(3);
  if (options.pipewire_target) {
    std::atomic<bool> allowed{false};
    const audio::TransportPermission permission{
        &allowed, [](const void* context) noexcept {
          return static_cast<const std::atomic<bool>*>(context)->load(std::memory_order_acquire);
        }};
    audio::PipeWireOutput output(source->render_source(&workers), cursor, permission);
    take(output.start(sample_rate, static_cast<std::uint32_t>(options.frames),
                      *options.pipewire_target),
         "PipeWire start");
    const auto ready_by = std::chrono::steady_clock::now() + live_ready_timeout;
    auto status = output.status();
    while (std::chrono::steady_clock::now() < ready_by &&
           !(status.state == audio::OutputState::streaming && status.observed_block_frames)) {
      if (status.state == audio::OutputState::error ||
          status.state == audio::OutputState::disconnected)
        throw std::runtime_error("the PipeWire target failed before playback");
      std::this_thread::sleep_for(live_poll);
      status = output.status();
    }
    if (status.state != audio::OutputState::streaming || !status.observed_block_frames)
      throw std::runtime_error("PipeWire did not start streaming");
    const auto run_for = std::chrono::duration<double>(static_cast<double>(target_frames) /
                                                       static_cast<double>(sample_rate));
    const auto finish_by = std::chrono::steady_clock::now() +
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               run_for * live_timeout_factor + live_timeout_margin);
    CpuSampler sampler;
    const auto cpu_started = process_cpu_seconds();
    const auto wall_started = std::chrono::steady_clock::now();
    allowed.store(true, std::memory_order_release);
    while (cursor.completed_snapshot().next_sample < target_frames &&
           std::chrono::steady_clock::now() < finish_by) {
      status = output.status();
      if (status.state != audio::OutputState::streaming || source->status().worker_failed)
        break;
      std::this_thread::sleep_for(live_poll);
    }
    allowed.store(false, std::memory_order_release);
    const auto cpu_seconds = process_cpu_seconds() - cpu_started;
    const auto wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_started).count();
    const auto windows = sampler.finish();
    status = output.status();
    output.stop();
    const auto completed = cursor.completed_snapshot().next_sample;
    const auto source_status = source->status();
    std::cout << "result mode=live frames=" << options.frames
              << " observed_frames=" << status.observed_block_frames.value_or(0)
              << " graph_rate=" << status.observed_graph_rate.value_or(0)
              << " completed_frames=" << completed << " target_frames=" << target_frames
              << " render_failures=" << status.render_failures
              << " missing_frames=" << source_status.missing_frames
              << " worker_failed=" << source_status.worker_failed
              << " callback_blocks=" << status.measured_callback_blocks.value_or(0)
              << " over_period=" << status.callback_over_period_blocks.value_or(0)
              << " workers=" << status.realtime_workers << " max_us="
              << static_cast<double>(status.max_callback_duration_ns.value_or(0)) /
                     nanoseconds_per_microsecond
              << " xruns="
              << (status.xruns ? std::to_string(*status.xruns) : std::string("unavailable"))
              << " period_us=" << period_us;
    print_cpu(windows, cpu_seconds, wall_seconds);
    std::cout << '\n';
    if (completed < target_frames || status.render_failures != 0 ||
        source_status.missing_frames != 0 || source_status.worker_failed)
      throw std::runtime_error("live playback failed a correctness check");
  } else {
    const auto callback = source->render_source(&workers);
    workers.start_owned_threads(workers.wanted_workers());
    std::array<engine::StereoFrame, max_frames> stereo{};
    std::array<float, max_frames * stereo_channels> interleaved{};
    const auto frame_span = std::span(stereo).first(options.frames);
    const auto sample_span = std::span(interleaved).first(options.frames * stereo_channels);
    std::vector<double> blocks;
    blocks.reserve(total_blocks - options.warmup);
    std::size_t render_failures = 0;
    std::size_t audible_blocks = 0;
    std::size_t over_period = 0;
    const auto period = std::chrono::duration<double>(static_cast<double>(options.frames) /
                                                      static_cast<double>(sample_rate));
    foundation::trace::name_thread("audio callback");
    // Marked as the device callbacks are, so the effects treat it as one.
    const foundation::RealtimeThreadScope realtime;
    std::optional<CpuSampler> sampler;
    double cpu_started = 0.0;
    auto wall_started = std::chrono::steady_clock::now();
    auto deadline = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < total_blocks; ++block) {
      if (block == options.warmup) {
        // Marks where the measured blocks start, for the trace's summaries.
        foundation::trace::instant("qualification.measured");
        sampler.emplace();
        cpu_started = process_cpu_seconds();
        wall_started = std::chrono::steady_clock::now();
      }
      const auto started = std::chrono::steady_clock::now();
      begin_allocation_probe();
      const auto result = [&] {
        // As the device outputs frame their render, so the trace's blocks
        // match a sink's.
        OSCILLATE_TRACE_SPAN("audio.render", options.frames);
        return audio::render_transport_block(callback, cursor, true, frame_span, sample_span);
      }();
      end_allocation_probe();
      const double elapsed =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
              .count();
      render_failures += result == audio::BlockResult::rendered ? 0U : 1U;
      if (std::ranges::any_of(sample_span, [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error("non-finite output");
      if (std::ranges::any_of(sample_span,
                              [](float sample) { return std::abs(sample) > audible_sample; }))
        ++audible_blocks;
      if (block >= options.warmup) {
        blocks.push_back(elapsed);
        over_period += elapsed > period_us ? 1U : 0U;
      }
      deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
      std::this_thread::sleep_until(deadline);
    }
    const auto cpu_seconds = process_cpu_seconds() - cpu_started;
    const auto wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_started).count();
    const auto windows = sampler->finish();
    const auto helpers = workers.attached_workers();
    workers.stop_owned_threads();
    const auto source_status = source->status();
    std::ranges::sort(blocks);
    double total = 0.0;
    for (const auto value : blocks)
      total += value;
    std::cout << "result mode=fake frames=" << options.frames << " blocks=" << blocks.size()
              << " mean_us=" << total / static_cast<double>(blocks.size())
              << " p50_us=" << percentile(blocks, median) << " p99_us=" << percentile(blocks, tail)
              << " max_us=" << blocks.back() << " over_period=" << over_period
              << " workers=" << helpers << " period_us=" << period_us
              << " render_failures=" << render_failures
              << " missing_frames=" << source_status.missing_frames
              << " worker_failed=" << source_status.worker_failed
              << " audible_blocks=" << audible_blocks
              << " callback_allocations=" << allocation_probe_count();
    print_cpu(windows, cpu_seconds, wall_seconds);
    std::cout << '\n';
    if (render_failures != 0 || source_status.missing_frames != 0 || source_status.worker_failed ||
        audible_blocks == 0)
      throw std::runtime_error("playback failed a correctness check");
  }
  if (options.trace) {
    const auto written = foundation::trace::stop_and_write(*options.trace);
    if (!written)
      throw std::runtime_error("cannot write the trace: " + written.error().message);
  }
#if defined(OSCILLATE_QUALIFICATION_VST3)
  if (vst3.instrument && !instruments.stop_after_join()) {
    const auto fault = control.processor_fault();
    throw std::runtime_error("the VST3 instrument did not stop cleanly" +
                             (fault ? ": " + fault->message : std::string()));
  }
#endif
}

void run(const Options& options) {
  const auto directory = options.scratch / ("reference-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(options.frames));
  fs::remove_all(directory);
  fs::create_directories(directory / "media");
  Vst3Parts vst3;
#if defined(OSCILLATE_QUALIFICATION_VST3)
  if (options.reference == Reference::heavy) {
    vst3.effect = scan_plugin(options.vst3_effect.value_or(VST3_QUALIFICATION_GAIN_BUNDLE), false);
    vst3.instrument =
        scan_plugin(options.vst3_instrument.value_or(VST3_QUALIFICATION_INSTRUMENT_BUNDLE), true);
  }
#endif
  try {
    const auto root = directory / "project";
    const auto workload = build_project(root, directory / "media", options, vst3);
    std::cout << "workload reference="
              << reference_names[static_cast<std::size_t>(options.reference)]
              << " frames=" << options.frames << " measured_seconds=" << options.seconds
              << " warmup_blocks=" << options.warmup << " audio_tracks=" << workload.audio_tracks
              << " native_synths=" << workload.native_synths
              << " native_inserts=" << workload.native_inserts
              << " vst3_effects=" << workload.vst3_effects
              << " vst3_instruments=" << workload.vst3_instruments << " notes=" << workload.notes
              << " master=" << master_names[static_cast<std::size_t>(options.master)]
              << " master_devices=" << master_chain(options.master).size()
              << " groups=" << bus_count << " returns=" << bus_count
              << " rebalance=" << (options.rebalance ? "on" : "off") << '\n'
              << std::flush;
    measure(options, root, vst3);
  } catch (...) {
    fs::remove_all(directory);
    throw;
  }
  fs::remove_all(directory);
}
} // namespace

int main(int argc, char** argv) {
  try {
    run(parse(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Reference project: " << error.what() << '\n';
    return 1;
  }
}
