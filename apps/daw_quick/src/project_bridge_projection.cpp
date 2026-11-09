#include "project_bridge_projection.hpp"
#include "oscillate/application/midi_pedal_edit.hpp"
#include "project_bridge.hpp"
#include "tempo_lane_controller.hpp"

#include "arrangement_automation_controller.hpp"
#include "clip_tempo_controller.hpp"
#include "clip_warp_controller.hpp"
#include "harmony_controller.hpp"
#include "insert_chain_view.hpp"
#include "key_match_controller.hpp"
#include "keyed_list_model.hpp"
#include "loop_transport.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "oscillate/application/browser_preview.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/harmony/clip.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "plugin_automation_controller.hpp"
#include "saved_processing.hpp"
#include "track_routing_view.hpp"
#include "waveform_controller.hpp"

#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::quick::detail {
namespace {
// "#rrggbb", or empty when the app's palette decides.
[[nodiscard]] QString color_text(const std::optional<std::uint32_t>& color) {
  return color ? QStringLiteral("#%1").arg(*color, 6, 16, QLatin1Char('0')) : QString();
}

// A group bus's members' clips merged into spans of zero-based bars
// ({startBar, endBar}): a folded bus's lane outlines them.
void add_member_spans(QVariantList& tracks, double timeline_bars) {
  for (auto& entry : tracks) {
    auto bus = entry.toMap();
    if (bus.value(QStringLiteral("kind")).toString() != QStringLiteral("group"))
      continue;
    const auto id = bus.value(QStringLiteral("id")).toString();
    std::vector<std::pair<double, double>> spans;
    for (const auto& other : tracks) {
      const auto member = other.toMap();
      if (member.value(QStringLiteral("busId")).toString() != id)
        continue;
      for (const auto& clip : member.value(QStringLiteral("clips")).toList()) {
        const auto map = clip.toMap();
        const double start = map.value(QStringLiteral("start")).toDouble();
        spans.emplace_back(start, start + map.value(QStringLiteral("length")).toDouble());
      }
    }
    std::ranges::sort(spans);
    std::vector<std::pair<double, double>> merged;
    for (const auto& span : spans) {
      if (!merged.empty() && span.first <= merged.back().second)
        merged.back().second = std::max(merged.back().second, span.second);
      else
        merged.push_back(span);
    }
    QVariantList projected;
    for (const auto& [start, end] : merged)
      projected.push_back(
          QVariantMap{{"startBar", start * timeline_bars}, {"endBar", end * timeline_bars}});
    bus.insert(QStringLiteral("memberSpans"), projected);
    entry = bus;
  }
}

// The saved automation lanes a track holds: its own and its clips'.
[[nodiscard]] int automation_lane_count(const project::Track& track) {
  std::size_t lanes = track.automation.size();
  for (const auto& clip : track.clips)
    lanes += clip.automation.size();
  for (const auto& clip : track.midi_clips)
    lanes += clip.automation.size();
  return static_cast<int>(lanes);
}
} // namespace

// The sound a sampling engine plays, named by its first region (a starter
// sound's single chop, say); empty when the source has none.
static QString starter_sound_name(const project::Track& track) {
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  if (!patch || patch->document.regions.empty())
    return {};
  return QString::fromStdString(patch->document.regions.front().name);
}

static double bar_position(const project::Snapshot& snapshot, timeline::TickPosition tick) {
  const auto located = snapshot.meter.locate(tick);
  if (!located || located.value().bar < 1) {
    return 0.0;
  }
  const auto bar = located.value().bar;
  const auto begin = snapshot.meter.tick_at({bar, 1, 0});
  const auto next = snapshot.meter.tick_at({bar + 1, 1, 0});
  if (!begin || !next || next.value().value <= begin.value().value) {
    return static_cast<double>(bar - 1);
  }
  const auto fraction = static_cast<double>(tick.value - begin.value().value) /
                        static_cast<double>(next.value().value - begin.value().value);
  return static_cast<double>(bar - 1) + fraction;
}

static QVariantMap clip_projection(const project::Snapshot& snapshot, project::ClipId id,
                                   const project::Placement& placement, QString kind,
                                   int timeline_bars) {
  timeline::TickPosition begin{0};
  timeline::TickPosition end{0};
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    begin = musical->start;
    end = musical->end;
  } else if (const auto* elapsed = std::get_if<project::ElapsedInterval>(&placement)) {
    const auto start_tick = snapshot.tempo.sample_to_tick(elapsed->start, snapshot.sample_rate);
    const auto end_tick = snapshot.tempo.sample_to_tick(elapsed->end, snapshot.sample_rate);
    if (start_tick && end_tick) {
      begin = start_tick.value();
      end = end_tick.value();
    }
  }
  const double start_bar = bar_position(snapshot, begin);
  const double end_bar = bar_position(snapshot, end);
  return {
      {"id", QString::fromStdString(id.to_string())},
      {"kind", std::move(kind)},
      {"timebase", std::holds_alternative<project::MusicalInterval>(placement)
                       ? QStringLiteral("Musical")
                       : QStringLiteral("Elapsed")},
      {"start", std::clamp(start_bar / timeline_bars, 0.0, 1.0)},
      {"length", std::clamp((end_bar - start_bar) / timeline_bars, 0.0, 1.0)},
      // The clip's span in ticks from its start, as MIDI patterns count.
      {"lengthTicks", static_cast<qlonglong>(std::max<std::int64_t>(0, end.value - begin.value))}};
}

static int arrangement_bar_extent(const project::Snapshot& snapshot) {
  double last_bar = 0.0;
  const auto visit = [&](const project::Placement& placement) {
    timeline::TickPosition end{0};
    if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
      end = musical->end;
    } else if (const auto* elapsed = std::get_if<project::ElapsedInterval>(&placement)) {
      if (const auto tick = snapshot.tempo.sample_to_tick(elapsed->end, snapshot.sample_rate))
        end = tick.value();
    }
    last_bar = std::max(last_bar, bar_position(snapshot, end));
  };
  for (const auto& track : snapshot.tracks) {
    for (const auto& clip : track.clips)
      visit(clip.placement);
    for (const auto& clip : track.midi_clips)
      visit(clip.placement);
  }
  if (last_bar <= visible_bars)
    return visible_bars;
  constexpr int maximum_bars = 1'000'000;
  // Room to drag clips well past the end (a DJ set grows to the right):
  // 64 bars or a quarter of the project, whichever is more.
  const auto padded =
      std::min(last_bar + std::max(64.0, last_bar / 4.0), static_cast<double>(maximum_bars));
  return static_cast<int>(std::ceil(padded / visible_bars)) * visible_bars;
}

} // namespace oscillate::quick::detail

namespace oscillate::quick {
using detail::arrangement_bar_extent;
using detail::channel_automation_projection;
using detail::clip_channel_automation_projection;
using detail::clip_projection;
using detail::send_automation_projection;
using detail::starter_sound_name;

void ProjectBridge::refresh() {
  OSCILLATE_TRACE_SPAN("ui.refresh_projection");
  const auto& snapshot = session_->snapshot();
  // Bars added before bar 1 (or undone) move the loop with the music.
  if (const auto lead = static_cast<int>(snapshot.lead_bars); lead != shown_lead_bars_) {
    const auto delta = lead - shown_lead_bars_;
    shown_lead_bars_ = lead;
    if (loop_transport_ && loop_transport_->start_bar() + delta >= 1)
      loop_transport_->set_range(loop_transport_->start_bar() + delta,
                                 loop_transport_->end_bar() + delta);
  }
  if (native_synth_adapter_) {
    const auto found =
        std::ranges::find(snapshot.tracks, native_synth_adapter_->track(), &project::Track::id);
    const auto* patch =
        found == snapshot.tracks.end()
            ? nullptr
            : std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
    if (!patch || patch->engine != native_synth_adapter_->engine().descriptor->id)
      close_native_synth_editor();
  }
  if (native_effect_adapter_) {
    // The open effect's slot was removed (or undone away): close its panel.
    const auto& path = native_effect_adapter_->path();
    const std::vector<project::EffectInsert>* effects = &snapshot.master_effects;
    if (const auto* track = std::get_if<project::TrackId>(&path)) {
      const auto owner = std::ranges::find(snapshot.tracks, *track, &project::Track::id);
      effects = owner == snapshot.tracks.end() ? nullptr : &owner->effects;
    }
    const auto slot = *native_effect_adapter_->effect_slot();
    if (!effects || std::ranges::none_of(*effects, [&](const project::EffectInsert& effect) {
          const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor);
          return effect.id == slot && patch &&
                 patch->engine == native_effect_adapter_->descriptor().id;
        }))
      close_native_effect_editor();
  }
  refresh_native_effect_context();
  timeline_bars_ = arrangement_bar_extent(snapshot);
  exports_->follow_timeline_bars(timeline_bars_);
  key_review_->refresh_snapshot();
  waveform_->refresh_sources(snapshot);
  revision_ = snapshot.revision;
  summary_.refresh(snapshot);
  QVariantList tracks;
  const TrackRoutingView routing_view(snapshot);
  route_targets_ = routing_view.targets();
  route_target_count_ = routing_view.target_count();
  routing_active_ = routing_view.active();
  std::size_t projected_audio_clips = 0;
  for (const auto& track : snapshot.tracks) {
    QVariantList clips;
    for (const auto& clip : track.clips) {
      auto projected = clip_projection(snapshot, clip.id, clip.placement, QStringLiteral("audio"),
                                       timeline_bars_);
      projected.insert(QStringLiteral("channelAutomation"),
                       clip_channel_automation_projection(snapshot, track, clip));
      // A badge for clips that render through the warp engine.
      const auto warp_request = processing::processed_clip_request(snapshot, clip.id);
      const bool processed = !warp_request || warp_request.value().has_value();
      projected.insert(QStringLiteral("processed"), processed);
      projected.insert(
          QStringLiteral("warpBadge"),
          !warp_request ? QStringLiteral("WARP ERROR")
          : processed ? QStringLiteral("WARP · %1").arg(warp_mode_label(clip.processing.warp.mode))
                      : QString{});
      projected.insert(QStringLiteral("name"), QString::fromStdString(clip.name));
      projected.insert(QStringLiteral("sourceStartFrame"),
                       static_cast<qlonglong>(clip.source_range.start.value));
      projected.insert(
          QStringLiteral("sourceFrameCount"),
          static_cast<double>(clip.source_range.end.value - clip.source_range.start.value));
      projected.insert(QStringLiteral("fadeInFraction"), clip.fades.in_fraction);
      projected.insert(QStringLiteral("fadeOutFraction"), clip.fades.out_fraction);
      projected.insert(QStringLiteral("fadeInCurve"),
                       clip.fades.in_curve == project::FadeCurve::smoothstep ? 1 : 0);
      projected.insert(QStringLiteral("fadeOutCurve"),
                       clip.fades.out_curve == project::FadeCurve::smoothstep ? 1 : 0);
      const double clip_start_bar =
          projected.value(QStringLiteral("start")).toDouble() * timeline_bars_;
      const auto waveform_fields = waveform_->project_clip_waveform(
          snapshot, clip,
          {.start_bar = clip_start_bar,
           .end_bar = clip_start_bar +
                      projected.value(QStringLiteral("length")).toDouble() * timeline_bars_,
           .processed = processed,
           .audio_clip_ordinal = ++projected_audio_clips,
           .columns = false});
      // The columns live in the clip model's waveform role, so zooming
      // re-projects only them.
      for (auto item = waveform_fields.cbegin(); item != waveform_fields.cend(); ++item)
        projected.insert(item.key(), item.value());
      projected.insert(QStringLiteral("color"), detail::color_text(clip.color));
      clips.push_back(projected);
    }
    for (const auto& clip : track.midi_clips) {
      auto projected = clip_projection(snapshot, clip.id, clip.placement, QStringLiteral("midi"),
                                       timeline_bars_);
      // Notes trimmed off the clip's left edge are kept but neither drawn
      // nor edited until the edge is dragged back over them.
      std::vector<project::MidiNote> shown;
      std::ranges::copy_if(clip.notes, std::back_inserter(shown),
                           [](const project::MidiNote& note) { return note.start.value >= 0; });
      QVariantList notes;
      for (const auto& note : shown) {
        notes.push_back(QVariantMap{
            {"id", QString::fromStdString(note.id.to_string())},
            {"step", static_cast<int>(note.start.value / (timeline::ticks_per_quarter / 4))},
            {"startTicks", static_cast<qlonglong>(note.start.value)},
            {"durationTicks", static_cast<qlonglong>(note.end.value - note.start.value)},
            // Where it stops sounding under the pedals, for its ringing tail.
            {"soundingEndTicks", static_cast<qlonglong>(application::sounding_end(note, clip))},
            {"key", static_cast<int>(note.key)},
            {"velocity", static_cast<int>(note.velocity)}});
      }
      projected.insert(QStringLiteral("notes"), notes);
      // The chords the notes make (ADR-0084), named on the clip.
      QVariantList chords;
      const auto chord_key = snapshot.key.value_or(HarmonyController::fallback_key);
      for (const auto& region : harmony::recognise(shown, clip.pattern_length))
        chords.push_back(QVariantMap{
            {"startTicks", static_cast<qlonglong>(region.position.value)},
            {"endTicks", static_cast<qlonglong>(region.position.value + region.length_ticks)},
            {"name", QString::fromStdString(harmony::name(region.chord, chord_key))}});
      projected.insert(QStringLiteral("chords"), chords);
      projected.insert(QStringLiteral("patternTicks"),
                       static_cast<qlonglong>(clip.pattern_length.value));
      projected.insert(QStringLiteral("loop"), clip.loop);
      // The pedal strip's spans, per pedal (ADR-0035).
      QVariantMap pedals{
          {"sustain", QVariantList{}}, {"sostenuto", QVariantList{}}, {"soft", QVariantList{}}};
      for (const auto& lane : clip.pedals) {
        QVariantList spans;
        for (const auto& span : application::pedal_spans(lane.points, clip.pattern_length.value)) {
          spans.push_back(QVariantMap{{"startTicks", static_cast<qlonglong>(span.start)},
                                      {"endTicks", static_cast<qlonglong>(span.end)},
                                      {"depth", span.depth}});
        }
        const auto name = lane.pedal == project::MidiPedal::sustain ? QStringLiteral("sustain")
                          : lane.pedal == project::MidiPedal::sostenuto
                              ? QStringLiteral("sostenuto")
                              : QStringLiteral("soft");
        pedals.insert(name, spans);
      }
      projected.insert(QStringLiteral("pedals"), pedals);
      projected.insert(QStringLiteral("color"), detail::color_text(clip.color));
      clips.push_back(projected);
    }
    const auto instrument = track.kind == project::TrackKind::instrument;
    QVariantList sends;
    for (const auto& send : track.sends) {
      const auto target_name =
          send.target_insert
              ? routing_view.sidechain_target_name(send.destination, *send.target_insert)
              : QString{};
      sends.push_back(QVariantMap{
          {"id", QString::fromStdString(send.id.to_string())},
          {"destinationId", QString::fromStdString(send.destination.to_string())},
          {"destinationName", routing_view.send_destination_name(send.destination)},
          {"purpose", send.purpose == project::SendPurpose::sidechain ? QStringLiteral("sidechain")
                                                                      : QStringLiteral("audible")},
          {"targetSlotId", send.target_insert
                               ? QString::fromStdString(send.target_insert->to_string())
                               : QString()},
          {"targetName", target_name},
          {"gain", send.gain},
          {"automation", send_automation_projection(snapshot, track, send.id)}});
    }
    const auto* native_patch =
        std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    const auto* native_engine =
        native_patch ? native_dsp::synth::find_engine(native_patch->engine) : nullptr;
    QVariantList vst3_plugins;
    for (const auto* plugin : detail::saved_vst3_plugins(track)) {
      vst3_plugins.push_back(QVariantMap{{"id", QString::fromStdString(plugin->id.to_string())},
                                         {"name", QString::fromStdString(plugin->name)},
                                         {"vendor", QString::fromStdString(plugin->vendor)},
                                         {"missing", plugin->missing},
                                         {"bundle", QString::fromStdString(plugin->bundle_path)}});
    }
    const auto sound_name = instrument ? starter_sound_name(track) : QString();
    const auto kick_lock = application::kick_bass_lock_state(snapshot, track.id);
    const auto& nesting = routing_view.nesting(track.id);
    tracks.push_back(QVariantMap{
        {"id", QString::fromStdString(track.id.to_string())},
        {"name",
         track.name.empty() ? routing_view.name(track.id) : QString::fromStdString(track.name)},
        {"customName", QString::fromStdString(track.name)},
        // "#rrggbb", or empty for the app's palette.
        {"color", detail::color_text(track.color)},
        {"kind", routing_view.kind(track.id)},
        // Clips, and so the dock's Clip and Notes tabs (#331).
        {"holdsClips", project::track_holds_clips(track.kind)},
        {"muted", track.muted},
        {"soloed", track.soloed},
        {"level", track.level},
        {"pan", track.pan},
        {"mainDestinationId",
         track.main_output ? QString::fromStdString(track.main_output->to_string()) : QString()},
        {"mainDestinationName", routing_view.main_destination_name(track.main_output)},
        // The track list's nesting (TrackRoutingView::Nesting): the row it
        // shows in and the group bus it sits under.
        {"displayIndex", nesting.display_index},
        {"busId", nesting.bus ? QString::fromStdString(nesting.bus->to_string()) : QString()},
        {"memberCount", nesting.member_count},
        {"lastMember", nesting.last_member},
        {"fedBy", nesting.fed_by},
        {"sends", sends},
        {"sendCount", static_cast<int>(track.sends.size())},
        {"nativeSourceReady",
         std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source)},
        {"nativeEngineId", native_patch ? QString::fromStdString(native_patch->engine) : QString()},
        {"nativeEngineName",
         native_engine  ? QString::fromStdString(std::string(native_engine->descriptor->name))
         : native_patch ? QString::fromStdString(native_patch->engine)
                        : QString()},
        {"vst3SourceReady", detail::vst3_instrument_source(track) != nullptr},
        {"soundName", sound_name},
        // Smart duck (ADR-0059): a kick that keys nothing yet is offered
        // "Duck everything to this".
        {"kickLike", application::looks_like_kick(track, sound_name.toStdString())},
        {"keysSidechain", application::keys_a_sidechain(track)},
        // Kick & Bass Lock (ADR-0072): a Nova track offers it while the
        // project has an Anvil track.
        {"kickLockOffered", kick_lock.offered},
        {"kickLocked", kick_lock.locked},
        // Freeze (ADR-0059): the track plays its render; devices are greyed.
        {"frozen", track.frozen.has_value()},
        {"vst3Plugins", vst3_plugins},
        {"channelAutomation", channel_automation_projection(snapshot, track)},
        // Every saved lane the track holds, its clips' included: the track
        // list marks a track with automation before its lanes are shown.
        {"automationLaneCount", detail::automation_lane_count(track)},
        {"clipCount", static_cast<int>(clips.size())},
        {"clips", clips}});
  }
  detail::add_member_spans(tracks, static_cast<double>(timeline_bars_));
  insert_chains_.clear();
  const auto projected_chains = project::project_insert_chains(snapshot);
  if (projected_chains && projected_chains.value().paths.size() == snapshot.tracks.size() + 1) {
    for (std::size_t index = 0; index < snapshot.tracks.size(); ++index) {
      insert_chains_.push_back(insert_chain_view(projected_chains.value().paths[index],
                                                 snapshot.tracks[index].effects,
                                                 &snapshot.tracks[index]));
    }
    insert_chains_.push_back(
        insert_chain_view(projected_chains.value().paths.back(), snapshot.master_effects, nullptr));
  } else {
    // The project remains open so the user can recover it. Never show a
    // fabricated empty chain when validation or projection failed.
    const auto error = projected_chains
                           ? QStringLiteral("Insert path count does not match saved tracks")
                           : QString::fromStdString(projected_chains.error().message);
    for (std::size_t index = 0; index <= snapshot.tracks.size(); ++index) {
      insert_chains_.push_back(
          QVariantMap{{"effects", QVariantList{}},
                      {"sourceName", QString()},
                      {"sourceStatus", QString()},
                      {"status", QStringLiteral("Saved chain unavailable: %1").arg(error)}});
    }
  }
  const bool tracks_changed = tracks != tracks_;
  if (tracks_changed) {
    tracks_ = std::move(tracks);
    publish_track_models(tracks_);
  }
  refresh_clip_waveforms();
  publish_project_timing();
  tempo_lane_->refresh();
  plugin_automation_->refresh();
  arrangement_automation_->refresh();
  clip_warp_->refresh_snapshot();
  clip_tempo_->refresh();
  if (key_match_)
    key_match_->refresh();
  if (native_synth_controller_)
    native_synth_controller_->refresh();
  if (native_synth_automation_)
    native_synth_automation_->refresh();
  if (native_effect_controller_)
    native_effect_controller_->refresh();
  if (native_effect_automation_)
    native_effect_automation_->refresh();
  // Each projection signals only when it changed, so an edit to one track
  // rebinds only what reads that track.
  Published now{.open = project_open(),
                .name = project_name(),
                .format = project_format(),
                .revision = revision_,
                .markers = markers(),
                .key = project_key(),
                .suggested_key = suggested_project_key(),
                .timeline_bars = timeline_bars_,
                .lead_bars = lead_bars(),
                .insert_chains = insert_chains_,
                .route_targets = route_targets_,
                .route_target_count = route_target_count_,
                .routing_active = routing_active_,
                .master_level = master_level()};
  auto before = std::exchange(published_, now);
  if (before.open != now.open || before.name != now.name || before.format != now.format)
    emit projectChanged();
  if (before.revision != now.revision)
    emit revisionChanged();
  if (tracks_changed)
    emit tracksChanged();
  if (before.markers != now.markers)
    emit markersChanged();
  if (before.key != now.key || before.suggested_key != now.suggested_key)
    emit projectKeyChanged();
  if (before.timeline_bars != now.timeline_bars || before.lead_bars != now.lead_bars)
    emit timelineChanged();
  if (before.insert_chains != now.insert_chains)
    emit insertChainsChanged();
  if (before.route_targets != now.route_targets ||
      before.route_target_count != now.route_target_count ||
      before.routing_active != now.routing_active)
    emit routingChanged();
  if (before.master_level != now.master_level)
    emit masterLevelChanged();
}

QObject* ProjectBridge::track_model() const noexcept { return track_model_; }

QVariantList ProjectBridge::clip_waveform(const QString& clip_id) const {
  for (const auto* clips : clip_models_)
    if (clips->indexOf(clip_id) >= 0)
      return clips->extra(clip_id, 0).toList();
  return {};
}

void ProjectBridge::publish_track_models(const QVariantList& tracks) {
  std::vector<KeyedListModel::Row> rows;
  rows.reserve(static_cast<std::size_t>(tracks.size()));
  QSet<QString> kept;
  for (const auto& item : tracks) {
    auto fields = item.toMap();
    const auto id = fields.value(QStringLiteral("id")).toString();
    auto*& clips = clip_models_[id];
    if (!clips)
      clips = new KeyedListModel({QStringLiteral("waveform"), QStringLiteral("tempo")}, this);
    std::vector<KeyedListModel::Row> clip_rows;
    for (const auto& clip : fields.value(QStringLiteral("clips")).toList()) {
      auto clip_fields = clip.toMap();
      auto clip_id = clip_fields.value(QStringLiteral("id")).toString();
      clip_rows.push_back({std::move(clip_id), std::move(clip_fields), {}});
    }
    clips->update(std::move(clip_rows));
    // A clip edit signals its clip's row, never the track's.
    fields.remove(QStringLiteral("clips"));
    rows.push_back({id, std::move(fields), {QVariant::fromValue<QObject*>(clips)}});
    kept.insert(id);
  }
  track_model_->update(std::move(rows));
  for (auto item = clip_models_.begin(); item != clip_models_.end();) {
    if (kept.contains(item.key())) {
      ++item;
      continue;
    }
    item.value()->deleteLater();
    item = clip_models_.erase(item);
  }
  publish_clip_tempo();
}

void ProjectBridge::publish_clip_tempo() {
  const auto tempo = clip_tempo_->clips();
  for (auto* clips : std::as_const(clip_models_))
    for (const auto& row : clips->rows())
      clips->set_extra(row.id, 1, tempo.value(row.id));
}

void ProjectBridge::refresh_clip_waveforms() {
  if (session_) {
    const auto& snapshot = session_->snapshot();
    std::size_t ordinal = 0;
    for (const auto& track : snapshot.tracks) {
      for (const auto& clip : track.clips) {
        const auto projected = clip_projection(snapshot, clip.id, clip.placement,
                                               QStringLiteral("audio"), timeline_bars_);
        const auto request = processing::processed_clip_request(snapshot, clip.id);
        const double start = projected.value(QStringLiteral("start")).toDouble() * timeline_bars_;
        const auto fields = waveform_->project_clip_waveform(
            snapshot, clip,
            {.start_bar = start,
             .end_bar =
                 start + projected.value(QStringLiteral("length")).toDouble() * timeline_bars_,
             .processed = !request || request.value().has_value(),
             .audio_clip_ordinal = ++ordinal});
        if (auto* clips = clip_models_.value(QString::fromStdString(track.id.to_string())))
          clips->set_extra(QString::fromStdString(clip.id.to_string()), 0,
                           fields.value(QStringLiteral("waveform")));
      }
    }
  }
}

void ProjectBridge::publish_project_timing() {
  const auto& snapshot = session_->snapshot();
  project_timing_ = QVariantMap{
      {"projectBpm", snapshot.tempo.events().empty() ? 0.0 : snapshot.tempo.events().front().bpm},
      {"projectMeter", snapshot.meter.events().empty()
                           ? QString()
                           : QStringLiteral("%1/%2")
                                 .arg(snapshot.meter.events().front().numerator)
                                 .arg(snapshot.meter.events().front().denominator)}};
  emit projectTimingChanged();
}

} // namespace oscillate::quick
