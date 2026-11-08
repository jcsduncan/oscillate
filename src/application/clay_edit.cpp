#include "oscillate/application/clay_edit.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/clay_dsp/limits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace oscillate::application::clay {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr double seconds_per_minute = 60.0;
// Playback schedules at most this many notes in a project: an on and an off
// each.
constexpr std::size_t max_followed_notes = project::max_midi_events_per_project / 2;
// The most pattern repeats' notes one follow walks, under the clip or near
// it, so the walk ends however short the pattern.
constexpr std::size_t max_walked_notes = project::max_midi_events_per_project;
// Division names and their steps per quarter note.
constexpr std::array<std::pair<std::string_view, int>, 4> divisions{
    {{"1/4", 1}, {"1/8", 2}, {"1/16", 4}, {"1/32", 8}}};

[[nodiscard]] double clamp_bend(double value) noexcept { return std::clamp(value, -1.0, 1.0); }

} // namespace

Result<ClipTime> ClipTime::of(const project::Snapshot& snapshot, const project::Clip& clip) {
  const auto source = std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
  if (source == snapshot.sources.end())
    return Result<ClipTime>::failure({ErrorCode::not_found, "The clip's source is missing"});
  auto plan =
      processing::prepare_clip_frame_plan(clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!plan)
    return Result<ClipTime>::failure(plan.error());
  return Result<ClipTime>::success(
      ClipTime{*source, std::move(plan).value(), snapshot.tempo, snapshot.sample_rate});
}

std::int64_t ClipTime::first_source_frame() const noexcept {
  return static_cast<std::int64_t>(plan_.source_start_frame);
}

std::int64_t ClipTime::end_source_frame() const noexcept {
  return static_cast<std::int64_t>(plan_.source_start_frame + plan_.warp.source_frames());
}

double ClipTime::first_song_seconds() const noexcept {
  return static_cast<double>(plan_.project_start_frame) / project_rate_;
}

double ClipTime::end_song_seconds() const noexcept {
  return static_cast<double>(plan_.project_end_frame()) / project_rate_;
}

double ClipTime::song_frame(double source_frame) const noexcept {
  const auto segments = plan_.warp.segments();
  const double relative = source_frame - static_cast<double>(plan_.source_start_frame);
  const auto start = static_cast<double>(plan_.project_start_frame);
  if (segments.empty())
    return start;
  // The segment holding the frame, or the nearest edge one to extend.
  const auto* segment = &segments.front();
  for (const auto& candidate : segments) {
    segment = &candidate;
    if (relative < static_cast<double>(candidate.end.source_frame))
      break;
  }
  const auto source_span = static_cast<double>(segment->source_frames());
  const auto output_span = static_cast<double>(segment->output_frames());
  const double rate = source_span > 0.0 ? output_span / source_span : 1.0;
  return start + static_cast<double>(segment->begin.output_frame) +
         (relative - static_cast<double>(segment->begin.source_frame)) * rate;
}

double ClipTime::source_frame(double song_frame) const noexcept {
  const auto segments = plan_.warp.segments();
  const double relative = song_frame - static_cast<double>(plan_.project_start_frame);
  const auto start = static_cast<double>(plan_.source_start_frame);
  if (segments.empty())
    return start;
  const auto* segment = &segments.front();
  for (const auto& candidate : segments) {
    segment = &candidate;
    if (relative < static_cast<double>(candidate.end.output_frame))
      break;
  }
  const auto source_span = static_cast<double>(segment->source_frames());
  const auto output_span = static_cast<double>(segment->output_frames());
  const double rate = output_span > 0.0 ? source_span / output_span : 1.0;
  return start + static_cast<double>(segment->begin.source_frame) +
         (relative - static_cast<double>(segment->begin.output_frame)) * rate;
}

double ClipTime::song_seconds(double source_frame) const noexcept {
  return song_frame(source_frame) / project_rate_;
}

double ClipTime::source_frame_at_seconds(double seconds) const noexcept {
  return source_frame(seconds * project_rate_);
}

Result<timeline::TickPosition> ClipTime::tick(double source_frame) const {
  const auto frame = std::max(0.0, std::round(song_frame(source_frame)));
  if (frame > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 2.0)
    return Result<timeline::TickPosition>::failure(
        {ErrorCode::invalid_argument, "A Clay note is too far into the song"});
  return tempo_.sample_to_tick({static_cast<std::int64_t>(frame)}, project_rate_);
}

Result<ClipRef> find_clip(const project::Snapshot& snapshot, project::ClipId id) {
  for (const auto& track : snapshot.tracks) {
    const auto clip = std::ranges::find(track.clips, id, &project::Clip::id);
    if (clip == track.clips.end())
      continue;
    const auto source =
        std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
    if (source == snapshot.sources.end())
      return Result<ClipRef>::failure({ErrorCode::not_found, "The clip's source is missing"});
    return Result<ClipRef>::success({&track, &*clip, &*source});
  }
  return Result<ClipRef>::failure({ErrorCode::not_found, "That audio clip is not in the project"});
}

std::optional<project::MusicalKey> source_key(const project::Snapshot& snapshot,
                                              project::SourceId source) {
  const auto review =
      std::ranges::find(snapshot.source_key_reviews, source, &project::SourceKeyReview::source);
  if (review == snapshot.source_key_reviews.end())
    return std::nullopt;
  return review->correction ? review->correction : review->detected;
}

project::ClayTake new_take(const project::Snapshot& snapshot, const project::Clip& clip,
                           project::ClayMode mode) {
  project::ClayTake take;
  take.mode = mode;
  take.analysis_version = clay_dsp::analysis_version;
  take.key = source_key(snapshot, clip.source);
  if (!take.key)
    take.key = snapshot.key;
  return take;
}

std::optional<int> steps_per_quarter(std::string_view division) {
  const auto found =
      std::ranges::find(divisions, division, &std::pair<std::string_view, int>::first);
  if (found == divisions.end())
    return std::nullopt;
  return found->second;
}

Result<clay_dsp::TimeGrid> quantize_grid(const project::Snapshot& snapshot, const ClipTime& time,
                                         int steps,
                                         const std::optional<persistence::GrooveTemplate>& groove) {
  if (groove)
    steps = static_cast<int>(groove->steps_per_quarter);
  if (steps <= 0)
    return Result<clay_dsp::TimeGrid>::failure(
        {ErrorCode::invalid_argument, "A quantize grid needs steps"});
  // The song's grid from bar 1, at the tempo where the clip starts.
  const auto clip_start = time.song_frame(static_cast<double>(time.first_source_frame()));
  double bpm = snapshot.tempo.events().empty() ? 0.0 : snapshot.tempo.events().front().bpm;
  for (const auto& event : snapshot.tempo.events()) {
    auto at = snapshot.tempo.tick_to_sample(event.tick, snapshot.sample_rate);
    if (at && static_cast<double>(at.value().value) <= clip_start)
      bpm = event.bpm;
  }
  if (!(bpm > 0.0))
    return Result<clay_dsp::TimeGrid>::failure(
        {ErrorCode::unavailable, "The song has no tempo to quantize to"});
  const double song_step = static_cast<double>(snapshot.sample_rate) * seconds_per_minute / bpm /
                           static_cast<double>(steps);
  // Source frames per song frame where the clip starts.
  const double ratio = time.source_frame(clip_start + song_step) - time.source_frame(clip_start);
  clay_dsp::TimeGrid grid;
  grid.origin = static_cast<std::int64_t>(std::llround(time.source_frame(0.0)));
  grid.step_frames = ratio;
  if (groove)
    for (const auto& step : groove->steps)
      grid.offsets.push_back(step.timing);
  return Result<clay_dsp::TimeGrid>::success(std::move(grid));
}

std::vector<clay_dsp::TargetNote> follow_targets(const project::Snapshot& snapshot,
                                                 const ClipTime& time, project::TrackId id) {
  std::vector<clay_dsp::TargetNote> targets;
  const auto track = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  if (track == snapshot.tracks.end())
    return targets;
  const auto first = static_cast<double>(time.first_source_frame());
  const auto end = static_cast<double>(time.end_source_frame());
  // The audio clip's song ticks: only the pattern repeats that reach them
  // are walked, so a short looping pattern costs what the clip covers.
  auto window_start = time.tick(first);
  auto window_end = time.tick(end);
  if (!window_start || !window_end)
    return targets;
  const auto source_at_tick = [&](std::int64_t tick) -> std::optional<double> {
    auto sample = snapshot.tempo.tick_to_sample({tick}, snapshot.sample_rate);
    if (!sample)
      return std::nullopt;
    return time.source_frame(static_cast<double>(sample.value().value));
  };
  std::size_t walked = 0;
  for (const auto& clip : track->midi_clips) {
    const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement);
    const auto pattern = clip.pattern_length.value;
    // A pattern whose notes all lie past its end is silent; every other
    // repeat walks at least one note, so the walk budget below bounds it.
    if (musical == nullptr || pattern <= 0 ||
        std::ranges::none_of(clip.notes,
                             [&clip](const auto& note) { return project::note_plays(clip, note); }))
      continue;
    const auto clip_start = musical->start.value;
    const auto clip_end = musical->end.value;
    // A looping clip repeats its pattern to its end. One repeat before the
    // window is walked too, so rounding between ticks and frames loses none.
    const auto span = clip.loop ? pattern : clip_end - clip_start;
    const auto skipped =
        std::max<std::int64_t>(0, (window_start.value().value - clip_start) / span - 1);
    const auto last = std::min(clip_end, window_end.value().value);
    for (auto origin = clip_start + skipped * span; origin < clip_end && origin <= last;
         origin += span) {
      // As playback hears them: notes past the pattern are silent, and a
      // note crossing its end is cut there.
      const auto pattern_end = std::min(origin + pattern, clip_end);
      for (const auto& note : clip.notes) {
        if (!project::note_plays(clip, note))
          continue;
        const auto note_start = origin + note.start.value;
        if (note_start >= pattern_end)
          continue;
        // The walk is bounded on its own, so notes outside the clip cannot
        // use up the targets.
        if (++walked > max_walked_notes) {
          std::ranges::sort(targets, {}, &clay_dsp::TargetNote::start);
          return targets;
        }
        // Cut at the pattern end before adding the origin, so a long note
        // cannot overflow.
        const auto note_end = origin + std::min(note.end.value, pattern_end - origin);
        auto start = source_at_tick(note_start);
        auto stop = source_at_tick(note_end);
        if (!start || !stop || *stop <= first || *start >= end)
          continue;
        targets.push_back({static_cast<std::int64_t>(std::llround(*start)),
                           static_cast<std::int64_t>(std::llround(*stop)), note.key});
        // Playback schedules no more notes than this, so neither does Clay.
        if (targets.size() == max_followed_notes) {
          std::ranges::sort(targets, {}, &clay_dsp::TargetNote::start);
          return targets;
        }
      }
      if (!clip.loop)
        break;
    }
  }
  std::ranges::sort(targets, {}, &clay_dsp::TargetNote::start);
  return targets;
}

Result<MidiClipOut> midi_clip_out(const ClipTime& time, const clay_dsp::MidiOut& midi) {
  MidiClipOut out;
  auto start = time.tick(static_cast<double>(time.first_source_frame()));
  auto end = time.tick(static_cast<double>(time.end_source_frame()));
  if (!start || !end)
    return Result<MidiClipOut>::failure(!start ? start.error() : end.error());
  out.start = start.value();
  std::int64_t last = end.value().value;
  for (const auto& note : midi.notes) {
    auto from = time.tick(static_cast<double>(note.start));
    auto to = time.tick(static_cast<double>(note.end));
    if (!from || !to)
      return Result<MidiClipOut>::failure(!from ? from.error() : to.error());
    const auto local_start = std::max<std::int64_t>(0, from.value().value - out.start.value);
    const auto local_end = std::max(local_start + 1, to.value().value - out.start.value);
    last = std::max(last, out.start.value + local_end);
    project::MidiNote made{
        project::NoteId::from_bits({0, 1}).value(),
        {local_start},
        {local_end},
        static_cast<std::uint8_t>(std::clamp(note.key, 0, foundation::max_midi_key)),
        static_cast<std::uint8_t>(std::clamp(note.velocity, foundation::min_note_velocity,
                                             foundation::max_note_velocity))};
    out.notes.push_back(made);
  }
  if (out.notes.size() > project::max_midi_notes_per_clip)
    return Result<MidiClipOut>::failure(
        {ErrorCode::resource_exhausted, "Too many Clay notes for one MIDI clip"});
  out.length = {std::max<std::int64_t>(1, last - out.start.value)};
  double needed = 0.0;
  for (const auto& bend : midi.bends)
    needed = std::max(needed, std::abs(bend.semitones));
  out.bend_range = std::ceil(std::max(needed, midi.bend_range));
  if (out.bend_range > 0.0) {
    std::int64_t previous = -1;
    for (const auto& bend : midi.bends) {
      auto at = time.tick(static_cast<double>(bend.frame));
      if (!at)
        return Result<MidiClipOut>::failure(at.error());
      const auto local = at.value().value - out.start.value;
      // Points are strictly increasing and inside the clip.
      if (local <= previous || local < 0 || local >= out.length.value)
        continue;
      previous = local;
      out.bends.push_back({local, clamp_bend(bend.semitones / out.bend_range)});
    }
  }
  return Result<MidiClipOut>::success(std::move(out));
}

} // namespace oscillate::application::clay
