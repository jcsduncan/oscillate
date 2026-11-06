#include "oscillate/application/midi_file_import.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using timeline::TickPosition;
using Plan = Result<MidiImportPlan>;

constexpr std::int64_t tpq = timeline::ticks_per_quarter;
constexpr std::uint8_t drum_channel = 9; // Channel 10, counted from zero.

template <typename Id> Result<Id> numbered(std::uint64_t& next) {
  if (next == 0)
    return Result<Id>::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  return Id::from_bits({0, next++});
}

// File ticks to project ticks: an exact rational rescale of absolute
// positions (round half up when the division does not divide the project's
// ticks per quarter), or elapsed SMPTE time on the project's tempo.
// Exact intermediate products of ticks, rates and divisions.
__extension__ using Wide = __int128;

class TickConverter {
public:
  TickConverter(const midi_file::Division& division, const timeline::TempoMap& tempo,
                std::uint32_t sample_rate, TickPosition origin)
      : division_(division), tempo_(tempo), sample_rate_(sample_rate), origin_(origin) {}

  [[nodiscard]] Result<std::int64_t> operator()(std::uint64_t file_tick) const {
    if (!division_.smpte) {
      const auto numerator = static_cast<Wide>(file_tick) * tpq;
      auto quotient = numerator / division_.ticks_per_quarter;
      if (2 * (numerator % division_.ticks_per_quarter) >= division_.ticks_per_quarter)
        ++quotient;
      if (quotient > std::numeric_limits<std::int64_t>::max() - origin_.value)
        return too_long();
      return Result<std::int64_t>::success(origin_.value + static_cast<std::int64_t>(quotient));
    }
    // 29 means 29.97 drop-frame: 30000/1001 frames per second.
    const std::int64_t fps_numerator =
        division_.frames_per_second == 29 ? 30'000 : division_.frames_per_second;
    const std::int64_t fps_denominator = division_.frames_per_second == 29 ? 1'001 : 1;
    const auto numerator =
        static_cast<Wide>(file_tick) * fps_denominator * static_cast<Wide>(sample_rate_);
    const auto denominator = static_cast<Wide>(fps_numerator) * division_.ticks_per_frame;
    auto samples = numerator / denominator;
    if (2 * (numerator % denominator) >= denominator)
      ++samples;
    auto start = tempo_.tick_to_sample(origin_, sample_rate_);
    if (!start)
      return Result<std::int64_t>::failure(start.error());
    if (samples > std::numeric_limits<std::int64_t>::max() - start.value().value)
      return too_long();
    auto tick = tempo_.sample_to_tick({start.value().value + static_cast<std::int64_t>(samples)},
                                      sample_rate_);
    if (!tick)
      return Result<std::int64_t>::failure(tick.error());
    return Result<std::int64_t>::success(tick.value().value);
  }

private:
  static Result<std::int64_t> too_long() {
    return Result<std::int64_t>::failure(
        {ErrorCode::resource_exhausted, "The MIDI file is too long for the project timeline"});
  }

  midi_file::Division division_;
  const timeline::TempoMap& tempo_;
  std::uint32_t sample_rate_;
  TickPosition origin_;
};

struct ImportNote {
  std::int64_t start;
  std::int64_t end;
  std::uint8_t key;
  std::uint8_t velocity;
};

struct ImportPedal {
  std::int64_t tick;
  project::MidiPedal pedal;
  double value;
};

struct Group {
  std::size_t file_track = 0;
  std::uint8_t channel = 0;
  std::vector<ImportNote> notes;
  std::vector<ImportPedal> pedals;
};

std::optional<project::MidiPedal> pedal_for(std::uint8_t controller) {
  switch (controller) {
  case 64:
    return project::MidiPedal::sustain;
  case 66:
    return project::MidiPedal::sostenuto;
  case 67:
    return project::MidiPedal::soft;
  default:
    return std::nullopt;
  }
}

class Bars {
public:
  explicit Bars(const timeline::MeterMap& meter) : meter_(meter) {}

  // The start of the bar containing `tick`.
  [[nodiscard]] Result<std::int64_t> start(std::int64_t tick) const {
    auto located = meter_.locate({tick});
    if (!located)
      return Result<std::int64_t>::failure(located.error());
    auto bar = meter_.tick_at({located.value().bar, 1, 0});
    if (!bar)
      return Result<std::int64_t>::failure(bar.error());
    return Result<std::int64_t>::success(bar.value().value);
  }
  // The first bar boundary at or after `tick`.
  [[nodiscard]] Result<std::int64_t> ceiling(std::int64_t tick) const {
    auto located = meter_.locate({tick});
    if (!located)
      return Result<std::int64_t>::failure(located.error());
    const auto& at = located.value();
    if (at.beat == 1 && at.tick_within_beat == 0)
      return Result<std::int64_t>::success(tick);
    auto next = meter_.tick_at({at.bar + 1, 1, 0});
    if (!next)
      return Result<std::int64_t>::failure(next.error());
    return Result<std::int64_t>::success(next.value().value);
  }

private:
  const timeline::MeterMap& meter_;
};

struct ClipSpan {
  std::int64_t start;
  std::int64_t end;
  std::size_t first_note;
  std::size_t note_count;
};

// Whole-bar clips with at most the model's note count each, split only where
// no note crosses the cut.
Result<std::vector<ClipSpan>> clip_spans(const Group& group, const Bars& bars) {
  using R = Result<std::vector<ClipSpan>>;
  std::int64_t first = std::numeric_limits<std::int64_t>::max();
  std::int64_t last = std::numeric_limits<std::int64_t>::min();
  if (!group.notes.empty()) {
    first = group.notes.front().start;
    for (const auto& note : group.notes)
      last = std::max(last, note.end);
  }
  if (!group.pedals.empty()) {
    first = std::min(first, group.pedals.front().tick);
    last = std::max(last, group.pedals.back().tick + 1);
  }
  auto start = bars.start(first);
  if (!start)
    return R::failure(start.error());
  std::vector<ClipSpan> spans;
  ClipSpan current{start.value(), 0, 0, 0};
  std::int64_t reach = current.start + 1;
  for (std::size_t index = 0; index < group.notes.size(); ++index) {
    const auto& note = group.notes[index];
    if (current.note_count == project::max_midi_notes_per_clip) {
      auto cut = bars.ceiling(reach);
      if (!cut)
        return R::failure(cut.error());
      if (note.start < cut.value())
        return R::failure({ErrorCode::resource_exhausted,
                           "A MIDI track is too dense to split into clips of " +
                               std::to_string(project::max_midi_notes_per_clip) + " notes"});
      current.end = cut.value();
      spans.push_back(current);
      auto next = bars.start(note.start);
      if (!next)
        return R::failure(next.error());
      current = {next.value(), 0, index, 0};
      reach = current.start + 1;
    }
    ++current.note_count;
    reach = std::max(reach, note.end);
  }
  reach = std::max(reach, last);
  auto end = bars.ceiling(reach);
  if (!end)
    return R::failure(end.error());
  current.end = end.value();
  spans.push_back(current);
  return R::success(std::move(spans));
}

// One clip's pedal lanes: the pedal state chased to its start, then its
// changes, with repeats dropped.
Result<std::vector<project::MidiPedalLane>> clip_pedals(const Group& group, const ClipSpan& span) {
  using R = Result<std::vector<project::MidiPedalLane>>;
  std::vector<project::MidiPedalLane> lanes;
  for (const auto pedal :
       {project::MidiPedal::sustain, project::MidiPedal::sostenuto, project::MidiPedal::soft}) {
    double held = 0.0;
    std::vector<project::MidiPedalPoint> points;
    for (const auto& event : group.pedals) {
      if (event.pedal != pedal || event.tick >= span.end)
        continue;
      if (event.tick < span.start) {
        held = event.value;
        continue;
      }
      const TickPosition position{event.tick - span.start};
      if (!points.empty() && points.back().position == position)
        points.back().value = event.value;
      else
        points.push_back({position, event.value});
    }
    // Chase a held pedal into the clip unless the clip sets it at its start.
    if (held > 0.0 && (points.empty() || points.front().position.value != 0))
      points.insert(points.begin(), {TickPosition{0}, held});
    std::vector<project::MidiPedalPoint> changes;
    double previous = 0.0;
    for (const auto& point : points) {
      if (point.value != previous)
        changes.push_back(point);
      previous = point.value;
    }
    if (changes.empty())
      continue;
    if (changes.size() > project::max_midi_pedal_points_per_lane)
      return R::failure(
          {ErrorCode::resource_exhausted, "A MIDI clip has too many pedal changes to import"});
    lanes.push_back({pedal, std::move(changes)});
  }
  return R::success(std::move(lanes));
}

std::string plural(std::size_t count, const char* one, const char* many) {
  return std::to_string(count) + ' ' + (count == 1 ? one : many);
}

} // namespace

Result<MidiImportPlan> plan_midi_import(const project::Snapshot& snapshot,
                                        const midi_file::File& file,
                                        const MidiImportOptions& options, MidiImportIds ids) {
  try {
    MidiImportPlan plan;
    auto& result = plan.result;
    if (options.zero_based_bar < 0 ||
        options.zero_based_bar == std::numeric_limits<std::int64_t>::max())
      return Plan::failure({ErrorCode::invalid_argument, "MIDI import bar is out of range"});
    auto origin = snapshot.meter.tick_at({options.zero_based_bar + 1, 1, 0});
    if (!origin)
      return Plan::failure(origin.error());
    const TickPosition at = origin.value();

    const project::Track* target = nullptr;
    if (options.target == MidiImportOptions::Target::track) {
      if (!options.track)
        return Plan::failure({ErrorCode::invalid_argument, "Choose a track to import MIDI into"});
      const auto found = std::ranges::find(snapshot.tracks, *options.track, &project::Track::id);
      if (found == snapshot.tracks.end())
        return Plan::failure({ErrorCode::not_found, "MIDI import track not found"});
      if (found->kind != project::TrackKind::instrument)
        return Plan::failure(
            {ErrorCode::invalid_argument, "MIDI can only be imported into an instrument track"});
      target = &*found;
    }

    // Tempo and meter: the project's before the import bar, the file's after.
    const auto* tempo = &snapshot.tempo;
    const auto* meter = &snapshot.meter;
    std::optional<timeline::TempoMap> imported_tempo;
    std::optional<timeline::MeterMap> imported_meter;
    if (options.import_tempo_and_meter && file.division.smpte) {
      result.notices.push_back(
          "The file is timed in SMPTE frames, so its tempo map was not imported");
    } else if (options.import_tempo_and_meter) {
      const TickConverter musical(file.division, snapshot.tempo, snapshot.sample_rate, at);
      std::vector<timeline::TempoEvent> tempo_events;
      for (const auto& event : snapshot.tempo.events()) {
        if (event.tick < at)
          tempo_events.push_back({event.tick, event.bpm, event.shape_to_next});
      }
      if (file.tempo.empty() || file.tempo.front().tick != 0)
        tempo_events.push_back({at, 120.0, timeline::TempoShape::step}); // The SMF default.
      for (const auto& change : file.tempo) {
        auto tick = musical(change.tick);
        if (!tick)
          return Plan::failure(tick.error());
        tempo_events.push_back({{tick.value()},
                                60'000'000.0 / static_cast<double>(change.microseconds_per_quarter),
                                timeline::TempoShape::step});
      }
      // The file's tempo changes are steps. A project ramp running into the
      // import bar would otherwise retarget to the file's tempo and move
      // everything before it, so it holds its tempo instead.
      for (auto& event : tempo_events) {
        if (event.tick >= at)
          event.shape_to_next = timeline::TempoShape::step;
      }
      for (std::size_t index = 0; index + 1 < tempo_events.size(); ++index) {
        if (tempo_events[index].tick < at && tempo_events[index + 1].tick >= at &&
            tempo_events[index].shape_to_next != timeline::TempoShape::step) {
          tempo_events[index].shape_to_next = timeline::TempoShape::step;
          result.notices.push_back("The tempo ramp ending at the import bar now holds its tempo");
        }
      }
      auto map = timeline::TempoMap::create(std::move(tempo_events), snapshot.tempo.revision() + 1);
      if (!map)
        return Plan::failure(map.error());
      imported_tempo.emplace(std::move(map).value());
      tempo = &*imported_tempo;
      result.tempo_imported = true;

      std::vector<timeline::MeterEvent> meter_events;
      for (const auto& event : snapshot.meter.events()) {
        if (event.tick < at)
          meter_events.push_back(event);
      }
      bool representable = true;
      if (file.meter.empty() || file.meter.front().tick != 0)
        meter_events.push_back({at, 4, 4}); // The SMF default.
      for (const auto& signature : file.meter) {
        if (signature.numerator > 32 || signature.denominator > 64) {
          representable = false;
          break;
        }
        auto tick = musical(signature.tick);
        if (!tick)
          return Plan::failure(tick.error());
        meter_events.push_back({{tick.value()},
                                signature.numerator,
                                static_cast<std::uint8_t>(signature.denominator)});
      }
      auto meter_map = representable ? timeline::MeterMap::create(std::move(meter_events),
                                                                  snapshot.meter.revision() + 1)
                                     : Result<timeline::MeterMap>::failure(
                                           {ErrorCode::unsupported, "unrepresentable signature"});
      if (meter_map) {
        imported_meter.emplace(std::move(meter_map).value());
        meter = &*imported_meter;
        result.meter_imported = true;
      } else {
        result.notices.push_back("The file's time signatures do not fall on bar lines the "
                                 "project can represent, so the project's meter was kept");
      }
    }
    if (imported_tempo)
      plan.operations.emplace_back(project::ReplaceTempoMap{*imported_tempo});
    if (imported_meter)
      plan.operations.emplace_back(project::ReplaceMeterMap{*imported_meter});

    // Notes and pedals, grouped by destination.
    const TickConverter convert(file.division, *tempo, snapshot.sample_rate, at);
    std::map<std::pair<std::size_t, std::uint8_t>, Group> groups;
    const auto group_for = [&](std::size_t track, std::uint8_t channel) -> Group& {
      const auto key = target ? std::pair<std::size_t, std::uint8_t>{0, 0}
                              : std::pair<std::size_t, std::uint8_t>{track, channel};
      auto& group = groups[key];
      group.file_track = key.first;
      group.channel = key.second;
      return group;
    };
    for (std::size_t index = 0; index < file.tracks.size(); ++index) {
      for (const auto& note : file.tracks[index].notes) {
        auto start = convert(note.start);
        auto end = convert(note.end);
        if (!start || !end)
          return Plan::failure(!start ? start.error() : end.error());
        // SMPTE rounding could collapse a very short note; keep one tick.
        const auto finish = std::max(end.value(), start.value() + 1);
        group_for(index, note.channel)
            .notes.push_back({start.value(), finish, note.key, note.velocity});
      }
      for (const auto& controller : file.tracks[index].controllers) {
        const auto pedal = pedal_for(controller.number);
        if (!pedal)
          continue;
        auto tick = convert(controller.tick);
        if (!tick)
          return Plan::failure(tick.error());
        group_for(index, controller.channel)
            .pedals.push_back({tick.value(), *pedal,
                               controller.value / static_cast<double>(foundation::max_midi_value)});
      }
    }
    std::size_t ignored_programs = 0;
    for (const auto& track : file.tracks)
      ignored_programs += track.programs.size();

    const Bars bars(*meter);
    const auto& default_engine = native_dsp::synth::default_engine();
    const auto* drum_engine = native_dsp::synth::find_engine("pulse");
    for (auto& [key, group] : groups) {
      if (group.notes.empty() && group.pedals.empty())
        continue;
      std::ranges::stable_sort(group.notes, [](const ImportNote& a, const ImportNote& b) {
        return a.start != b.start ? a.start < b.start : a.key < b.key;
      });
      std::ranges::stable_sort(
          group.pedals, [](const ImportPedal& a, const ImportPedal& b) { return a.tick < b.tick; });
      std::optional<project::TrackId> destination;
      if (target) {
        destination = target->id;
      } else {
        auto id = numbered<project::TrackId>(ids.next_track);
        if (!id)
          return Plan::failure(id.error());
        destination = id.value();
        const auto& engine =
            group.channel == drum_channel && drum_engine ? *drum_engine : default_engine;
        auto patch = native_dsp::synth::native_patch_for_preset(engine, std::nullopt);
        if (!patch)
          return Plan::failure(patch.error());
        plan.operations.emplace_back(
            project::AddTrack{*destination, project::TrackKind::instrument, std::nullopt});
        plan.operations.emplace_back(
            project::SetNativeInstrumentPatch{*destination, std::move(patch).value()});
      }
      const auto track_id = *destination;
      if (result.tracks.empty() || result.tracks.back() != track_id)
        result.tracks.push_back(track_id);
      auto spans = clip_spans(group, bars);
      if (!spans)
        return Plan::failure(spans.error());
      for (const auto& span : spans.value()) {
        auto clip_id = numbered<project::ClipId>(ids.next_clip);
        if (!clip_id)
          return Plan::failure(clip_id.error());
        const TickPosition length{span.end - span.start};
        project::MidiClip clip{clip_id.value(),
                               project::MusicalInterval{{span.start}, {span.end}},
                               length,
                               false,
                               {},
                               1};
        clip.notes.reserve(span.note_count);
        for (std::size_t n = span.first_note; n < span.first_note + span.note_count; ++n) {
          const auto& note = group.notes[n];
          auto note_id = numbered<project::NoteId>(ids.next_note);
          if (!note_id)
            return Plan::failure(note_id.error());
          clip.notes.push_back({note_id.value(),
                                {note.start - span.start},
                                {note.end - span.start},
                                note.key,
                                note.velocity});
        }
        auto lanes = clip_pedals(group, span);
        if (!lanes)
          return Plan::failure(lanes.error());
        for (const auto& lane : lanes.value())
          result.pedal_points += lane.points.size();
        clip.pedals = std::move(lanes).value();
        result.notes += clip.notes.size();
        ++result.clips;
        plan.operations.emplace_back(project::AddMidiClip{track_id, std::move(clip)});
      }
    }
    if (result.clips == 0)
      return Plan::failure({ErrorCode::invalid_argument, "The MIDI file has no notes or pedals"});

    const auto& repairs = file.repairs;
    if (repairs.unmatched_note_offs != 0)
      result.notices.push_back("Ignored " +
                               plural(repairs.unmatched_note_offs, "note-off", "note-offs") +
                               " with no matching note-on");
    if (repairs.notes_closed_at_track_end != 0)
      result.notices.push_back(
          "Ended " +
          plural(repairs.notes_closed_at_track_end, "unfinished note", "unfinished notes") +
          " at the end of their track");
    if (repairs.zero_length_notes != 0)
      result.notices.push_back(
          "Gave " + plural(repairs.zero_length_notes, "zero-length note", "zero-length notes") +
          " one file tick");
    if (repairs.missing_end_of_track != 0)
      result.notices.push_back("Ended " + plural(repairs.missing_end_of_track, "track", "tracks") +
                               " that had no End of Track event at their last event");
    if (ignored_programs != 0)
      result.notices.push_back("Did not map " +
                               plural(ignored_programs, "program change", "program changes") +
                               " to instruments");
    return Plan::success(std::move(plan));
  } catch (const std::bad_alloc&) {
    return Plan::failure(
        {ErrorCode::resource_exhausted, "Not enough memory to import the MIDI file"});
  }
}

} // namespace oscillate::application
