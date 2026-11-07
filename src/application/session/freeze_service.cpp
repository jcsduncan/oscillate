#include "oscillate/application/session/freeze_service.hpp"

#include "oscillate/export/offline_master.hpp"

#include "edit_support.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using project::find_track;
template <typename T> using Result = foundation::Result<T>;

void refresh(project::AutomationLane& lane, FreshIdentities& ids) {
  lane.id = project::AutomationLaneId::from_bits({ids.space, ids.next_lane++}).value();
  for (auto& point : lane.points)
    point.id = project::AutomationPointId::from_bits({ids.space, ids.next_point++}).value();
}

// A private file for the render, unlinked once open: nothing is left behind
// whether the render publishes, fails or is cancelled.
class RenderDirectory {
public:
  RenderDirectory() = default;
  RenderDirectory(const RenderDirectory&) = delete;
  RenderDirectory& operator=(const RenderDirectory&) = delete;
  ~RenderDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  [[nodiscard]] Result<std::filesystem::path> create() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error)
      return Result<std::filesystem::path>::failure(
          {ErrorCode::io_failure, "No temporary directory for the render"});
    auto pattern = (base / "oscillate-render-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr)
      return Result<std::filesystem::path>::failure(
          {ErrorCode::io_failure, "Cannot create a private render directory"});
    path_ = pattern;
    return Result<std::filesystem::path>::success(path_ / "render.wav");
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string bounced_name(const project::Track& track) {
  return track.name.empty() ? std::string{"Bounce"} : track.name + " · Bounce";
}

} // namespace

TrackRender::TrackRender(TrackRender&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), first_frame_(other.first_frame_),
      origin_revision_(other.origin_revision_), track_(other.track_) {}

TrackRender& TrackRender::operator=(TrackRender&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0)
      (void)::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    first_frame_ = other.first_frame_;
    origin_revision_ = other.origin_revision_;
    track_ = other.track_;
  }
  return *this;
}

TrackRender::~TrackRender() {
  if (fd_ >= 0)
    (void)::close(fd_);
}

Result<TrackRender>
TrackRenderTask::run(std::function<bool(std::uint64_t, std::uint64_t)> progress) && {
  RenderDirectory directory;
  auto target = directory.create();
  if (!target)
    return Result<TrackRender>::failure(target.error());
  auto rendered = exporting::render_track(*snapshot_, reader_,
                                          {target.value(), track_, range_, std::move(progress)});
  if (!rendered)
    return Result<TrackRender>::failure(rendered.error());
  const int fd = ::open(target.value().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return Result<TrackRender>::failure({ErrorCode::io_failure, "Cannot open the track render"});
  return Result<TrackRender>::success(TrackRender{
      fd,
      {.first_frame = rendered.value().first_frame, .origin_revision = snapshot_->revision},
      track_});
}

Result<TrackRenderTask>
FreezeService::begin_track_render(project::TrackId track,
                                  std::optional<std::pair<std::uint64_t, std::uint64_t>> range) {
  using R = Result<TrackRenderTask>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  if (!state_.pending_receipts.empty())
    return R::failure({ErrorCode::conflict, "Save the pending managed source before rendering"});
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return R::failure({ErrorCode::not_found, "Track to render not found"});
  if (owner->kind != project::TrackKind::audio && owner->kind != project::TrackKind::instrument)
    return R::failure({ErrorCode::unsupported, "Only audio and instrument tracks render"});
  if (range && range->second <= range->first)
    return R::failure({ErrorCode::invalid_argument, "Select a time range to bounce"});
  auto reader = persistence::ProjectStore::open_reader(state_.root);
  if (!reader)
    return R::failure(reader.error());
  return R::success(TrackRenderTask{model().snapshot(), std::move(reader).value(), track, range});
}

Result<TrackFreezeStage> FreezeService::begin_track_render_stage(const TrackRender& render) {
  using R = Result<TrackFreezeStage>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  auto source_id = numbered_id<project::SourceId>(next_source_number());
  if (!source_id)
    return R::failure(source_id.error());
  auto stage = state_.store.begin_wav_stage(source_id.value(), render.fd());
  if (!stage)
    return R::failure(stage.error());
  return R::success({std::move(stage).value(), render.origin_revision(), render.track()});
}

Result<project::TrackId> FreezeService::finish_track_bounce(persistence::StagedWav&& render,
                                                            std::uint64_t origin_revision,
                                                            project::TrackId track,
                                                            std::uint64_t first_frame,
                                                            BounceMode mode) {
  using R = Result<project::TrackId>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  const auto& state = snapshot();
  if (origin_revision > state.revision || state.audible_revision > origin_revision ||
      render.record().id.bits().low != next_source_number())
    return R::failure({ErrorCode::conflict, "The project changed while bouncing; bounce again"});
  const auto& record = render.record();
  if (record.sample_rate != state.sample_rate || record.channels != 2 || record.frames == 0 ||
      record.frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      first_frame >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - record.frames)
    return R::failure({ErrorCode::invalid_argument, "A bounce is stereo at the project rate"});
  const auto found = std::ranges::find(state.tracks, track, &project::Track::id);
  if (found == state.tracks.end())
    return R::failure({ErrorCode::not_found, "Track to bounce not found"});
  const auto& original = *found;
  const auto row = static_cast<std::size_t>(found - state.tracks.begin());
  auto track_id = numbered_id<project::TrackId>(next_track_number());
  auto clip_id = numbered_id<project::ClipId>(next_clip_number());
  auto ids = fresh_identities(state.revision);
  if (!track_id || !clip_id || !ids)
    return R::failure(!track_id ? track_id.error() : !clip_id ? clip_id.error() : ids.error());
  const auto audio = track_id.value();

  const auto existing = std::ranges::find_if(state.sources, [&record](const auto& source) {
    return source.sha256 == record.sha256 && source.byte_length == record.byte_length &&
           source.encoding == record.encoding && source.sample_rate == record.sample_rate &&
           source.channels == record.channels && source.frames == record.frames &&
           !source.external_link;
  });
  const bool is_new_source = existing == state.sources.end();
  std::vector<project::Operation> operations;
  if (is_new_source) {
    state_.pending_receipts.reserve(state_.pending_receipts.size() + 1);
    operations.emplace_back(project::AddSource{record});
  }
  const bool flatten = mode == BounceMode::flatten;
  operations.emplace_back(
      project::AddTrack{audio, project::TrackKind::audio, flatten ? row : row + 1});
  const auto first = static_cast<std::int64_t>(first_frame);
  const auto frames = static_cast<std::int64_t>(record.frames);
  project::Clip clip{clip_id.value(),
                     is_new_source ? record.id : existing->id,
                     {{0}, {frames}},
                     project::ElapsedInterval{{first}, {first + frames}}};
  clip.name = flatten ? original.name : bounced_name(original);
  operations.emplace_back(project::AddClip{audio, std::move(clip)});

  // The audio sounds as the source did: its channel, routing and sends.
  const auto name = flatten ? original.name : bounced_name(original);
  if (!name.empty())
    operations.emplace_back(project::SetTrackName{audio, name});
  if (original.color)
    operations.emplace_back(project::SetTrackColor{audio, original.color});
  if (original.level != 1.0 || original.pan != 0.0)
    operations.emplace_back(project::SetTrackLevelPan{audio, original.level, original.pan});
  if (original.main_output)
    operations.emplace_back(project::SetTrackMainOutput{audio, original.main_output});
  std::map<project::SendId, project::SendId> sends;
  auto next_send = next_send_number();
  for (const auto& send : original.sends) {
    // A bounce keeps its muted source, which still keys (keys are taken
    // before the fader): only Flatten moves the keys.
    if (!flatten && send.purpose == project::SendPurpose::sidechain)
      continue;
    auto send_id = numbered_id<project::SendId>(next_send);
    if (!send_id)
      return R::failure(send_id.error());
    next_send = next_send == std::numeric_limits<std::uint64_t>::max() ? 0 : next_send + 1;
    sends.emplace(send.id, send_id.value());
    auto route = send;
    route.id = send_id.value();
    operations.emplace_back(project::AddSendRoute{audio, route});
  }

  // Channel and send lanes follow the audio; device lanes stay with the
  // devices.
  for (const auto& lane : original.automation) {
    auto copy = lane;
    if (auto* control = std::get_if<project::TrackControlTarget>(&copy.target)) {
      if (!project::is_channel_control(control->key))
        continue;
      control->track = audio;
    } else if (auto* gain = std::get_if<project::SendGainTarget>(&copy.target)) {
      const auto renamed = sends.find(gain->send);
      if (renamed == sends.end())
        continue;
      gain->send = renamed->second;
    } else {
      continue;
    }
    refresh(copy, ids.value());
    operations.emplace_back(project::AddAutomationLane{audio, std::move(copy)});
  }
  if (!flatten) {
    // The source stays, muted, to unmute or freeze later.
    operations.emplace_back(project::SetTrackMuted{track, true});
  } else {
    if (original.muted)
      operations.emplace_back(project::SetTrackMuted{audio, true});
    if (original.soloed)
      operations.emplace_back(project::SetTrackSoloed{audio, true});
    // Keys into the source's inserts go with them.
    for (const auto& other : state.tracks) {
      for (const auto& send : other.sends) {
        if (send.destination != track)
          continue;
        for (const auto& lane : other.automation) {
          const auto* gain = std::get_if<project::SendGainTarget>(&lane.target);
          if (gain != nullptr && gain->send == send.id)
            operations.emplace_back(project::RemoveAutomationLane{lane.id});
        }
        operations.emplace_back(project::RemoveSendRoute{send.id});
      }
    }
    operations.emplace_back(project::RemoveTrack{track});
  }

  auto candidate = project::ProjectModel::open(state, *state_.gate);
  if (!candidate)
    return R::failure(candidate.error());
  if (auto validated = candidate.value().apply(operations); !validated)
    return R::failure(validated.error());
  auto published = state_.store.publish_wav_stage(std::move(render));
  if (!published)
    return R::failure(published.error());
  auto applied = model().apply(operations);
  if (!applied)
    return R::failure(applied.error());
  if (is_new_source)
    state_.pending_receipts.push_back(std::move(published).value());
  auto saved = saving_.save();
  if (!saved)
    return R::failure(
        {saved.error().code,
         "Track bounced in the current project, but save failed: " + saved.error().message});
  return R::success(audio);
}

Result<TrackFreezeStage> FreezeService::begin_track_freeze(project::TrackId track, int render_fd) {
  using R = Result<TrackFreezeStage>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return R::failure({ErrorCode::not_found, "Track to freeze not found"});
  if (owner->kind != project::TrackKind::audio && owner->kind != project::TrackKind::instrument)
    return R::failure({ErrorCode::unsupported, "Only audio and instrument tracks freeze"});
  if (owner->frozen)
    return R::failure({ErrorCode::conflict, "The track is already frozen"});
  auto source_id = numbered_id<project::SourceId>(next_source_number());
  if (!source_id)
    return R::failure(source_id.error());
  auto stage = state_.store.begin_wav_stage(source_id.value(), render_fd);
  if (!stage)
    return R::failure(stage.error());
  return R::success({std::move(stage).value(), snapshot().revision, track});
}

Result<void> FreezeService::finish_track_freeze(persistence::StagedWav&& render,
                                                std::uint64_t origin_revision,
                                                project::TrackId track, std::uint64_t first_frame) {
  if (state_.repair_state)
    return Result<void>::failure(repair_in_progress());
  const auto& state = snapshot();
  // Names, colours and markers may change while the render runs; anything
  // audible makes it stale.
  if (origin_revision > state.revision || state.audible_revision > origin_revision ||
      render.record().id.bits().low != next_source_number())
    return Result<void>::failure(
        {ErrorCode::conflict, "The project changed while freezing; freeze again"});
  const auto& record = render.record();
  if (record.sample_rate != state.sample_rate || record.channels != 2 || record.frames == 0)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "A freeze render is stereo at the project rate"});
  const auto existing = std::ranges::find_if(state.sources, [&record](const auto& source) {
    return source.sha256 == record.sha256 && source.byte_length == record.byte_length &&
           source.encoding == record.encoding && source.sample_rate == record.sample_rate &&
           source.channels == record.channels && source.frames == record.frames &&
           !source.external_link;
  });
  const bool is_new_source = existing == state.sources.end();
  const auto& source = is_new_source ? record : *existing;
  const project::FrozenTrack frozen{{source.id, source.sha256, source.byte_length,
                                     source.sample_rate, source.channels, source.frames},
                                    first_frame,
                                    origin_revision};
  std::vector<project::Operation> operations;
  if (is_new_source) {
    state_.pending_receipts.reserve(state_.pending_receipts.size() + 1);
    operations.emplace_back(project::AddSource{record});
  }
  operations.emplace_back(project::SetTrackFrozen{track, frozen});
  // Preflight the complete command before making the managed asset visible.
  auto candidate = project::ProjectModel::open(state, *state_.gate);
  if (!candidate)
    return Result<void>::failure(candidate.error());
  if (auto validated = candidate.value().apply(operations); !validated)
    return Result<void>::failure(validated.error());
  auto published = state_.store.publish_wav_stage(std::move(render));
  if (!published)
    return Result<void>::failure(published.error());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  if (is_new_source)
    state_.pending_receipts.push_back(std::move(published).value());
  auto saved = saving_.save();
  if (!saved)
    return Result<void>::failure(
        {saved.error().code,
         "Track frozen in the current project, but save failed: " + saved.error().message});
  return Result<void>::success();
}

Result<void> FreezeService::unfreeze_track(project::TrackId track) {
  return done(model().apply({project::SetTrackFrozen{track, std::nullopt}}));
}

} // namespace oscillate::application
