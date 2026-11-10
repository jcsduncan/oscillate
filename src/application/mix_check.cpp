// Mix Check (ADR-0074): one offline render of the song, measured track by
// track as it renders, the findings it shows and the one-step fixes for them.
#include "oscillate/application/mix_check.hpp"
#include "oscillate/project/lookup.hpp"

#include "oscillate/engine/midi_events.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include <type_traits>

namespace oscillate::application::mix_check {
namespace {
using foundation::ErrorCode;
using project::find_track;
template <typename T> using Result = foundation::Result<T>;
namespace mix = analysis::mix;
namespace fs = std::filesystem;

// A cancelled check fails with this message.
constexpr std::string_view cancelled_message = "Mix Check cancelled";
// A reference file is read in blocks of this many frames.
constexpr std::size_t read_block_frames = 65'536;
// A track carrying this share of the tracks' summed sub energy is blamed.
constexpr double sub_blame_share = 0.25;
// Evidence plots' resolution.
constexpr std::size_t evidence_points = 96;
// The rumble plot runs from the sub floor's display edge to this many times
// the fundamental (at least sub_plot_top_hz).
constexpr double sub_plot_span = 2.5;
constexpr double sub_plot_top_hz = 80.0;
// A ringing plot spans the peaks and this ratio each side.
constexpr double ringing_plot_margin = 1.25;
// Bands shown each side of a mono or masking span.
constexpr std::size_t evidence_band_margin = 3;
// The click plot's half width (seconds) around the edge.
constexpr double click_plot_seconds = 0.003;
// Display and audio constants.
constexpr double a4_hz = 440.0;
constexpr int a4_key = 69;
constexpr double semitones_per_octave = 12.0;
constexpr double kilo = 1'000.0;
// Taps are post-fader: each track's profile already carries its level, pan
// and automation, so measurements take it at unity.
constexpr double post_fader = 1.0;

// ---- Formatting ----------------------------------------------------------------------

[[nodiscard]] std::string fixed(double value, int decimals) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(decimals) << std::fabs(value);
  // The typographic minus, as the panels show it.
  return (value < 0.0 && out.str().find_first_not_of("0.") != std::string::npos ? "−" : "") +
         out.str();
}
[[nodiscard]] std::string signed_text(double value, int decimals) {
  return (value > 0.0 ? "+" : "") + fixed(value, decimals);
}
[[nodiscard]] std::string hz_text(double hz) {
  if (hz >= kilo)
    return fixed(hz / kilo, hz >= 10 * kilo ? 1 : 2) + " kHz";
  return fixed(hz, 0) + " Hz";
}
[[nodiscard]] std::string band_text(double low, double high) {
  if (low >= kilo && high >= kilo)
    return fixed(low / kilo, low >= 10 * kilo ? 1 : 2) + "–" + hz_text(high);
  if (high < kilo)
    return fixed(low, 0) + "–" + hz_text(high);
  return hz_text(low) + "–" + hz_text(high);
}

// A 24 dB/oct (fourth-order Butterworth) high-pass's cut at `hz` (dB):
// its power response is 1 / (1 + (cutoff / hz)^high_pass_24_power).
constexpr double high_pass_24_power = 8.0;
[[nodiscard]] double high_pass_cut_db(double cutoff, double hz) noexcept {
  return 10.0 * std::log10(1.0 + std::pow(cutoff / hz, high_pass_24_power));
}

[[nodiscard]] double key_hz(int key) noexcept {
  return a4_hz * std::pow(2.0, static_cast<double>(key - a4_key) / semitones_per_octave);
}

// ---- Project lookups -----------------------------------------------------------------

[[nodiscard]] std::string_view engine_of(const project::Track& track) {
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  return patch == nullptr ? std::string_view{} : std::string_view(patch->engine);
}

[[nodiscard]] bool drum_engine(std::string_view engine) {
  return std::ranges::find(drum_engines, engine) != drum_engines.end();
}

// The track's name, or its row as the arrangement numbers it.
[[nodiscard]] std::string plain_name(const project::Snapshot& snapshot,
                                     const project::Track& track) {
  if (!track.name.empty())
    return track.name;
  const auto row = std::ranges::find(snapshot.tracks, track.id, &project::Track::id);
  return "Track " + std::to_string(row - snapshot.tracks.begin() + 1);
}

// The name with what plays it, as the findings' chips show it.
[[nodiscard]] std::string track_name(const project::Snapshot& snapshot,
                                     const project::Track& track) {
  std::string name = plain_name(snapshot, track);
  const auto engine = engine_of(track);
  if (const auto* synth = native_dsp::synth::find_engine(engine))
    name += " (" + std::string(synth->descriptor->name) + ")";
  else if (track.kind == project::TrackKind::audio)
    name += " (audio)";
  return name;
}

[[nodiscard]] TrackRef ref(const project::Snapshot& snapshot, const project::Track& track) {
  return {track.id, track_name(snapshot, track), track.color};
}

[[nodiscard]] bool has_content(const project::Track& track) {
  return !track.clips.empty() || !track.midi_clips.empty() || track.frozen.has_value();
}

// The tracks a listener hears: audio and instrument tracks with something
// on them, unmuted, and soloed when anything is.
[[nodiscard]] std::vector<project::TrackId> audible_tracks(const project::Snapshot& snapshot) {
  const bool any_solo = std::ranges::any_of(snapshot.tracks, &project::Track::soloed);
  std::vector<project::TrackId> tracks;
  for (const auto& track : snapshot.tracks) {
    if (track.kind != project::TrackKind::audio && track.kind != project::TrackKind::instrument)
      continue;
    if (track.muted || (any_solo && !track.soloed) || !has_content(track))
      continue;
    tracks.push_back(track.id);
  }
  return tracks;
}

[[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>>
clip_frames(const project::Snapshot& snapshot, const project::Placement& placement) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    const auto first = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    const auto last = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!first || !last)
      return std::nullopt;
    return std::pair{first.value().value, last.value().value};
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  return std::pair{elapsed.start.value, elapsed.end.value};
}

[[nodiscard]] bool same_timing(const project::Snapshot& a, const project::Snapshot& b) {
  const auto& ta = a.tempo.events();
  const auto& tb = b.tempo.events();
  const auto& ma = a.meter.events();
  const auto& mb = b.meter.events();
  if (a.sample_rate != b.sample_rate || ta.size() != tb.size() || ma.size() != mb.size() ||
      !(a.sources == b.sources))
    return false;
  for (std::size_t i = 0; i < ta.size(); ++i)
    if (ta[i].tick.value != tb[i].tick.value || ta[i].bpm != tb[i].bpm ||
        ta[i].shape_to_next != tb[i].shape_to_next)
      return false;
  for (std::size_t i = 0; i < ma.size(); ++i)
    if (ma[i].tick.value != mb[i].tick.value || ma[i].numerator != mb[i].numerator ||
        ma[i].denominator != mb[i].denominator)
      return false;
  return true;
}

// ---- What a track plays --------------------------------------------------------------

// One sounding source of a built-in instrument: the parameters that switch
// it on, set its level and move it from the played key (empty: none). Its
// octave reads octave_scale * value + octave_offset, so a choice of octaves
// maps to them.
struct PitchSource {
  std::string_view on;
  std::string_view level;
  std::string_view octave;
  double octave_scale;
  double octave_offset;
  std::string_view semitone;
  std::string_view pitch;
};
constexpr std::array<PitchSource, 4> nova_sources{{
    {"nova.osc.a.on", "nova.osc.a.level", "nova.osc.a.octave", 1.0, 0.0, "nova.osc.a.semitone",
     "nova.osc.a.pitch"},
    {"nova.osc.b.on", "nova.osc.b.level", "nova.osc.b.octave", 1.0, 0.0, "nova.osc.b.semitone",
     "nova.osc.b.pitch"},
    {"nova.osc.c.on", "nova.osc.c.level", "nova.osc.c.octave", 1.0, 0.0, "nova.osc.c.semitone",
     "nova.osc.c.pitch"},
    // Its choices are -2, -1 and 0 octaves.
    {"nova.sub.on", "nova.sub.level", "nova.sub.octave", 1.0, -2.0, {}, {}},
}};
constexpr std::array<PitchSource, 3> prism_sources{{
    {{}, "prism.osc1.level", "prism.osc1.octave", 1.0, 0.0, "prism.osc1.semi", {}},
    {{}, "prism.osc2.level", "prism.osc2.octave", 1.0, 0.0, "prism.osc2.semi", {}},
    // Its choices are -1 and -2 octaves.
    {{}, "prism.sub.level", "prism.sub.octave", -1.0, -1.0, {}, {}},
}};
constexpr std::array<PitchSource, 1> drift_sources{{
    {{}, {}, "drift.source.octave", 1.0, 0.0, {}, {}},
}};
// A switch reads on from this.
constexpr double switch_on = 0.5;
// Note effects: a scale step moves a note at most this far (a pentatonic
// minor third), and a chord has this many shifts.
constexpr double scale_step_semitones = 3.0;
constexpr std::size_t chord_shifts = 6;
// A bass's spectrum may confirm a sub this many octaves under a source.
constexpr int confirmed_sub_octaves = 2;

// The semitones each sounding source of the track's built-in instrument
// moves the played key by ({0} for an engine that sounds at the key).
[[nodiscard]] std::vector<double> source_offsets(const project::Track& track) {
  std::vector<double> at_key{0.0};
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  if (patch == nullptr)
    return at_key;
  std::span<const PitchSource> sources;
  if (patch->engine == "nova")
    sources = nova_sources;
  else if (patch->engine == "prism")
    sources = prism_sources;
  else if (patch->engine == "drift")
    sources = drift_sources;
  if (sources.empty())
    return at_key;
  auto resolved = native_inserts::resolve_native_instrument(*patch);
  if (!resolved)
    return at_key;
  const auto& descriptor = *resolved.value().engine->descriptor;
  const auto& values = resolved.value().values;
  const auto value = [&](std::string_view id) -> std::optional<double> {
    if (id.empty())
      return std::nullopt;
    const auto index = native_dsp::find_parameter(descriptor, id);
    return index && *index < values.size() ? std::optional<double>(values[*index]) : std::nullopt;
  };
  std::vector<double> offsets;
  for (const auto& source : sources) {
    if (value(source.on).value_or(1.0) < switch_on || !(value(source.level).value_or(1.0) > 0.0))
      continue;
    const double octave = source.octave_scale * value(source.octave).value_or(0.0) +
                          (value(source.octave) ? source.octave_offset : 0.0);
    offsets.push_back(semitones_per_octave * octave + value(source.semitone).value_or(0.0) +
                      value(source.pitch).value_or(0.0));
  }
  return offsets.empty() ? at_key : offsets;
}

// How far the track's enabled note effects can move a note down
// (semitones, at most 0), added along the chain: Scale's octave and
// transpose, Chord's lowest shift, Echo's falling repeats, Random's range.
[[nodiscard]] double note_effect_reach(const project::Track& track) {
  double reach = 0.0;
  for (const auto& insert : track.note_effects) {
    if (!insert.enabled)
      continue;
    auto resolved = native_dsp::note_effects::resolve_note_effect(insert);
    if (!resolved)
      continue;
    const auto& effect = resolved.value();
    const auto value = [&](const std::string& id) {
      const auto index = native_dsp::note_effects::note_effect_parameter_index(*effect.engine, id);
      return index && index.value() < effect.values.size() ? effect.values[index.value()] : 0.0;
    };
    if (insert.engine == "scale") {
      reach += semitones_per_octave * std::min(0.0, value("scale.octave")) +
               scale_step_semitones * std::min(0.0, value("scale.transpose"));
    } else if (insert.engine == "chord") {
      double lowest = 0.0;
      for (std::size_t shift = 1; shift <= chord_shifts; ++shift)
        lowest = std::min(lowest, value("chord.shift" + std::to_string(shift)));
      reach += lowest;
    } else if (insert.engine == "echo") {
      reach += std::min(0.0, value("echo.pitch")) * value("echo.repeats");
    } else if (insert.engine == "random") {
      if (value("random.chance") > 0.0)
        reach -= value("random.range");
    }
  }
  return reach;
}

// The track's notes from the compiled plan: its keys and note-on frames.
struct Notes {
  std::set<int> keys;
  std::vector<std::uint64_t> onsets;
};
[[nodiscard]] Notes notes_of(const engine::CompiledMidiPlan* plan, project::TrackId track) {
  Notes notes;
  if (plan == nullptr)
    return notes;
  const auto events =
      std::ranges::find(plan->instruments(), track, &engine::InstrumentEvents::track);
  if (events == plan->instruments().end())
    return notes;
  for (const auto& event : events->events()) {
    if (event.kind != engine::MidiEventKind::note_on || event.sample.value < 0)
      continue;
    notes.keys.insert(event.key);
    notes.onsets.push_back(static_cast<std::uint64_t>(event.sample.value));
  }
  return notes;
}

// What the track sounds and its lowest fundamental: the played keys moved
// by its sources and note effects, lowered further when its own spectrum
// confirms a peak on one of those notes or an octave or two under one (a
// sub the model does not know of). A bass without notes (audio) takes its
// strongest low peak, or a confirmed peak an octave or two under it.
void fundamentals(TrackMeasure& measured, const Notes& notes) {
  measured.played_hz.clear();
  measured.lowest_fundamental_hz = 0.0;
  if (measured.drum)
    return;
  std::vector<double> candidates;
  const auto with_subs = [&](double hz) {
    for (int octave = 0; octave <= confirmed_sub_octaves; ++octave)
      candidates.push_back(hz / std::exp2(octave));
  };
  if (!notes.keys.empty()) {
    const auto offsets = source_offsets(measured.track);
    const double reach = note_effect_reach(measured.track);
    double lowest = 0.0;
    for (const int key : notes.keys)
      for (const double offset : offsets) {
        const double hz = key_hz(key) * std::exp2(offset / semitones_per_octave);
        const double reached = hz * std::exp2(reach / semitones_per_octave);
        measured.played_hz.push_back(hz);
        lowest = lowest == 0.0 ? reached : std::min(lowest, reached);
        with_subs(hz);
        if (reached != hz)
          with_subs(reached);
      }
    const auto confirmed = mix::lowest_matching_peak_hz(measured.profile, candidates);
    measured.lowest_fundamental_hz = confirmed ? std::min(lowest, *confirmed) : lowest;
    return;
  }
  if (!measured.bass)
    return;
  const auto peak = mix::strongest_low_peak_hz(measured.profile);
  if (!peak)
    return;
  with_subs(*peak);
  measured.lowest_fundamental_hz =
      std::min(*peak, mix::lowest_matching_peak_hz(measured.profile, candidates).value_or(*peak));
}

// ---- Rendering -----------------------------------------------------------------------

// Mix Check renders a pinned snapshot offline, as Freeze's track renders do;
// it never touches the live graph, so its render takes its lease from a gate
// of its own and playback may start while it runs.
class DetachedGate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

// The render's blocks are gathered into chunks of this many frames; each
// chunk's tracks are measured in parallel, then the measurements that need
// several tracks at once (the drum sum, kick against bass).
constexpr std::size_t analysis_chunk_frames = 65'536;
// The render's share of the progress bar; measuring what is left the rest.
constexpr double render_share = 0.95;
// A clip edge's window: the click's context and plot each side, and a few
// samples more for the slope into the window's first sample.
constexpr std::size_t edge_guard_frames = 2;

// One chunk of a track's output, and what is derived from it.
struct ChunkSamples {
  std::vector<float> left;
  std::vector<float> right;
  std::vector<float> mono; // (L+R)/2, for the drum sum.
  std::vector<float> low;  // Under the lock crossover, for kick and bass.
  void append(std::span<const float> l, std::span<const float> r) {
    left.insert(left.end(), l.begin(), l.end());
    right.insert(right.end(), r.begin(), r.end());
  }
  void clear() {
    left.clear();
    right.clear();
    mono.clear();
    low.clear();
  }
};

// An unfaded clip edge of an audio track and the samples around it.
struct EdgeWindow {
  project::ClipId clip;
  bool start = true;
  std::int64_t edge = 0;  // The project frame of the edge.
  std::int64_t first = 0; // The project frame of the window's first sample.
  std::vector<float> left;
  std::vector<float> right;
};

[[nodiscard]] std::vector<EdgeWindow> clip_edges(const project::Snapshot& snapshot,
                                                 const project::Track& track) {
  const auto context =
      static_cast<std::int64_t>(
          std::lround((mix::click_context_seconds + click_plot_seconds) * snapshot.sample_rate)) +
      static_cast<std::int64_t>(edge_guard_frames);
  std::vector<EdgeWindow> edges;
  for (const auto& clip : track.clips) {
    const auto frames = clip_frames(snapshot, clip.placement);
    if (!frames)
      continue;
    for (const bool start : {true, false}) {
      if ((start ? clip.fades.in_fraction : clip.fades.out_fraction) > 0.0)
        continue;
      const auto edge = start ? frames->first : frames->second;
      edges.push_back({clip.id, start, edge, edge - context,
                       std::vector<float>(static_cast<std::size_t>(2 * context), 0.0F),
                       std::vector<float>(static_cast<std::size_t>(2 * context), 0.0F)});
    }
  }
  return edges;
}

// One audible track as the song renders: its profile, its share of the drum
// sum and kick meters, and the samples around its clip edges.
struct Lane {
  Lane(const project::Track& track, std::uint32_t rate, Notes track_notes)
      : measure{track}, notes(std::move(track_notes)),
        builder(rate, {.detect_pitch = track.kind == project::TrackKind::audio}) {}

  TrackMeasure measure;
  Notes notes;
  mix::ProfileBuilder builder;
  bool drum_engine = false;
  // An audio track's place among the drum sum's candidates.
  std::optional<std::size_t> candidate;
  std::optional<mix::LowBand> low;
  // A kick's meters against every track that is not a drum.
  std::vector<std::pair<Lane*, mix::CancellationMeter>> meters;
  std::vector<EdgeWindow> edges;
  ChunkSamples chunk;

  // The chunk starting at project frame `first`. Touches only this lane.
  void measure_chunk(std::uint64_t first) {
    const auto count = chunk.left.size();
    if (count == 0)
      return;
    builder.push(chunk.left, chunk.right);
    if (drum_engine || candidate) {
      chunk.mono.resize(count);
      for (std::size_t i = 0; i < count; ++i)
        chunk.mono[i] = 0.5F * (chunk.left[i] + chunk.right[i]);
    }
    if (low) {
      chunk.low.resize(count);
      low->process(chunk.left, chunk.right, chunk.low);
    }
    const auto chunk_first = static_cast<std::int64_t>(first);
    const auto chunk_end = chunk_first + static_cast<std::int64_t>(count);
    for (auto& edge : edges) {
      const auto window_end = edge.first + static_cast<std::int64_t>(edge.left.size());
      for (auto frame = std::max(edge.first, chunk_first); frame < std::min(window_end, chunk_end);
           ++frame) {
        edge.left[static_cast<std::size_t>(frame - edge.first)] =
            chunk.left[static_cast<std::size_t>(frame - chunk_first)];
        edge.right[static_cast<std::size_t>(frame - edge.first)] =
            chunk.right[static_cast<std::size_t>(frame - chunk_first)];
      }
    }
  }

  // Each edge that clicks, with its samples for the evidence.
  [[nodiscard]] std::vector<TrackMeasure::Click> clicks(std::uint32_t rate) const {
    std::vector<TrackMeasure::Click> found;
    const auto half = static_cast<std::size_t>(std::lround(click_plot_seconds * rate));
    for (const auto& edge : edges) {
      const auto context = edge.left.size() / 2;
      const auto ratio = mix::edge_click(edge.left, edge.right, context, rate);
      if (!ratio)
        continue;
      TrackMeasure::Click click{edge.clip,
                                edge.start,
                                static_cast<std::uint64_t>(std::max<std::int64_t>(0, edge.edge)),
                                *ratio,
                                {}};
      for (auto i = context - std::min(context, half);
           i < std::min(edge.left.size(), context + half); ++i)
        click.samples.push_back(edge.left[i]);
      found.push_back(std::move(click));
    }
    return found;
  }
};

// One-based bars' loudness through the song, from the master's steps.
std::vector<BarEnergy> bar_energy(const project::Snapshot& snapshot, const mix::Profile& master) {
  std::vector<BarEnergy> bars;
  if (master.step_frames == 0)
    return bars;
  for (std::int64_t bar = 1;; ++bar) {
    const auto first_tick = snapshot.meter.tick_at({bar, 1, 0});
    const auto last_tick = snapshot.meter.tick_at({bar + 1, 1, 0});
    if (!first_tick || !last_tick)
      break;
    const auto first = snapshot.tempo.tick_to_sample(first_tick.value(), snapshot.sample_rate);
    const auto last = snapshot.tempo.tick_to_sample(last_tick.value(), snapshot.sample_rate);
    if (!first || !last || first.value().value < 0 ||
        static_cast<std::uint64_t>(first.value().value) >= master.frames)
      break;
    const auto from = static_cast<std::size_t>(first.value().value) / master.step_frames;
    const auto to = static_cast<std::size_t>(last.value().value) / master.step_frames;
    const auto lufs = mix::steps_loudness(master, from, std::max(from + 1, to));
    bars.push_back({static_cast<std::size_t>(bar), lufs.value_or(mix::silence_db)});
  }
  return bars;
}

} // namespace

double lowered_ceiling_db(const CeilingReading& reading) noexcept {
  const auto& current = reading.current;
  const double target = reading.target;
  const double true_peak = reading.true_peak;
  if (!current)
    return target - ceiling_true_peak_margin_db;
  // What the master measured over the limiter's ceiling (peaks between
  // samples, or gain after it) comes on top of any new ceiling too.
  const double over_ceiling = std::max(0.0, true_peak - *current);
  return std::min(*current, target - over_ceiling) - ceiling_true_peak_margin_db;
}

const Target& target(std::string_view id) noexcept {
  for (const auto& candidate : targets)
    if (candidate.id == id)
      return candidate;
  for (const auto& candidate : targets)
    if (candidate.id == default_target)
      return candidate;
  return targets.front();
}

std::string_view kind_name(Kind kind) noexcept {
  switch (kind) {
  case Kind::sub:
    return "sub";
  case Kind::mono:
    return "mono";
  case Kind::mono_lows:
    return "mono_lows";
  case Kind::ringing:
    return "ringing";
  case Kind::kick_bass:
    return "kick_bass";
  case Kind::loudness:
    return "loudness";
  case Kind::click:
    return "click";
  case Kind::energy:
    return "energy";
  case Kind::masking:
    return "masking";
  case Kind::clipping:
    return "clipping";
  case Kind::balance:
    return "balance";
  case Kind::dc:
    return "dc";
  case Kind::silent:
    return "silent";
  }
  return "unknown";
}

// ---- Measure -------------------------------------------------------------------------

Result<Measurements> measure(const PinnedSnapshot& pinned, const Progress& progress) {
  using R = Result<Measurements>;
  const auto& snapshot = *pinned.snapshot;
  const auto rate = snapshot.sample_rate;
  bool stopped = false;
  const auto report_progress = [&](double fraction, const std::string& stage) {
    if (progress && !progress(std::clamp(fraction, 0.0, 1.0), stage))
      stopped = true;
    return !stopped;
  };
  const std::string rendering = "Rendering and measuring the song";
  if (!report_progress(0.0, rendering))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});

  Measurements result;
  result.snapshot = pinned.snapshot;
  result.project_name = pinned.name;
  auto plan = engine::compile_midi_events(snapshot);
  const auto* compiled = plan ? &plan.value() : nullptr;

  // Each audible track's lane, set up from what it is before it sounds.
  std::vector<std::unique_ptr<Lane>> lanes;
  std::map<project::TrackId, std::size_t> lane_of;
  std::size_t candidates = 0;
  for (const auto id : audible_tracks(snapshot)) {
    const auto& track = *find_track(snapshot, id);
    auto lane = std::make_unique<Lane>(track, rate, notes_of(compiled, id));
    const auto engine = engine_of(track);
    lane->measure.kick = engine == native_dsp::synth::lock_kick_engine;
    lane->drum_engine = drum_engine(engine);
    if (track.kind == project::TrackKind::audio) {
      lane->candidate = candidates++;
      if (!track.frozen)
        lane->edges = clip_edges(snapshot, track);
    }
    lane_of.emplace(id, lanes.size());
    lanes.push_back(std::move(lane));
  }
  // Every kick against every track that is not a drum: which of them is a
  // bass is known only once they are measured.
  for (const auto& kick : lanes) {
    if (!kick->measure.kick || kick->notes.onsets.empty())
      continue;
    kick->low.emplace(rate);
    for (auto& other : lanes) {
      if (other->drum_engine)
        continue;
      if (!other->low)
        other->low.emplace(rate);
      kick->meters.push_back({&*other, mix::CancellationMeter(rate, kick->notes.onsets)});
    }
  }
  mix::ProfileBuilder master(rate);
  ChunkSamples master_chunk;
  mix::DrumCrest crest(candidates);
  std::vector<float> drums;
  std::vector<std::span<const float>> candidate_blocks(candidates);
  std::uint64_t chunk_first = 0;
  std::size_t chunk_fill = 0;

  // A chunk's tracks (and the master's waiting samples) are measured in
  // parallel; then the drum sum and the kicks' meters, which need them all.
  const auto flush = [&] {
    std::vector<std::future<void>> jobs;
    jobs.reserve(lanes.size() + 1);
    for (auto& lane : lanes)
      jobs.push_back(std::async(std::launch::async | std::launch::deferred,
                                [&own = *lane, chunk_first] { own.measure_chunk(chunk_first); }));
    jobs.push_back(std::async(std::launch::async | std::launch::deferred, [&] {
      master.push(master_chunk.left, master_chunk.right);
      master_chunk.clear();
    }));
    for (auto& job : jobs)
      job.get();
    if (chunk_fill == 0)
      return;
    drums.assign(chunk_fill, 0.0F);
    bool any_drum = false;
    for (const auto& lane : lanes) {
      if (lane->drum_engine) {
        any_drum = true;
        for (std::size_t i = 0; i < chunk_fill; ++i)
          drums[i] += lane->chunk.mono[i];
      }
      if (lane->candidate)
        candidate_blocks[*lane->candidate] = lane->chunk.mono;
    }
    crest.push(any_drum ? std::span<const float>(drums) : std::span<const float>{},
               candidate_blocks);
    for (auto& lane : lanes)
      for (auto& [other, meter] : lane->meters)
        meter.push(lane->chunk.low, other->chunk.low, chunk_first);
    for (auto& lane : lanes)
      lane->chunk.clear();
    chunk_first += chunk_fill;
    chunk_fill = 0;
  };

  exporting::RenderTaps taps;
  taps.trim_silent_tail = true;
  taps.tracks = [&](std::span<const exporting::TrackTap> blocks, std::uint64_t) {
    std::size_t count = 0;
    for (const auto& block : blocks) {
      const auto found = lane_of.find(block.track);
      if (found == lane_of.end())
        continue;
      lanes[found->second]->chunk.append(block.left, block.right);
      count = block.left.size();
    }
    chunk_fill += count;
    if (chunk_fill >= analysis_chunk_frames)
      flush();
  };
  taps.master = [&](std::span<const float> left, std::span<const float> right, std::uint64_t) {
    master_chunk.append(left, right);
    // With no track to flush it, the master measures its own chunks.
    if (lanes.empty() && master_chunk.left.size() >= analysis_chunk_frames) {
      master.push(master_chunk.left, master_chunk.right);
      master_chunk.clear();
    }
  };
  taps.progress = [&](std::uint64_t made, std::uint64_t total) {
    return report_progress(
        total == 0 ? 0.0 : render_share * static_cast<double>(made) / static_cast<double>(total),
        rendering);
  };
  {
    DetachedGate gate;
    auto authority = exporting::StoppedExportAuthority::acquire(snapshot, gate);
    if (!authority)
      return R::failure(authority.error());
    auto rendered =
        exporting::render_master_taps(snapshot, pinned.reader, std::move(authority).value(), taps);
    if (!rendered) {
      if (stopped)
        return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
      return R::failure(rendered.error());
    }
  }
  flush();
  if (!report_progress(render_share, "Measuring"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});

  // The profiles' last frames and decays, each on its own thread.
  {
    std::vector<std::future<void>> jobs;
    jobs.reserve(lanes.size());
    for (auto& lane : lanes)
      jobs.push_back(std::async(std::launch::async | std::launch::deferred, [&own = *lane] {
        own.measure.profile = std::move(own.builder).finish();
      }));
    result.master = std::move(master).finish();
    for (auto& job : jobs)
      job.get();
  }
  result.loudness = mix::loudness(result.master);
  result.bars = bar_energy(snapshot, result.master);

  std::vector<bool> drum_candidates(candidates, false);
  for (const auto& lane : lanes) {
    auto& measured = lane->measure;
    const auto& profile = measured.profile;
    const double own_crest = mix::crest_db(
        profile.sample_peak, profile.frames == 0 ? 0.0
                                                 : (profile.left_squares + profile.right_squares) /
                                                       (2.0 * static_cast<double>(profile.frames)));
    measured.drum = lane->drum_engine || (lane->candidate && own_crest >= transient_crest_db);
    if (lane->candidate)
      drum_candidates[*lane->candidate] = measured.drum;
    measured.bass = !measured.drum && mix::low_share(profile) >= bass_low_share;
    fundamentals(measured, lane->notes);
    if (measured.kick)
      for (const auto onset : lane->notes.onsets)
        measured.hit_seconds.push_back(static_cast<double>(onset) / static_cast<double>(rate));
    measured.clicks = lane->clicks(rate);
  }
  const auto drum_crest = crest.crest_db(drum_candidates);
  result.have_drums = drum_crest.has_value();
  result.drum_crest_db = drum_crest.value_or(0.0);
  for (auto& lane : lanes)
    for (auto& [other, meter] : lane->meters)
      if (auto cancellation = meter.finish())
        result.kick_pairs.push_back(
            {lane->measure.track.id, other->measure.track.id, std::move(*cancellation)});
  for (auto& lane : lanes) {
    result.order.push_back(lane->measure.track.id);
    result.tracks.emplace(lane->measure.track.id, std::move(lane->measure));
  }
  if (!report_progress(1.0, "Measuring"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  return R::success(std::move(result));
}

// ---- Report --------------------------------------------------------------------------

namespace {

struct Builder {
  const Measurements& m;
  const project::Snapshot& snapshot;
  const Target& goal;
  Report out;

  [[nodiscard]] const TrackMeasure& at(project::TrackId id) const { return m.tracks.at(id); }
  [[nodiscard]] TrackRef track_ref(project::TrackId id) const {
    return ref(snapshot, at(id).track);
  }
  [[nodiscard]] std::string name(project::TrackId id) const { return track_ref(id).name; }
  [[nodiscard]] std::string plain(const TrackMeasure& t) const {
    return plain_name(snapshot, t.track);
  }
  [[nodiscard]] std::uint64_t frame_of_hop(std::size_t hop) const {
    return static_cast<std::uint64_t>(hop) * m.master.short_hop;
  }
  [[nodiscard]] std::uint64_t frame_of_seconds(double seconds) const {
    return static_cast<std::uint64_t>(std::max(0.0, seconds) * snapshot.sample_rate);
  }

  // The loudest master hop between two frequencies: where a band problem
  // is easiest to hear.
  [[nodiscard]] std::uint64_t loudest_frame(double low_hz, double high_hz) const {
    const auto first = mix::band_of(low_hz);
    const auto last = mix::band_of(high_hz);
    std::size_t best = 0;
    double most = -1.0;
    for (std::size_t i = 0; i < m.master.hop_bands.size(); ++i) {
      double sum = 0.0;
      for (auto b = first; b <= last; ++b)
        sum += m.master.hop_bands[i][b];
      if (sum > most) {
        most = sum;
        best = m.master.hop_index[i];
      }
    }
    return frame_of_hop(best);
  }

  [[nodiscard]] std::optional<project::InsertSlotId>
  effect_slot(const std::vector<project::EffectInsert>& chain, std::string_view engine,
              bool last) const {
    std::optional<project::InsertSlotId> found;
    for (const auto& insert : chain) {
      const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor);
      if (patch != nullptr && patch->engine == engine) {
        found = insert.id;
        if (!last)
          break;
      }
    }
    return found;
  }

  // An existing built-in effect's dense values by id.
  [[nodiscard]] std::optional<std::vector<double>>
  effect_values(const std::vector<project::EffectInsert>& chain, project::InsertSlotId slot) const {
    const auto insert = std::ranges::find(chain, slot, &project::EffectInsert::id);
    if (insert == chain.end())
      return std::nullopt;
    const auto* patch = std::get_if<project::NativeEffectPatch>(&insert->processor);
    if (patch == nullptr)
      return std::nullopt;
    auto resolved = native_inserts::resolve_native_effect(*patch);
    if (!resolved)
      return std::nullopt;
    return std::move(resolved.value().values);
  }
  [[nodiscard]] static double value_of(std::string_view engine, const std::vector<double>& values,
                                       std::string_view id) {
    const auto* found = native_dsp::effects::find_engine(engine);
    const auto index =
        found == nullptr ? std::nullopt : native_dsp::find_parameter(*found->descriptor, id);
    return index && *index < values.size() ? values[*index] : 0.0;
  }
  // One parameter of a built-in effect engine.
  struct EngineParameter {
    std::string_view engine;
    std::string_view id;
  };
  [[nodiscard]] static double parameter_minimum(EngineParameter parameter) {
    const auto* found = native_dsp::effects::find_engine(parameter.engine);
    const auto index = found == nullptr
                           ? std::nullopt
                           : native_dsp::find_parameter(*found->descriptor, parameter.id);
    return index ? found->descriptor->parameters[*index].minimum : mix::silence_db;
  }
  [[nodiscard]] static std::vector<double> default_values(std::string_view engine) {
    const auto* found = native_dsp::effects::find_engine(engine);
    std::vector<double> values;
    if (found != nullptr)
      for (const auto& parameter : found->descriptor->parameters)
        values.push_back(parameter.default_value);
    return values;
  }

  // ---- Checks ----

  void sub() {
    double f0 = 0.0;
    for (const auto id : m.order) {
      const auto& t = at(id);
      double own = t.lowest_fundamental_hz;
      if (t.kick)
        own = mix::strongest_low_peak_hz(t.profile).value_or(0.0);
      if ((t.bass || t.kick) && own > 0.0)
        f0 = f0 == 0.0 ? own : std::min(f0, own);
    }
    if (f0 <= 0.0)
      return;
    const auto rumble = mix::find_sub_rumble(m.master, f0);
    out.meters.push_back({"sub", "Sub",
                          rumble ? signed_text(rumble->excess_db, 1) + " dB" : "0.0 dB",
                          "above the bass's lowest note",
                          rumble ? (rumble->excess_db >= mix::sub_bad_db ? "bad" : "warn") : "ok"});
    if (!rumble) {
      out.fine.push_back("Nothing rumbles under the bass's lowest note (" + hz_text(f0) + ").");
      return;
    }
    out.meters.back().label = "Sub " + band_text(rumble->low_hz, rumble->high_hz);
    // Blame: tracks carrying a fair share of the sub's energy.
    std::vector<std::pair<project::TrackId, double>> shares;
    double total = 0.0;
    for (const auto id : m.order) {
      const double p = mix::power_between(at(id).profile, rumble->low_hz, rumble->high_hz);
      shares.emplace_back(id, p);
      total += p;
    }
    Finding f;
    f.kind = Kind::sub;
    f.bad = rumble->excess_db >= mix::sub_bad_db;
    f.metric = rumble->excess_db;
    f.metric_label = "Sub excess";
    EffectFix fix;
    std::vector<std::pair<std::string, std::string>> params;
    std::string ids;
    // The rumble's centre, where the cut is judged.
    const double centre = std::sqrt(rumble->low_hz * rumble->high_hz);
    std::vector<std::string> too_close;
    for (const auto& [id, p] : shares) {
      if (!(total > 0.0) || p < total * sub_blame_share)
        continue;
      const auto& t = at(id);
      f.tracks.push_back(track_ref(id));
      ids += (ids.empty() ? "" : "+") + id.to_string();
      // The track's own lowest fundamental (its sub's, a kick's body): the
      // cut stays under it.
      const double own = !t.kick && t.lowest_fundamental_hz > 0.0
                             ? t.lowest_fundamental_hz
                             : mix::strongest_low_peak_hz(t.profile).value_or(f0);
      const double cutoff = own * contour_cutoff_ratio;
      if (high_pass_cut_db(cutoff, centre) < contour_min_cut_db) {
        too_close.push_back(plain(t) + " (" + hz_text(own) + ")");
        continue;
      }
      fix.edits.push_back({id,
                           std::nullopt,
                           std::size_t{0},
                           "contour",
                           {{"contour.model", contour_high_pass_24},
                            {"contour.cutoff", cutoff},
                            {"contour.resonance", 0.0},
                            {"contour.mix", 1.0},
                            {"contour.output", 0.0}}});
      params.emplace_back(f.tracks.size() > 1 ? "Cutoff, " + plain(t) : "Cutoff",
                          fixed(cutoff, 0) + " Hz");
    }
    if (f.tracks.empty())
      return;
    f.id = "sub:" + ids;
    f.band = Band{rumble->low_hz, rumble->high_hz, band_text(rumble->low_hz, rumble->high_hz)};
    f.title = "Rumble under the bass";
    f.text = "There is energy at " + f.band->label + ", below the bass's lowest real note (" +
             hz_text(f0) + "), " + signed_text(rumble->excess_db, 1) +
             " dB over what a clean low end carries there. It eats headroom and muddies big "
             "speakers.";
    f.worst_frame = loudest_frame(rumble->low_hz, rumble->high_hz);
    const auto& first = at(f.tracks.front().id);
    f.evidence.kind = "spectrum";
    f.evidence.caption =
        plain(first) + ", " +
        band_text(mix::sub_floor_hz, std::max(sub_plot_top_hz, f0 * sub_plot_span));
    f.evidence.x = mix::log_points(mix::sub_floor_hz, std::max(sub_plot_top_hz, f0 * sub_plot_span),
                                   evidence_points);
    f.evidence.before = mix::spectrum_points(first.profile, f.evidence.x.front(),
                                             f.evidence.x.back(), evidence_points);
    f.evidence.markers = {f0};
    if (fix.edits.empty()) {
      // A high-pass under the fundamental would barely touch the rumble.
      std::string list;
      for (std::size_t i = 0; i < too_close.size(); ++i)
        list += (i == 0 ? "" : i + 1 == too_close.size() ? " and " : ", ") + too_close[i];
      f.expected = "Too close under the lowest fundamental of " + list +
                   " to filter without thinning it: lower the sub or what adds the rumble";
      out.findings.push_back(std::move(f));
      return;
    }
    params.insert(params.begin(),
                  std::pair<std::string, std::string>{"Mode", "High-pass 24 dB/oct"});
    params.emplace_back("Resonance", "0");
    params.emplace_back("Mix", "100 %");
    const auto edited = std::get<project::TrackId>(fix.edits.front().path);
    f.fix =
        Fix{"Contour",
            fix.edits.size() == 1 ? "first on " + plain(at(edited)) : "first on each blamed track",
            params, std::move(fix)};
    f.expected = band_text(rumble->low_hz, rumble->high_hz) + " down " +
                 fixed(rumble->excess_db, 1) + " dB or more · fundamental kept";
    out.findings.push_back(std::move(f));
  }

  void mono() {
    const auto loss = mix::find_mono_loss(m.master);
    const double worst = loss ? loss->loss_db : 0.0;
    out.meters.push_back({"mono",
                          loss ? "Mono loss, " + band_text(mix::band_low(loss->first_band),
                                                           mix::band_high(loss->last_band))
                               : "Mono loss",
                          fixed(worst, 1) + " dB", "lost when folded to mono",
                          !loss                       ? "ok"
                          : worst >= mix::mono_bad_db ? "bad"
                                                      : "warn"});
    if (!loss) {
      out.fine.push_back("Folded to mono, no band loses " + fixed(mix::mono_loss_threshold_db, 1) +
                         " dB or more.");
      return;
    }
    const double low = mix::band_low(loss->first_band);
    const double high = mix::band_high(loss->last_band);
    // Blame: the track with the most side energy in the span.
    std::optional<project::TrackId> culprit;
    double most = 0.0;
    for (const auto id : m.order) {
      const auto side = mix::band_side(at(id).profile);
      double sum = 0.0;
      for (auto b = loss->first_band; b <= loss->last_band; ++b)
        sum += side[b];
      if (sum > most) {
        most = sum;
        culprit = id;
      }
    }
    if (!culprit)
      return;
    const auto& t = at(*culprit);
    Finding f;
    f.kind = Kind::mono;
    f.bad = loss->loss_db >= mix::mono_bad_db;
    f.id = "mono:" + culprit->to_string() + ":" + std::to_string(loss->first_band) + "-" +
           std::to_string(loss->last_band);
    f.metric = loss->loss_db;
    f.metric_label = "Mono loss";
    f.tracks.push_back(track_ref(*culprit));
    f.band = Band{low, high, band_text(low, high)};
    f.title = "The " +
              std::string(high <= 500.0    ? "low mids"
                          : low >= 2'000.0 ? "highs"
                                           : "mids") +
              " disappear in mono";
    f.text = "Folded to mono, " + f.band->label + " loses " + fixed(loss->loss_db, 1) + " dB. " +
             plain(t) +
             " is wide there with its sides out of phase, so they cancel on a "
             "club system or a phone.";
    f.worst_frame = loudest_frame(low, high);
    mono_evidence(f, loss->first_band, loss->last_band, m.master);
    // Fix: Vista's band covering the span, narrowed until the track's own
    // fold loses at most vista_target_loss_db there.
    const auto mid = mix::band_mid(t.profile);
    const auto side = mix::band_side(t.profile);
    double pm = 0.0;
    double ps = 0.0;
    for (auto b = loss->first_band; b <= loss->last_band; ++b) {
      pm += mid[b];
      ps += side[b];
    }
    double factor = 1.0;
    if (ps > 0.0)
      factor =
          std::sqrt(std::max(0.0, pm * (std::pow(10.0, vista_target_loss_db / 10.0) - 1.0) / ps));
    const double centre = std::sqrt(low * high);
    const auto existing = effect_slot(t.track.effects, "vista", false);
    std::vector<double> values =
        existing ? effect_values(t.track.effects, *existing).value_or(default_values("vista"))
                 : default_values("vista");
    const auto count =
        static_cast<std::size_t>(std::lround(value_of("vista", values, "vista.bands"))) + 1;
    std::size_t band = 0;
    for (const auto* xover : {"vista.xover_1", "vista.xover_2", "vista.xover_3"})
      if (centre >= value_of("vista", values, xover))
        ++band;
    band = std::min(band, count - 1);
    const auto width_id = "vista.width_" + std::to_string(band + 1);
    const double overall = value_of("vista", values, "vista.width");
    const double current = value_of("vista", values, width_id);
    const double net = current * overall;
    const double target_net =
        std::clamp(net * std::min(1.0, factor), vista_min_width, std::min(1.0, net));
    const double new_width = overall > 0.0 ? std::clamp(target_net / overall, 0.0, 2.0) : current;
    EffectFix fix;
    fix.edits.push_back({*culprit, existing, std::nullopt, "vista", {{width_id, new_width}}});
    const auto label = "Band " + std::to_string(band + 1) + " width";
    f.fix = Fix{"Vista",
                existing ? "your existing one on " + plain(t) : "last on " + plain(t),
                {{label, fixed(current, 2) + " → " + fixed(new_width, 2)},
                 {"Net width", "≈ " + fixed(target_net, 2)},
                 {"Other bands", "unchanged"}},
                std::move(fix)};
    f.expected = "Mono loss " + fixed(loss->loss_db, 1) + " → under " +
                 fixed(mix::mono_loss_threshold_db, 1) + " dB";
    out.findings.push_back(std::move(f));
  }

  void mono_evidence(Finding& f, std::size_t first, std::size_t last, const mix::Profile& p) {
    const auto losses = mix::band_mono_loss(p);
    f.evidence.kind = "mono";
    f.evidence.caption = "Mono minus stereo per band";
    const auto from = first > evidence_band_margin ? first - evidence_band_margin : 0;
    const auto to = std::min(mix::band_count - 1, last + evidence_band_margin);
    for (auto b = from; b <= to; ++b) {
      f.evidence.x.push_back(mix::band_centre(b));
      f.evidence.before.push_back(-losses[b]);
    }
    f.evidence.markers = {mix::band_low(first), mix::band_high(last)};
  }

  void mono_lows() {
    const double ratio = mix::low_side_db(m.master);
    if (ratio <= mix::low_side_threshold_db) {
      out.fine.push_back("Lows under " + hz_text(mix::lock_crossover_hz) + " are mono (side " +
                         fixed(ratio, 0) + " dB under the mid).");
      return;
    }
    std::optional<project::TrackId> culprit;
    double most = 0.0;
    for (const auto id : m.order) {
      const auto& p = at(id).profile;
      double side = 0.0;
      for (std::size_t k = 0; k < p.side.size(); ++k)
        if (static_cast<double>(k) * p.fine_bin_hz() < mix::lock_crossover_hz)
          side += p.side[k];
      if (side > most) {
        most = side;
        culprit = id;
      }
    }
    if (!culprit)
      return;
    const auto& t = at(*culprit);
    Finding f;
    f.kind = Kind::mono_lows;
    f.bad = true;
    f.id = "mono_lows:" + culprit->to_string();
    f.metric = ratio;
    f.metric_label = "Low side";
    f.tracks.push_back(track_ref(*culprit));
    f.band =
        Band{mix::sub_floor_hz, mix::lock_crossover_hz, "under " + hz_text(mix::lock_crossover_hz)};
    f.title = "The lows are wide";
    f.text = "Under " + hz_text(mix::lock_crossover_hz) + " the sides sit only " +
             fixed(-ratio, 1) + " dB under the middle, mostly from " + plain(t) +
             ". Club systems sum the lows to mono, so wide lows thin out or cancel there.";
    f.worst_frame = loudest_frame(mix::sub_floor_hz, mix::lock_crossover_hz);
    mono_evidence(f, mix::band_of(mix::sub_floor_hz), mix::band_of(mix::lock_crossover_hz),
                  m.master);
    const auto existing = effect_slot(t.track.effects, "vista", false);
    EffectFix fix;
    fix.edits.push_back({*culprit,
                         existing,
                         std::nullopt,
                         "vista",
                         {{"vista.mono_bass", 1.0}, {"vista.mono_freq", mix::lock_crossover_hz}}});
    f.fix = Fix{"Vista",
                existing ? "your existing one on " + plain(t) : "last on " + plain(t),
                {{"Mono Bass", "on"}, {"Below", hz_text(mix::lock_crossover_hz)}},
                std::move(fix)};
    f.expected = "Lows under " + hz_text(mix::lock_crossover_hz) + " mono";
    out.findings.push_back(std::move(f));
  }

  void ringing() {
    std::size_t peaks = 0;
    for (const auto id : m.order) {
      const auto& t = at(id);
      // Judged against the track's own sound: its notes' harmonics (an
      // audio track's detected pitch) and the partials that decay with the
      // rest of it are what it is meant to sound like.
      const auto found = mix::find_resonances(t.profile, t.played_hz, mix::resonance_max_peaks);
      if (found.empty())
        continue;
      peaks += found.size();
      Finding f;
      f.kind = Kind::ringing;
      f.bad = true;
      f.id = "ringing:" + id.to_string();
      double low = found.front().hz;
      double high = low;
      double strongest = 0.0;
      double longest = 0.0;
      std::vector<double> freqs;
      for (const auto& r : found) {
        low = std::min(low, r.hz);
        high = std::max(high, r.hz);
        strongest = std::max(strongest, r.prominence_db);
        longest = std::max(longest, r.decay_ratio);
        freqs.push_back(r.hz);
      }
      std::ranges::sort(freqs);
      f.metric = strongest;
      f.metric_label = "Strongest peak";
      f.tracks.push_back(track_ref(id));
      const double margin = 1.0 + 0.5 * (ringing_plot_margin - 1.0);
      f.band = Band{low / margin, high * margin, band_text(low / margin, high * margin)};
      std::string list;
      for (std::size_t i = 0; i < freqs.size(); ++i)
        list += (i == 0 ? "" : i + 1 == freqs.size() ? " and " : ", ") + hz_text(freqs[i]);
      f.title = plain(t) + " rings at " +
                (freqs.size() == 1 ? std::string("one pitch")
                                   : std::to_string(freqs.size()) + " pitches");
      f.text = std::string(freqs.size() == 1 ? "A narrow peak at " : "Narrow peaks at ") + list +
               " stand" + (freqs.size() == 1 ? "s" : "") + " up to " + fixed(strongest, 0) +
               " dB over the sound around " + (freqs.size() == 1 ? "it" : "them") +
               " and keep ringing up to " + fixed(longest, 1) + "× longer than the rest of " +
               plain(t) +
               "'s sound after each hit. That is the metallic edge you might hear as "
               "\"fizz\".";
      f.worst_frame = loudest_frame(low, high);
      f.evidence.kind = "spectrum";
      f.evidence.caption =
          plain(t) + " decay, " + band_text(low / ringing_plot_margin, high * ringing_plot_margin);
      f.evidence.x =
          mix::log_points(low / ringing_plot_margin, high * ringing_plot_margin, evidence_points);
      f.evidence.before = mix::spectrum_points(t.profile, f.evidence.x.front(), f.evidence.x.back(),
                                               evidence_points, true);
      f.evidence.markers = freqs;
      EffectFix fix;
      EffectEdit edit{id, std::nullopt, std::nullopt, "temper", {{"temper.depth", 0.0}}};
      std::string freq_text;
      for (std::size_t i = 0; i < freqs.size(); ++i) {
        const auto n = std::to_string(i + 1);
        edit.values.insert(edit.values.end(),
                           {{"temper.band_on_" + n, 1.0},
                            {"temper.band_kind_" + n, 0.0},
                            {"temper.band_freq_" + n, freqs[i]},
                            {"temper.band_q_" + n, temper_cut_q},
                            {"temper.band_range_" + n, temper_cut_range_db},
                            {"temper.band_threshold_" + n, temper_cut_threshold_db},
                            {"temper.band_ratio_" + n, temper_cut_ratio}});
        freq_text += (i == 0 ? "" : " · ") + fixed(freqs[i], 0);
      }
      fix.edits.push_back(std::move(edit));
      f.fix = Fix{"Temper",
                  "last on " + plain(t),
                  {{"Bands",
                    std::to_string(freqs.size()) + " dynamic cut" + (freqs.size() == 1 ? "" : "s")},
                   {"Freqs", freq_text},
                   {"Q", fixed(temper_cut_q, 0)},
                   {"Range", fixed(temper_cut_range_db, 0) + " dB"},
                   {"Suppressor", "off"}},
                  std::move(fix)};
      f.expected = "Peaks down " + fixed(-temper_cut_range_db, 0) + " dB · rest of " + plain(t) +
                   " ±" + fixed(unchanged_db, 1) + " dB";
      out.findings.push_back(std::move(f));
    }
    out.meters.push_back(
        {"ringing", "Ringing peaks", std::to_string(peaks),
         "narrow, ≥ " + fixed(mix::resonance_threshold_db, 0) + " dB over neighbours",
         peaks == 0 ? "ok" : "bad"});
    if (peaks == 0)
      out.fine.push_back("No track rings at a narrow pitch after its hits.");
  }

  void kick_bass() {
    const mix::Cancellation* deepest = nullptr;
    std::optional<std::pair<project::TrackId, project::TrackId>> pair;
    for (const auto& measured : m.kick_pairs) {
      const auto other = m.tracks.find(measured.other);
      if (other == m.tracks.end() || !other->second.bass)
        continue;
      if (deepest == nullptr || measured.cancellation.deepest_db > deepest->deepest_db) {
        deepest = &measured.cancellation;
        pair = std::pair{measured.kick, measured.other};
      }
    }
    const bool flagged =
        deepest != nullptr && deepest->deepest_db >= mix::cancellation_threshold_db;
    // The percentile read as "one hit in n".
    const auto one_in =
        "one hit in " + std::to_string(std::lround(1.0 / (1.0 - mix::dip_percentile)));
    out.meters.push_back({"dip", "Kick/bass dip",
                          deepest != nullptr ? fixed(deepest->deepest_db, 1) + " dB" : "–",
                          "this deep on " + one_in, flagged ? "warn" : "ok"});
    if (!flagged) {
      if (deepest != nullptr)
        out.fine.push_back("Kick and bass add up under the hits (deepest dip " +
                           fixed(deepest->deepest_db, 1) + " dB).");
      return;
    }
    // A flagged dip has a deepest pair.
    if (!pair)
      return;
    const auto [kick, bass] = *pair;
    Finding f;
    f.kind = Kind::kick_bass;
    f.bad = false;
    f.id = "kick_bass:" + kick.to_string() + ":" + bass.to_string();
    f.metric = deepest->deepest_db;
    f.metric_label = "Deepest dip";
    f.tracks = {track_ref(kick), track_ref(bass)};
    f.band = Band{40.0, mix::lock_crossover_hz, band_text(40.0, mix::lock_crossover_hz)};
    f.title = "Kick and bass cancel under the hits";
    f.text = "Where the kick's tail and the bass overlap under " + hz_text(mix::lock_crossover_hz) +
             " they cancel: by " + fixed(deepest->deepest_db, 1) + " dB or more on " + one_in +
             ", " + fixed(deepest->worst_db, 1) + " dB on the worst (in " +
             fixed(mix::dip_window_seconds * kilo, 0) +
             " ms windows), so the low end goes thin and changes from bar to bar.";
    f.worst_frame = frame_of_seconds(deepest->at_seconds);
    f.evidence.kind = "time";
    f.evidence.caption = "Kick + bass sum under " + hz_text(mix::lock_crossover_hz) + ", worst hit";
    f.evidence.before = deepest->envelope;
    for (std::size_t i = 0; i < f.evidence.before.size(); ++i)
      f.evidence.x.push_back(static_cast<double>(i) * mix::dip_plot_step_seconds -
                             mix::dip_plot_before_seconds);
    f.evidence.markers = {0.0};
    const auto& bass_track = at(bass).track;
    if (engine_of(bass_track) == native_dsp::synth::lock_bass_engine &&
        !kick_bass_lock_state(snapshot, bass).locked) {
      f.fix = Fix{"Lock",
                  plain(at(bass)) + " + kicks",
                  {{"Bass duck", "under each kick"},
                   {"Mono lows", "below " + hz_text(mix::lock_crossover_hz)},
                   {"Kick tone", "kept"}},
                  LockFix{bass}};
      f.expected = "Deepest dip down to a few dB";
    } else {
      f.expected = engine_of(bass_track) == native_dsp::synth::lock_bass_engine
                       ? "Lock is already on: tune the kick or duck the bass with Pump"
                       : "Duck the bass under the kick with Pump";
    }
    out.findings.push_back(std::move(f));
  }

  void loudness() {
    const auto& l = m.loudness;
    out.loudness.integrated = l.integrated.value_or(mix::silence_db);
    out.loudness.true_peak = l.true_peak_db;
    out.loudness.range = l.range.value_or(0.0);
    out.loudness.crest = m.have_drums ? m.drum_crest_db : 0.0;
    const bool clips = m.master.sample_peak >= clip_level;
    out.meters.push_back({"level", "Level", l.integrated ? fixed(*l.integrated, 1) : "–",
                          clips ? "LUFS · the master clips" : "LUFS · no clipping",
                          clips ? "bad" : "ok"});
    if (l.integrated && (*l.integrated < goal.lufs_low || *l.integrated > goal.lufs_high)) {
      const double off = *l.integrated < goal.lufs_low ? *l.integrated - goal.lufs_low
                                                       : *l.integrated - goal.lufs_high;
      Finding f;
      f.kind = Kind::loudness;
      f.bad = std::fabs(off) > target_near;
      f.id = "loudness:integrated";
      f.metric = *l.integrated;
      f.metric_label = "Integrated";
      f.title = off < 0.0 ? "Quieter than a " + std::string(goal.name) + " wants"
                          : "Louder than a " + std::string(goal.name) + " wants";
      f.text = "The song measures " + fixed(*l.integrated, 1) + " LUFS against " +
               fixed(goal.lufs_low, 0) + " to " + fixed(goal.lufs_high, 0) + " LUFS for " +
               std::string(goal.name) + ": " + fixed(std::fabs(off), 1) + " LU " +
               (off < 0.0 ? "under" : "over") + ". " + std::string(goal.note);
      f.worst_frame = 0;
      energy_evidence(f);
      f.expected = off < 0.0
                       ? "Turn up the master limiter's Gain by about " + fixed(-off, 1) + " dB"
                       : "Turn down the master by about " + fixed(off, 1) + " dB";
      out.findings.push_back(std::move(f));
    }
    if (l.true_peak_db > goal.true_peak || clips) {
      Finding f;
      f.kind = Kind::loudness;
      f.bad = true;
      f.id = "loudness:true_peak";
      f.metric = l.true_peak_db;
      f.metric_label = "True peak";
      f.title = clips ? "The master clips" : "True peak over the ceiling";
      f.text = "The loudest peak reaches " + signed_text(l.true_peak_db, 1) + " dBTP, " +
               fixed(l.true_peak_db - goal.true_peak, 1) + " dB over the " +
               signed_text(goal.true_peak, 1) + " dBTP a " + std::string(goal.name) +
               " allows. Converters and the next limiter turn that into distortion.";
      f.worst_frame = 0;
      energy_evidence(f);
      // The master's last limiter that runs, and what its ceiling lets past.
      std::optional<project::InsertSlotId> existing;
      std::size_t after = 0;
      for (const auto& insert : snapshot.master_effects) {
        if (!insert.enabled)
          continue;
        const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor);
        if (patch != nullptr && patch->engine == "ceiling") {
          existing = insert.id;
          after = 0;
        } else if (existing) {
          ++after;
        }
      }
      std::optional<double> current;
      if (existing)
        if (const auto values = effect_values(snapshot.master_effects, *existing))
          current = value_of("ceiling", *values, "ceiling.ceiling");
      const double ceiling =
          std::max(lowered_ceiling_db(
                       {.current = current, .target = goal.true_peak, .true_peak = l.true_peak_db}),
                   parameter_minimum({.engine = "ceiling", .id = "ceiling.ceiling"}));
      if (current && l.true_peak_db > *current) {
        const double over = l.true_peak_db - *current;
        const double fader_db = 20.0 * std::log10(snapshot.master_level);
        f.text += " Your limiter's ceiling is " + signed_text(*current, 1) + " dB, so " +
                  fixed(over, 1) + " dB comes on top of it: " +
                  (snapshot.master_level > 1.0
                       ? "the master fader adds " + fixed(fader_db, 1) + " dB after it."
                   : after > 0 ? std::to_string(after) + " effect" + (after == 1 ? "" : "s") +
                                     " after it on the master add to it."
                               : "it holds sample peaks, and the peaks between samples run over "
                                 "(it has no true-peak mode).");
      }
      EffectFix fix;
      fix.edits.push_back({project::MasterSignalPath{},
                           existing,
                           std::nullopt,
                           "ceiling",
                           {{"ceiling.ceiling", ceiling}}});
      f.fix = Fix{"Ceiling",
                  existing ? "your master limiter" : "last on the master",
                  {{"Ceiling", current ? fixed(*current, 1) + " → " + fixed(ceiling, 1) + " dB"
                                       : fixed(ceiling, 1) + " dB"},
                   {"Gain", existing ? "unchanged" : "0 dB"}},
                  std::move(fix)};
      f.expected = "True peak at or under " + signed_text(goal.true_peak, 1) + " dBTP";
      out.findings.push_back(std::move(f));
    } else {
      out.fine.push_back("True peak " + signed_text(l.true_peak_db, 1) + " dBTP: under the " +
                         signed_text(goal.true_peak, 1) + " dBTP ceiling; nothing clips.");
    }
  }

  void energy_evidence(Finding& f) {
    f.evidence.kind = "time";
    f.evidence.caption = "Loudness per bar";
    for (const auto& bar : m.bars) {
      f.evidence.x.push_back(static_cast<double>(bar.bar));
      f.evidence.before.push_back(bar.lufs);
    }
  }

  void clicks() {
    std::size_t total = 0;
    for (const auto id : m.order) {
      const auto& t = at(id);
      if (t.clicks.empty())
        continue;
      total += t.clicks.size();
      Finding f;
      f.kind = Kind::click;
      f.bad = true;
      f.id = "click:" + id.to_string();
      f.metric = static_cast<double>(t.clicks.size());
      f.metric_label = "Clicks";
      f.tracks.push_back(track_ref(id));
      std::vector<std::string> bars;
      FadeFix fix;
      for (const auto& click : t.clicks) {
        const auto tick = snapshot.tempo.sample_to_tick({static_cast<std::int64_t>(click.frame)},
                                                        snapshot.sample_rate);
        std::string where =
            "at " + fixed(static_cast<double>(click.frame) / snapshot.sample_rate, 2) + " s";
        if (tick) {
          const auto position = snapshot.meter.locate(tick.value());
          if (position)
            where = "bar " + std::to_string(position.value().bar);
        }
        bars.push_back(where);
        const auto clip = std::ranges::find(t.track.clips, click.clip, &project::Clip::id);
        if (clip == t.track.clips.end())
          continue;
        const auto frames = clip_frames(snapshot, clip->placement);
        if (!frames || frames->second <= frames->first)
          continue;
        const double duration = static_cast<double>(frames->second - frames->first);
        const double fraction = click_fade_seconds * snapshot.sample_rate / duration;
        auto existing = std::ranges::find(fix.fades, click.clip,
                                          &std::pair<project::ClipId, project::ClipFades>::first);
        if (existing == fix.fades.end()) {
          fix.fades.emplace_back(click.clip, clip->fades);
          existing = fix.fades.end() - 1;
        }
        auto& fades = existing->second;
        if (click.start) {
          fades.in_fraction = fraction;
          fades.in_curve = project::FadeCurve::smoothstep;
        } else {
          fades.out_fraction = fraction;
          fades.out_curve = project::FadeCurve::smoothstep;
        }
      }
      // A clip too short for both fades keeps none of the new ones.
      std::erase_if(fix.fades, [](const auto& entry) {
        return entry.second.in_fraction + entry.second.out_fraction > 1.0;
      });
      std::string list;
      for (std::size_t i = 0; i < bars.size(); ++i)
        list += (i == 0 ? "" : i + 1 == bars.size() ? " and " : ", ") + bars[i];
      f.title = t.clicks.size() == 1 ? "A click at a clip edge"
                                     : std::to_string(t.clicks.size()) + " clicks at clip edges";
      f.text = "Clips on " + plain(t) + " that start or end mid-wave make a click: " + list +
               ". They are short but cut through on a club system.";
      f.worst_frame = t.clicks.front().frame;
      f.evidence.kind = "time";
      f.evidence.caption =
          bars.front() + ", ±" + fixed(click_plot_seconds * kilo, 0) + " ms around the edge";
      const auto& samples = t.clicks.front().samples;
      for (std::size_t i = 0; i < samples.size(); ++i) {
        f.evidence.x.push_back(
            (static_cast<double>(i) - static_cast<double>(samples.size()) / 2.0) /
            snapshot.sample_rate * kilo);
        f.evidence.before.push_back(samples[i]);
      }
      f.evidence.markers = {0.0};
      if (!fix.fades.empty())
        f.fix = Fix{"Fades",
                    "on the " + std::to_string(t.clicks.size()) + " clip edge" +
                        (t.clicks.size() == 1 ? "" : "s"),
                    {{"Fade", fixed(click_fade_seconds * kilo, 0) + " ms"},
                     {"Shape", "smooth (S-curve)"},
                     {"Edges", list}},
                    std::move(fix)};
      f.expected = "Clicks gone · no audible change elsewhere";
      out.findings.push_back(std::move(f));
    }
    if (total == 0)
      out.fine.push_back("No audio clip clicks where it starts or ends.");
  }

  void energy() {
    out.energy = m.bars;
    std::vector<double> lufs;
    lufs.reserve(m.bars.size());
    for (const auto& bar : m.bars)
      lufs.push_back(bar.lufs);
    std::vector<mix::Section> sections;
    // Markers name the sections when they are there.
    if (!snapshot.markers.empty() && !lufs.empty()) {
      std::vector<std::pair<std::size_t, std::string>> starts;
      for (const auto& marker : snapshot.markers) {
        const auto position = snapshot.meter.locate(marker.position);
        if (!position || position.value().bar < 1)
          continue;
        const auto bar = static_cast<std::size_t>(position.value().bar - 1);
        if (bar < lufs.size())
          starts.emplace_back(bar,
                              marker.name.empty() ? "Bar " + std::to_string(bar + 1) : marker.name);
      }
      if (!starts.empty() && starts.front().first > 0)
        starts.insert(starts.begin(), std::pair<std::size_t, std::string>{0, "Start"});
      for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto last = i + 1 < starts.size() ? starts[i + 1].first - 1 : lufs.size() - 1;
        if (last < starts[i].first)
          continue;
        sections.push_back(
            {starts[i].second, starts[i].first, last, mix::mean_lufs(lufs, starts[i].first, last)});
      }
    }
    if (sections.empty())
      sections = mix::split_sections(lufs);
    // Named drops and breaks judge themselves; otherwise energy names them.
    auto judged = sections;
    const bool named = std::ranges::any_of(judged, [](const mix::Section& s) {
      std::string lower = s.name;
      std::ranges::transform(lower, lower.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return lower.find("drop") != std::string::npos || lower.find("break") != std::string::npos;
    });
    if (!named)
      mix::rename_by_energy(judged);
    for (auto& section : sections) {
      ++section.first_bar; // One-based for display.
      ++section.last_bar;
    }
    out.sections = sections;
    const auto issue = mix::find_energy_issue(judged);
    if (!issue) {
      out.energy_note = "Each drop sits at or above the one before, and above the breaks.";
      out.fine.push_back(out.energy_note);
      return;
    }
    const auto& earlier = judged[issue->earlier];
    const auto& later = judged[issue->later];
    const auto earlier_name = sections[issue->earlier].name;
    const auto later_name = sections[issue->later].name;
    Finding f;
    f.kind = Kind::energy;
    f.bad = false;
    f.id = "energy:" + std::to_string(earlier.first_bar) + "-" + std::to_string(later.first_bar);
    f.metric = issue->lu;
    f.metric_label = issue->break_louder ? "Break over drop" : "Drop under drop";
    f.title = issue->break_louder ? "A break is louder than a drop"
                                  : "A later drop is quieter than an earlier one";
    f.text = issue->break_louder
                 ? later_name + " is " + fixed(issue->lu, 1) + " LU louder than " + earlier_name +
                       ", so the drop does not lift."
                 : later_name + " is " + fixed(issue->lu, 1) + " LU quieter than " + earlier_name +
                       ": the song loses energy where it should hit hardest.";
    out.energy_note = f.text;
    f.worst_frame = 0;
    if (const auto tick =
            snapshot.meter.tick_at({static_cast<std::int64_t>(later.first_bar) + 1, 1, 0}))
      if (const auto sample = snapshot.tempo.tick_to_sample(tick.value(), snapshot.sample_rate))
        f.worst_frame = static_cast<std::uint64_t>(std::max<std::int64_t>(0, sample.value().value));
    energy_evidence(f);
    for (const auto& section : sections)
      f.evidence.markers.push_back(static_cast<double>(section.first_bar));
    f.expected = "Thin what keeps playing into " + later_name + " or lift it";
    out.findings.push_back(std::move(f));
  }

  // How much a track matters to the groove, named: kicks, drums and basses
  // first, then leads and vocals, then everything else, pads and effects
  // last.
  [[nodiscard]] int importance(project::TrackId id) const {
    const auto& t = at(id);
    if (t.kick || t.drum || t.bass)
      return 3;
    std::string lower = plain(t);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto* word : {"lead", "vox", "vocal", "hook"})
      if (lower.find(word) != std::string::npos)
        return 2;
    for (const auto* word : {"pad", "fx", "atmos", "texture", "noise", "drone"})
      if (lower.find(word) != std::string::npos)
        return 0;
    const auto engine = engine_of(t.track);
    if (engine == "ember" || engine == "dust")
      return 0;
    return 1;
  }

  void masking() {
    bool any = false;
    for (const auto heard : m.order) {
      std::optional<mix::Masking> worst;
      std::optional<project::TrackId> by;
      for (const auto other : m.order) {
        if (other == heard || importance(heard) <= importance(other))
          continue;
        const auto found =
            mix::find_masking(at(heard).profile, post_fader, at(other).profile, post_fader);
        if (found && (!worst || found->loss_db > worst->loss_db)) {
          worst = found;
          by = other;
        }
      }
      if (!worst)
        continue;
      any = true;
      const double low = mix::band_low(worst->first_band);
      const double high = mix::band_high(worst->last_band);
      const double centre = std::sqrt(low * high);
      const double cut = std::min(worst->loss_db, masking_max_cut_db);
      const auto& masker = at(*by);
      Finding f;
      f.kind = Kind::masking;
      f.bad = false;
      f.id = "masking:" + heard.to_string() + ":" + by->to_string() + ":" +
             std::to_string(worst->first_band);
      f.metric = worst->loss_db;
      f.metric_label = "Masked";
      f.tracks = {track_ref(*by), track_ref(heard)};
      f.band = Band{low, high, band_text(low, high)};
      f.title = plain(masker) + " hides " + plain(at(heard));
      f.text = "While both play, " + plain(masker) + " covers " + plain(at(heard)) + " at " +
               f.band->label + ", where its character lives: it reads " + fixed(worst->loss_db, 1) +
               " dB quieter there than when it plays alone.";
      f.worst_frame = frame_of_hop(worst->worst_hop);
      const auto losses =
          mix::masking_loss(at(heard).profile, post_fader, masker.profile, post_fader);
      f.evidence.kind = "spectrum";
      f.evidence.caption = plain(at(heard)) + "'s loss per band while " + plain(masker) + " plays";
      const auto from =
          worst->first_band > evidence_band_margin ? worst->first_band - evidence_band_margin : 0;
      const auto to = std::min(mix::band_count - 1, worst->last_band + evidence_band_margin);
      for (auto b = from; b <= to; ++b) {
        f.evidence.x.push_back(mix::band_centre(b));
        f.evidence.before.push_back(-losses[b]);
      }
      f.evidence.markers = {low, high};
      EffectFix fix;
      fix.edits.push_back({*by,
                           std::nullopt,
                           std::nullopt,
                           "temper",
                           {{"temper.depth", 0.0},
                            {"temper.band_on_1", 1.0},
                            {"temper.band_kind_1", 0.0},
                            {"temper.band_freq_1", centre},
                            {"temper.band_q_1", masking_cut_q},
                            {"temper.band_range_1", -cut},
                            {"temper.band_threshold_1", temper_cut_threshold_db},
                            {"temper.band_ratio_1", temper_cut_ratio}}});
      f.fix = Fix{"Temper",
                  "last on " + plain(masker),
                  {{"Band", hz_text(centre) + ", Q " + fixed(masking_cut_q, 1)},
                   {"Range", fixed(-cut, 1) + " dB"},
                   {"Elsewhere", plain(masker) + " untouched"}},
                  std::move(fix)};
      f.expected = plain(at(heard)) + " clearer at " + f.band->label;
      out.findings.push_back(std::move(f));
    }
    if (!any)
      out.fine.push_back("No track hides a more important one's character.");
  }

  void cleanup() {
    // Tracks over full scale before the master.
    bool clipped = false;
    for (const auto id : m.order) {
      const auto& t = at(id);
      if (t.profile.sample_peak < clip_level)
        continue;
      clipped = true;
      Finding f;
      f.kind = Kind::clipping;
      f.bad = false;
      f.id = "clipping:" + id.to_string();
      f.metric = mix::power_db(t.profile.sample_peak * t.profile.sample_peak);
      f.metric_label = "Peak";
      f.tracks.push_back(track_ref(id));
      f.title = plain(t) + " peaks over full scale";
      f.text = plain(t) + " reaches " + signed_text(f.metric, 1) +
               " dBFS after its fader. The mix bus has room, but a device or export after it "
               "may not: bring its fader or output down.";
      f.expected = "Peaks under 0 dBFS";
      out.findings.push_back(std::move(f));
    }
    if (!clipped)
      out.fine.push_back("No track peaks over full scale; " + fixed(-m.master.peak_db(), 1) +
                         " dB of headroom on the master's loudest peak.");
    const auto leaning = mix::worst_balance(m.master);
    const double balance = leaning.db;
    if (std::fabs(balance) > mix::balance_threshold_db) {
      Finding f;
      f.kind = Kind::balance;
      f.bad = false;
      f.id = "balance:master";
      f.metric = std::fabs(balance);
      f.metric_label = "Left/right";
      f.title = std::string("The mix leans ") + (balance > 0.0 ? "left" : "right");
      const double low = mix::band_low(leaning.band);
      const double high = mix::band_high(leaning.band);
      f.band = Band{low, high, band_text(low, high)};
      f.text = "At " + f.band->label + " the " + std::string(balance > 0.0 ? "left" : "right") +
               " side is " + fixed(std::fabs(balance), 1) +
               " dB louder than the other. Check pans and wide effects there.";
      f.worst_frame = loudest_frame(low, high);
      f.expected = "Left and right within " + fixed(mix::balance_threshold_db, 1) + " dB";
      out.findings.push_back(std::move(f));
    } else {
      out.fine.push_back("Left/right balance sits within " + fixed(std::fabs(balance), 1) +
                         " dB across the spectrum.");
    }
    bool offset = false;
    for (const auto id : m.order) {
      const auto& t = at(id);
      const double dc = mix::dc_offset(t.profile);
      if (dc < mix::dc_threshold)
        continue;
      offset = true;
      Finding f;
      f.kind = Kind::dc;
      f.bad = false;
      f.id = "dc:" + id.to_string();
      f.metric = mix::power_db(dc * dc);
      f.metric_label = "DC";
      f.tracks.push_back(track_ref(id));
      f.title = plain(t) + " has a DC offset";
      f.text = plain(t) + " sits off zero by " + signed_text(f.metric, 0) +
               " dBFS. It wastes headroom and thumps when the track starts or stops; a gentle "
               "high-pass removes it.";
      f.expected = "Centred on zero";
      out.findings.push_back(std::move(f));
    }
    if (!offset)
      out.fine.push_back("No DC offset on any track.");
    bool silent = false;
    for (const auto id : m.order) {
      const auto& t = at(id);
      if (t.profile.peak_db() > mix::silent_peak_db)
        continue;
      silent = true;
      Finding f;
      f.kind = Kind::silent;
      f.bad = false;
      f.id = "silent:" + id.to_string();
      f.metric = t.profile.peak_db();
      f.metric_label = "Peak";
      f.tracks.push_back(track_ref(id));
      f.title = plain(t) + " makes no sound";
      f.text = plain(t) + " has clips but renders silent. Check its instrument, its devices "
                          "and its clips' levels.";
      f.expected = "Audible, or removed";
      out.findings.push_back(std::move(f));
    }
    if (!silent)
      out.fine.push_back("Every track with clips makes sound.");
  }
};

} // namespace

Report report(const Measurements& measured, std::string_view target_id) {
  const auto& snapshot = *measured.snapshot;
  Builder b{measured, snapshot, target(target_id), {}};
  auto& out = b.out;
  out.revision = snapshot.revision;
  out.project_name = measured.project_name;
  out.track_count = measured.order.size();
  out.duration_seconds = measured.master.seconds();
  for (std::size_t i = 0; i < mix::display_points; ++i)
    out.freqs.push_back(mix::display_frequency(i));
  out.spectrum = mix::display_spectrum(measured.master);
  out.mono = mix::display_mono_spectrum(measured.master);
  out.bands = mix::band_powers(measured.master);
  b.sub();
  b.mono();
  b.mono_lows();
  b.ringing();
  b.kick_bass();
  b.loudness();
  b.clicks();
  b.energy();
  b.masking();
  b.cleanup();
  // The meters in the panel's order: sub, mono, ringing, dip, level.
  const std::array<std::string_view, 5> order{"sub", "mono", "ringing", "dip", "level"};
  std::vector<Meter> meters;
  for (const auto id : order) {
    const auto found = std::ranges::find(out.meters, id, &Meter::id);
    meters.push_back(found != out.meters.end()
                         ? *found
                         : Meter{std::string(id), id == "sub" ? "Sub" : std::string(id), "–",
                                 "no bass to judge", "ok"});
  }
  out.meters = std::move(meters);
  // Bad findings first, then by kind.
  std::ranges::stable_sort(out.findings,
                           [](const Finding& a, const Finding& c) { return a.bad && !c.bad; });
  return out;
}

bool same_mix(const project::Snapshot& a, const project::Snapshot& b) {
  if (a.tracks.size() != b.tracks.size() || !(a.master_effects == b.master_effects) ||
      a.master_level != b.master_level || a.rebalance != b.rebalance || !same_timing(a, b))
    return false;
  for (std::size_t i = 0; i < a.tracks.size(); ++i) {
    if (a.tracks[i].soloed == b.tracks[i].soloed) {
      if (!(a.tracks[i] == b.tracks[i]))
        return false;
      continue;
    }
    auto copy = b.tracks[i];
    copy.soloed = a.tracks[i].soloed;
    if (!(a.tracks[i] == copy))
      return false;
  }
  return true;
}

Result<void> apply(ProjectSession& session, const Finding& finding,
                   const project::Snapshot& measured) {
  if (!finding.fix)
    return Result<void>::failure({ErrorCode::invalid_argument, "This finding has no fix"});
  if (!same_mix(session.snapshot(), measured))
    return Result<void>::failure(
        {ErrorCode::conflict, "The project changed since the check: run it again"});
  return std::visit(
      [&](const auto& action) -> Result<void> {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, EffectFix>) {
          auto done = session.effects().apply_effect_edits(action.edits);
          return done ? Result<void>::success() : Result<void>::failure(done.error());
        } else if constexpr (std::is_same_v<T, FadeFix>) {
          return session.audio_clips().set_audio_clips_fades(action.fades);
        } else {
          return session.instruments().lock_kick_and_bass(action.bass, true);
        }
      },
      finding.fix->action);
}

namespace {

// The finding's evidence measured again on `now`, on the same x.
std::vector<double> evidence_now(const Finding& finding, const Measurements& now) {
  const auto& e = finding.evidence;
  if (e.x.empty())
    return {};
  const auto track = finding.tracks.empty()
                         ? std::nullopt
                         : std::optional<project::TrackId>(finding.tracks.front().id);
  const auto measured = track ? now.tracks.find(*track) : now.tracks.end();
  switch (finding.kind) {
  case Kind::sub:
  case Kind::ringing:
    if (measured == now.tracks.end())
      return {};
    return mix::spectrum_points(measured->second.profile, e.x.front(), e.x.back(), e.x.size(),
                                finding.kind == Kind::ringing);
  case Kind::mono:
  case Kind::mono_lows: {
    const auto losses = mix::band_mono_loss(now.master);
    std::vector<double> y;
    y.reserve(e.x.size());
    for (const double hz : e.x)
      y.push_back(-losses[mix::band_of(hz)]);
    return y;
  }
  case Kind::loudness:
  case Kind::energy: {
    std::vector<double> y;
    for (const double bar : e.x) {
      const auto found =
          std::ranges::find(now.bars, static_cast<std::size_t>(bar), &BarEnergy::bar);
      y.push_back(found == now.bars.end() ? mix::silence_db : found->lufs);
    }
    return y;
  }
  // A fixed edge no longer clicks; its samples are not kept.
  case Kind::click:
  case Kind::kick_bass:
  case Kind::masking:
  case Kind::clipping:
  case Kind::balance:
  case Kind::dc:
  case Kind::silent:
    return {};
  }
  return {};
}

} // namespace

void judge(Finding& finding, const Report& before, const Report& after, const Measurements& now) {
  finding.applied = true;
  const auto again = std::ranges::find(after.findings, finding.id, &Finding::id);
  std::string moved;
  if (again == after.findings.end())
    moved = finding.metric_label + " gone";
  else
    moved = finding.metric_label + " " + fixed(finding.metric, 1) + " → " + fixed(again->metric, 1);
  // Everything outside the fix's band should stay put.
  double largest = 0.0;
  std::size_t changed = 0;
  for (std::size_t b = 0; b < mix::band_count; ++b) {
    if (finding.band && mix::band_high(b) > finding.band->low &&
        mix::band_low(b) < finding.band->high)
      continue;
    const double was = mix::power_db(before.bands[b]);
    const double is = mix::power_db(after.bands[b]);
    if (was <= mix::silence_db + 30.0 && is <= mix::silence_db + 30.0)
      continue;
    const double delta = std::fabs(is - was);
    if (delta > unchanged_db) {
      ++changed;
      largest = std::max(largest, delta);
    }
  }
  finding.result =
      moved + (changed == 0
                   ? " · elsewhere within " + fixed(unchanged_db, 1) + " dB"
                   : " · " + std::to_string(changed) + " band" + (changed == 1 ? "" : "s") +
                         " elsewhere moved up to " + fixed(largest, 1) + " dB");
  auto y = evidence_now(finding, now);
  if (y.size() == finding.evidence.x.size())
    finding.evidence.after = std::move(y);
}

// ---- Reference -----------------------------------------------------------------------

Result<ReferenceTrack> measure_reference(const fs::path& path,
                                         const std::function<bool()>& cancelled) {
  using R = Result<ReferenceTrack>;
  ReferenceTrack reference;
  reference.name = path.stem().string();
  std::vector<float> left(read_block_frames);
  std::vector<float> right(read_block_frames);
  const auto run = [&](auto& reader, std::uint32_t rate, std::uint64_t frames,
                       auto convert) -> Result<void> {
    if (rate == 0 || static_cast<double>(frames) > reference_max_seconds * rate)
      return Result<void>::failure(
          {ErrorCode::invalid_argument, "The reference is too long or unreadable"});
    mix::ProfileBuilder builder(rate);
    std::uint64_t at = 0;
    while (at < frames) {
      if (cancelled && cancelled())
        return Result<void>::failure({ErrorCode::cancelled, "Reference cancelled"});
      auto got = convert(reader, at);
      if (!got)
        return Result<void>::failure(got.error());
      if (got.value() == 0)
        break;
      builder.push(std::span<const float>(left).first(got.value()),
                   std::span<const float>(right).first(got.value()));
      at += got.value();
    }
    reference.profile = std::move(builder).finish();
    return Result<void>::success();
  };
  if (auto wav = media::WavReader::open(path)) {
    std::vector<media::StereoFrame> frames(read_block_frames);
    auto done = run(wav.value(), wav.value().sample_rate(), wav.value().frame_count(),
                    [&](media::WavReader& reader, std::uint64_t at) -> Result<std::size_t> {
                      auto got = reader.read_at(at, frames);
                      if (got)
                        for (std::size_t i = 0; i < got.value(); ++i) {
                          left[i] = frames[i].left;
                          right[i] = frames[i].right;
                        }
                      return got;
                    });
    if (!done)
      return R::failure(done.error());
  } else {
#if OSCILLATE_HAVE_CODEC
    auto encoded = codec::EncodedReader::open(path);
    if (!encoded)
      return R::failure(encoded.error());
    std::vector<codec::StereoFrame> frames(read_block_frames);
    const auto info = encoded.value().info();
    auto done = run(encoded.value(), info.sample_rate, info.frames,
                    [&](codec::EncodedReader& reader, std::uint64_t at) -> Result<std::size_t> {
                      auto got = reader.read_at(at, frames);
                      if (got)
                        for (std::size_t i = 0; i < got.value(); ++i) {
                          left[i] = frames[i].left;
                          right[i] = frames[i].right;
                        }
                      return got;
                    });
    if (!done)
      return R::failure(done.error());
#else
    return R::failure(wav.error());
#endif
  }
  const auto measured = mix::loudness(reference.profile);
  if (!measured.integrated)
    return R::failure({ErrorCode::invalid_argument, "The reference is silent or too short"});
  reference.lufs = *measured.integrated;
  return R::success(std::move(reference));
}

} // namespace oscillate::application::mix_check
