// The Constellation's analysis (ADR-0077): each sound rendered playing one
// short phrase, measured, placed against the library and cached by key.
#include "oscillate/application/sound_map.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/native_dsp/spectrum.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/persistence/effect_preset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <fcntl.h>

namespace oscillate::application::sound_map {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using Json = nlohmann::json;
using native_dsp::StereoSpectrum;
using native_dsp::StereoSpectrumAnalyzer;

// The phrase plays at a steady 120 BPM in 4/4, so tempo-synced patches
// (arps, synced LFOs) render the same every time.
constexpr double phrase_bpm = 120.0;
constexpr double seconds_per_minute = 60.0;
constexpr double phrase_beats_per_bar = 4.0;
constexpr std::uint64_t phrase_generation = 1;
// Ids for the phrase's notes; any non-nil ids the render never reuses.
constexpr foundation::IdBits phrase_clip{1, 1};
constexpr std::uint64_t first_note_id = 1;

constexpr std::string_view cache_key_tag = "oscillate constellation";
constexpr std::size_t hex_digits = 16;
constexpr int hex_base = 16;
constexpr std::size_t feature_fields = 5;

constexpr std::size_t window = StereoSpectrum::window_frames;
constexpr std::size_t spectrum_hop = window / 2;
// Interior bins only: DC and Nyquist carry no pitch.
constexpr std::size_t first_bin = 1;
constexpr std::size_t last_bin = StereoSpectrum::bin_count - 1;
// Spectral flux of normalised band spectra lies in 0..2.
constexpr double max_flux = 2.0;

[[nodiscard]] double clamp01(double value) noexcept {
  return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

[[nodiscard]] bool has_imported_media(const project::NativeSynthDocument& document) {
  return std::ranges::any_of(document.media, [](const project::NativeMediaSlot& slot) {
    return std::holds_alternative<project::ManagedAudioReference>(slot.media);
  });
}

[[nodiscard]] Result<Sound> resolved(const project::NativeInstrumentPatch& patch) {
  auto instrument = native_dsp::synth::resolve_native_instrument(patch);
  if (!instrument)
    return Result<Sound>::failure(instrument.error());
  return Result<Sound>::success(
      {std::move(instrument.value().values), std::move(instrument.value().document)});
}

// One analysis window of the mid signal: its energy, log-frequency centroid
// (log2 Hz, 0 when nothing qualifies), and band shape for movement.
struct Frame {
  std::size_t start = 0;
  double energy = 0.0;
  double centroid = 0.0;
  double weight = 0.0; // The centroid's magnitude sum; 0 means no centroid.
  std::array<double, movement_bands> bands{};
};

[[nodiscard]] std::array<double, movement_bands + 1> band_edges() noexcept {
  std::array<double, movement_bands + 1> edges{};
  const double ratio = std::log(movement_high_hz / movement_low_hz);
  for (std::size_t band = 0; band <= movement_bands; ++band)
    edges[band] = movement_low_hz *
                  std::exp(ratio * static_cast<double>(band) / static_cast<double>(movement_bands));
  return edges;
}

[[nodiscard]] Frame analyse_frame(std::span<const engine::StereoFrame> mid_side,
                                  std::uint32_t sample_rate, std::size_t start) {
  Frame frame;
  frame.start = start;
  const auto spectrum = StereoSpectrumAnalyzer::analyze(mid_side, sample_rate);
  if (!spectrum)
    return frame;
  const auto& mid = spectrum->left;
  const double bin_hz = static_cast<double>(sample_rate) / static_cast<double>(window);
  float loudest = 0.0F;
  for (std::size_t bin = first_bin; bin < last_bin; ++bin) {
    loudest = std::max(loudest, mid[bin]);
    frame.energy += static_cast<double>(mid[bin]) * mid[bin];
  }
  static const auto edges = band_edges();
  double weighted = 0.0;
  const double floor = static_cast<double>(loudest) * spectrum_floor;
  for (std::size_t bin = first_bin; bin < last_bin; ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    const double magnitude = mid[bin];
    if (hz >= centroid_low_hz && hz <= centroid_high_hz && magnitude >= floor && magnitude > 0.0) {
      weighted += magnitude * std::log2(hz);
      frame.weight += magnitude;
    }
    if (hz >= edges.front() && hz < edges.back()) {
      const auto band =
          static_cast<std::size_t>(std::ranges::upper_bound(edges, hz) - edges.begin() - 1);
      frame.bands[std::min(band, movement_bands - 1)] += magnitude;
    }
  }
  if (frame.weight > 0.0)
    frame.centroid = weighted / frame.weight;
  return frame;
}

[[nodiscard]] std::string hex(std::uint64_t key) {
  std::array<char, hex_digits> digits{};
  std::ranges::fill(digits, '0');
  std::array<char, hex_digits> raw{};
  const auto written = std::to_chars(raw.data(), raw.data() + raw.size(), key, hex_base);
  const auto length = static_cast<std::size_t>(written.ptr - raw.data());
  std::copy(raw.data(), written.ptr, digits.data() + (hex_digits - length));
  return {digits.data(), digits.size()};
}

[[nodiscard]] std::optional<std::uint64_t> parse_hex(std::string_view text) {
  std::uint64_t key = 0;
  if (text.size() != hex_digits)
    return std::nullopt;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), key, hex_base);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return std::nullopt;
  return key;
}

[[nodiscard]] double percentile(std::vector<double> values, double fraction) {
  std::ranges::sort(values);
  const double at = fraction * static_cast<double>(values.size() - 1);
  const auto below = static_cast<std::size_t>(std::floor(at));
  const auto above = std::min(below + 1, values.size() - 1);
  return values[below] + (values[above] - values[below]) * (at - static_cast<double>(below));
}

} // namespace

Register register_for(std::string_view category) noexcept {
  return category == bass_category ? Register::bass : Register::other;
}

Features measure(std::span<const engine::StereoFrame> frames, std::uint32_t sample_rate,
                 std::size_t held_frames) {
  Features features;
  if (frames.empty() || sample_rate == 0)
    return features;
  const double rate = sample_rate;
  const auto hop =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(envelope_hop_seconds * rate)));

  // The level envelope, one RMS per hop.
  std::vector<double> envelope;
  envelope.reserve(frames.size() / hop + 1);
  double mid_energy = 0.0;
  double side_energy = 0.0;
  for (std::size_t start = 0; start < frames.size(); start += hop) {
    const auto end = std::min(frames.size(), start + hop);
    double power = 0.0;
    for (std::size_t i = start; i < end; ++i) {
      const double left = frames[i].left;
      const double right = frames[i].right;
      power += 0.5 * (left * left + right * right);
      const double mid = 0.5 * (left + right);
      const double side = 0.5 * (left - right);
      mid_energy += mid * mid;
      side_energy += side * side;
    }
    envelope.push_back(std::sqrt(power / static_cast<double>(end - start)));
  }
  const double peak = *std::ranges::max_element(envelope);
  if (!(peak >= silence_rms))
    return features;
  features.audible = true;

  // Length: the attack's share of the held time and how long it stays
  // audible of the whole render.
  const auto attack_hop = static_cast<std::size_t>(
      std::distance(envelope.begin(), std::ranges::find_if(envelope, [&](double level) {
                      return level >= attack_fraction * peak;
                    })));
  std::size_t last_audible = 0;
  for (std::size_t i = 0; i < envelope.size(); ++i)
    if (envelope[i] >= audible_floor * peak)
      last_audible = i;
  const double attack_seconds = static_cast<double>(attack_hop * hop) / rate;
  const double audible_seconds =
      static_cast<double>(std::min(frames.size(), (last_audible + 1) * hop)) / rate;
  const double held = std::max(1.0, static_cast<double>(held_frames)) / rate;
  const double total = static_cast<double>(frames.size()) / rate;
  features.length = clamp01(attack_share * clamp01(attack_seconds / held) +
                            (1.0 - attack_share) * clamp01(audible_seconds / total));

  features.width =
      mid_energy + side_energy > 0.0 ? clamp01(side_energy / (mid_energy + side_energy)) : 0.0;

  // Spectral frames of the mid signal (left) and side (right) over what is
  // audible, zero-padded when the render is shorter than one window.
  const auto audible_end = std::max(window, std::min(frames.size(), (last_audible + 1) * hop));
  std::vector<engine::StereoFrame> mid_side(window);
  std::vector<Frame> spectra;
  for (std::size_t start = 0; start + window <= audible_end; start += spectrum_hop) {
    for (std::size_t i = 0; i < window; ++i) {
      const auto at = start + i;
      const auto frame = at < frames.size() ? frames[at] : engine::StereoFrame{0.0F, 0.0F};
      mid_side[i] = {0.5F * (frame.left + frame.right), 0.5F * (frame.left - frame.right)};
    }
    spectra.push_back(analyse_frame(mid_side, sample_rate, start));
  }

  // Brightness: centroids weighted by each frame's energy.
  double centroid_sum = 0.0;
  double energy_sum = 0.0;
  double loudest = 0.0;
  for (const auto& frame : spectra) {
    loudest = std::max(loudest, frame.energy);
    if (frame.weight > 0.0) {
      centroid_sum += frame.energy * frame.centroid;
      energy_sum += frame.energy;
    }
  }
  if (energy_sum > 0.0) {
    const double low = std::log2(centroid_low_hz);
    const double high = std::log2(centroid_high_hz);
    features.brightness = clamp01((centroid_sum / energy_sum - low) / (high - low));
  }

  // Movement: mean flux between successive band shapes while held, after
  // the attack, of frames loud enough to have a shape.
  const auto attack_frame = attack_hop * hop;
  std::optional<std::array<double, movement_bands>> previous;
  double flux = 0.0;
  std::size_t steps = 0;
  for (const auto& frame : spectra) {
    if (frame.start < attack_frame || frame.start + window > held_frames ||
        frame.energy < movement_frame_floor * loudest)
      continue;
    double sum = 0.0;
    for (const double band : frame.bands)
      sum += band;
    if (!(sum > 0.0))
      continue;
    std::array<double, movement_bands> shape{};
    for (std::size_t band = 0; band < movement_bands; ++band)
      shape[band] = frame.bands[band] / sum;
    if (previous) {
      double distance = 0.0;
      for (std::size_t band = 0; band < movement_bands; ++band)
        distance += std::abs(shape[band] - (*previous)[band]);
      flux += distance;
      ++steps;
    }
    previous = shape;
  }
  if (steps > 0)
    features.movement = clamp01(flux / static_cast<double>(steps) / max_flux);
  return features;
}

Result<Sound> factory_sound(const native_dsp::synth::SynthEngine& engine, std::size_t preset) {
  auto patch = native_dsp::synth::native_patch_for_preset(engine, preset);
  if (!patch)
    return Result<Sound>::failure(patch.error());
  return resolved(patch.value());
}

Result<Sound> sound_from_preset_text(const native_dsp::synth::SynthEngine& engine,
                                     std::string_view text) {
  auto preset = persistence::decode_native_effect_preset(text);
  if (!preset)
    return Result<Sound>::failure(preset.error());
  if (preset.value().engine != engine.descriptor->id)
    return Result<Sound>::failure(
        {ErrorCode::invalid_argument, "That preset is for another synth"});
  return resolved({preset.value().engine, preset.value().engine_version,
                   std::move(preset.value().parameters), std::move(preset.value().document)});
}

Result<std::vector<engine::StereoFrame>> render(const native_dsp::synth::SynthEngine& engine,
                                                const Sound& sound, Register phrase,
                                                const StopRequested& stop) {
  using Frames = Result<std::vector<engine::StereoFrame>>;
  if (engine.descriptor == nullptr || sound.values.size() != engine.descriptor->parameters.size())
    return Frames::failure({ErrorCode::invalid_argument, "The sound does not fit the synth"});
  if (has_imported_media(sound.document))
    return Frames::failure(
        {ErrorCode::unsupported, "Sounds with imported audio are not placed in the sky"});
  if (engine.validate_document != nullptr) {
    if (auto valid = engine.validate_document(sound.document); !valid)
      return Frames::failure(valid.error());
  }
  auto created = native_dsp::synth::NativeSynthRuntime::create(engine, sound.values, sound.document,
                                                               {}, render_rate);
  if (!created)
    return Frames::failure(created.error());
  auto& runtime = *created.value();
  const engine::DenormalsFlushed flushed;

  const auto total = static_cast<std::size_t>(std::lround(render_seconds * render_rate));
  const auto held = static_cast<std::size_t>(std::lround(held_seconds * render_rate));
  std::vector<engine::StereoFrame> output(total, engine::StereoFrame{0.0F, 0.0F});
  std::vector<std::uint8_t> keys;
  if (phrase == Register::bass)
    keys.push_back(bass_key);
  else
    keys.assign(dyad_keys.begin(), dyad_keys.end());
  const double beats_per_frame = phrase_bpm / seconds_per_minute / render_rate;
  std::vector<engine::BlockMidiEvent> events;
  for (std::size_t start = 0; start < total; start += render_block_frames) {
    if (stop && stop())
      return Frames::failure({ErrorCode::unavailable, "Analysis stopped"});
    const auto frames = std::min<std::size_t>(render_block_frames, total - start);
    // Notes start on the first frame and release on the held frame, each
    // at its own offset in whichever block holds it.
    events.clear();
    const auto add = [&](std::size_t frame, engine::MidiEventKind kind) {
      if (frame < start || frame >= start + frames)
        return;
      for (std::size_t note = 0; note < keys.size(); ++note) {
        engine::BlockMidiEvent event;
        event.offset = static_cast<std::uint32_t>(frame - start);
        event.clip_id_bits = phrase_clip;
        event.note_id_bits = {0, first_note_id + note};
        event.kind = kind;
        event.key = keys[note];
        event.velocity = kind == engine::MidiEventKind::note_on ? phrase_velocity : 0;
        events.push_back(event);
      }
    };
    add(0, engine::MidiEventKind::note_on);
    add(held, engine::MidiEventKind::note_off);
    const double beat = static_cast<double>(start) * beats_per_frame;
    const native_dsp::synth::SynthTransport transport{
        beat, beats_per_frame, beat / phrase_beats_per_bar, phrase_beats_per_bar};
    const auto report = runtime.process(std::span(output).subspan(start, frames), events, start,
                                        phrase_generation, transport, {});
    if (report.status != native_dsp::synth::SynthProcessStatus::rendered)
      return Frames::failure({ErrorCode::internal, "The synth refused the analysis phrase"});
  }
  return Frames::success(std::move(output));
}

Result<Features> analyse(const native_dsp::synth::SynthEngine& engine, const Sound& sound,
                         Register phrase, const StopRequested& stop) {
  auto frames = render(engine, sound, phrase, stop);
  if (!frames)
    return Result<Features>::failure(frames.error());
  const auto held = static_cast<std::size_t>(std::lround(held_seconds * render_rate));
  return Result<Features>::success(measure(frames.value(), render_rate, held));
}

std::uint64_t fnv1a(std::string_view bytes, std::uint64_t basis) noexcept {
  constexpr std::uint64_t prime = 0x0000'0100'0000'01b3ULL;
  std::uint64_t hash = basis;
  for (const char byte : bytes) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= prime;
  }
  return hash;
}

Result<std::uint64_t> sound_key(const native_dsp::synth::SynthEngine& engine, const Sound& sound,
                                Register phrase) {
  auto patch = native_dsp::synth::make_native_patch(engine, sound.values, sound.document);
  if (!patch)
    return Result<std::uint64_t>::failure(patch.error());
  project::NativeEffectPatch saved;
  saved.engine = patch.value().engine;
  saved.engine_version = patch.value().engine_version;
  saved.parameters = std::move(patch.value().parameters);
  saved.document = std::move(patch.value().document);
  std::string text(cache_key_tag);
  text += '\0';
  text += std::to_string(analysis_version);
  text += '\0';
  text += phrase == Register::bass ? 'b' : 'o';
  text += '\0';
  text += persistence::encode_native_effect_preset(saved);
  return Result<std::uint64_t>::success(fnv1a(text));
}

Sky::Sky(std::span<const Features> library) {
  std::vector<double> brightness;
  std::vector<double> length;
  std::vector<double> movement;
  std::vector<double> width;
  for (const auto& features : library) {
    if (!features.audible)
      continue;
    brightness.push_back(features.brightness);
    length.push_back(features.length);
    movement.push_back(features.movement);
    width.push_back(features.width);
  }
  if (brightness.empty())
    return;
  const auto range = [](const std::vector<double>& values) {
    return Range{percentile(values, low_percentile), percentile(values, high_percentile)};
  };
  brightness_ = range(brightness);
  length_ = range(length);
  movement_ = range(movement);
  width_ = range(width);
}

Placement Sky::place(const Features& features) const noexcept {
  // An axis with less spread than this places everything at its middle.
  constexpr double min_spread = 1e-9;
  constexpr double middle = 0.5;
  const auto along = [](const Range& range, double value) {
    const double spread = range.high - range.low;
    return spread > min_spread ? clamp01((value - range.low) / spread) : middle;
  };
  return {along(brightness_, features.brightness), along(length_, features.length),
          along(movement_, features.movement), along(width_, features.width)};
}

FeatureCache FeatureCache::load(const std::filesystem::path& file) {
  FeatureCache cache;
  cache.file_ = file;
  std::error_code error;
  const auto bytes = std::filesystem::file_size(file, error);
  if (error || bytes > max_cache_bytes)
    return cache;
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return cache;
  std::ostringstream text;
  text << in.rdbuf();
  const auto document = Json::parse(text.str(), nullptr, false);
  // The version must be an unsigned number; anything else is a stale cache.
  if (document.is_discarded() || !document.is_object() || !document.contains("version") ||
      !document.at("version").is_number_unsigned() ||
      document.at("version").get<unsigned>() != analysis_version || !document.contains("sounds") ||
      !document.at("sounds").is_object())
    return cache;
  try {
    for (const auto& [name, entry] : document.at("sounds").items()) {
      const auto key = parse_hex(name);
      if (!key || !entry.is_array() || entry.size() != feature_fields)
        continue;
      Features features{entry.at(0).get<double>(), entry.at(1).get<double>(),
                        entry.at(2).get<double>(), entry.at(3).get<double>(),
                        entry.at(4).get<int>() != 0};
      if (!std::isfinite(features.brightness) || !std::isfinite(features.length) ||
          !std::isfinite(features.movement) || !std::isfinite(features.width))
        continue;
      cache.entries_[*key] = features;
    }
  } catch (const Json::exception&) {
    cache.entries_.clear();
  }
  return cache;
}

std::optional<Features> FeatureCache::find(std::uint64_t key) const {
  const auto found = entries_.find(key);
  return found == entries_.end() ? std::nullopt : std::optional<Features>(found->second);
}

void FeatureCache::insert(std::uint64_t key, const Features& features) { entries_[key] = features; }

void FeatureCache::retain(std::span<const std::uint64_t> keys) {
  std::erase_if(entries_, [&](const auto& entry) {
    return std::ranges::find(keys, entry.first) == keys.end();
  });
}

Result<void> FeatureCache::save() const {
  if (file_.empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "The cache has no file"});
  Json sounds = Json::object();
  for (const auto& [key, features] : entries_)
    sounds[hex(key)] = Json::array({features.brightness, features.length, features.movement,
                                    features.width, features.audible ? 1 : 0});
  const Json document{{"version", analysis_version}, {"sounds", std::move(sounds)}};
  const auto text = document.dump();
  std::error_code error;
  std::filesystem::create_directories(file_.parent_path(), error);
  if (error)
    return Result<void>::failure({ErrorCode::io_failure, "Cannot create the sound map cache"});
  const auto parent = file_.has_parent_path() ? file_.parent_path() : std::filesystem::path(".");
  const foundation::UniqueFd directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory)
    return Result<void>::failure({ErrorCode::io_failure, "Cannot create the sound map cache"});
  const auto name = file_.filename().string();
  auto replaced = foundation::replace_file(directory.get(), name + ".tmp", name,
                                           std::as_bytes(std::span(text.data(), text.size())),
                                           "the sound map cache");
  if (!replaced)
    return Result<void>::failure(std::move(replaced.error()));
  return Result<void>::success();
}

std::size_t analysis_threads() noexcept {
  const std::size_t cores = std::thread::hardware_concurrency();
  return std::clamp<std::size_t>(cores / 2, 1, max_analysis_threads);
}

void analyse_all(const native_dsp::synth::SynthEngine& engine, std::span<const Job> jobs,
                 std::size_t threads, const JobDone& done, const StopRequested& stop) {
  if (jobs.empty())
    return;
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (;;) {
      if (stop && stop())
        return;
      const auto index = next.fetch_add(1);
      if (index >= jobs.size())
        return;
      const auto result = [&]() -> Result<Features> {
        try {
          return analyse(engine, jobs[index].sound, jobs[index].phrase, stop);
        } catch (const std::exception& error) {
          return Result<Features>::failure({ErrorCode::internal, error.what()});
        }
      }();
      if (done)
        done(index, result);
    }
  };
  const auto count = std::clamp<std::size_t>(threads, 1, jobs.size());
  std::vector<std::jthread> pool;
  pool.reserve(count - 1);
  for (std::size_t i = 1; i < count; ++i)
    pool.emplace_back(work);
  work();
}

} // namespace oscillate::application::sound_map
