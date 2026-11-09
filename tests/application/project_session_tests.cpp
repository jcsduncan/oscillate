#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/project/clip_fade.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/fake_output.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wait.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

using oscillate::test::require;
using oscillate::test::take;

const std::string& external_path(const project::SourceRecord& source) {
  if (!source.external_link)
    throw std::runtime_error("expected an externally linked source");
  return source.external_link->last_known_path;
}

constexpr auto channel_level = project::TrackControlTarget::Key::channel_level;

const project::NativeInstrumentPatch* native_patch(const project::Track& track) {
  return std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-session") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

struct WavFrameCount {
  std::uint32_t value;
};

fs::path make_wav(const fs::path& root,
                  std::uint32_t sample_rate = oscillate::test::sample_rate_48k,
                  WavFrameCount frame_count = {4}) {
  const auto frames = frame_count.value;
  const auto path = root / "source.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, sample_rate);
  put32(bytes, sample_rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    put16(bytes, 8'192);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "fixture WAV write failed");
  return path;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "exported WAV reopen failed");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class FileDescriptor {
public:
  explicit FileDescriptor(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
    REQUIRE_GE(fd_, 0, "fixture WAV open failed");
  }
  ~FileDescriptor() { (void)::close(fd_); }
  [[nodiscard]] int get() const { return fd_; }

private:
  int fd_;
};

using oscillate::test::fake_output_factory;
using oscillate::test::FakePlaybackDevice;
using oscillate::test::FakePlaybackOutput;

void routing_commands_reopen_and_undo_atomically() {
  Directory directory;
  const auto root = directory.path() / "routed-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "routing fixture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "routing fixture setup failed");
  const auto source = session.value()->snapshot().tracks.front().id;
  auto group = session.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  auto returns = session.value()->tracks().add_bus_track(project::TrackKind::return_bus);
  REQUIRE_TRUE(group, "bus creation failed");
  REQUIRE_TRUE(returns, "bus creation failed");
  REQUIRE_NE(group.value(), returns.value(), "bus creation failed");
  require(!session.value()->tracks().add_bus_track(project::TrackKind::audio),
          "non-bus kind was accepted by bus API");
  REQUIRE_TRUE(session.value()->routing().set_main_output(source, group.value()),
               "main output validation changed");
  REQUIRE_TRUE(!session.value()->routing().set_main_output(group.value(), group.value()),
               "main output validation changed");
  auto send = session.value()->routing().add_send(source, returns.value(), 0.5);
  REQUIRE_TRUE(send, "send destination validation changed");
  REQUIRE_TRUE(!session.value()->routing().add_send(source, source, 0.5),
               "send destination validation changed");
  REQUIRE_TRUE(session.value()->routing().set_send_gain(send.value(), 0.25),
               "send gain edit failed");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().sends.front().gain, 0.25,
             "send gain edit failed");
  REQUIRE_TRUE(session.value()->routing().remove_send(send.value()), "send removal failed");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().sends.empty(), "send removal failed");
  REQUIRE_TRUE(session.value()->history().undo(),
               "send removal undo/redo did not restore exact route");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().sends.front().gain, 0.25,
             "send removal undo/redo did not restore exact route");
  REQUIRE_TRUE(session.value()->history().redo(),
               "send removal undo/redo did not restore exact route");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().sends.empty(),
               "send removal undo/redo did not restore exact route");
  REQUIRE_TRUE(session.value()->history().undo(),
               "send removal undo/redo did not restore exact route");
  REQUIRE_OK(session.value()->saving().save(), "routed project save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "routed project did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 3, "routed track count changed");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().main_output, group.value(),
             "routed main output changed");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().sends,
             (std::vector<project::SendRoute>{{send.value(), returns.value(), 0.25}}),
             "routed send changed");

  REQUIRE_TRUE(reopened.value()->routing().remove_send(send.value()), "send removal did not save");
  REQUIRE_TRUE(reopened.value()->saving().save(), "send removal did not save");
  reopened.value().reset();
  auto after_removal = application::ProjectSession::open(root);
  REQUIRE_OK(after_removal, "project did not reopen after send removal");
  auto replacement_send = after_removal.value()->routing().add_send(source, returns.value(), 0.5);
  REQUIRE_TRUE(replacement_send, "new send reused removed route identity after reopen");
  REQUIRE_NE(replacement_send.value(), send.value(),
             "new send reused removed route identity after reopen");
  auto removed_bus = after_removal.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_TRUE(removed_bus, "temporary bus undo did not save");
  REQUIRE_TRUE(after_removal.value()->history().undo(), "temporary bus undo did not save");
  REQUIRE_TRUE(after_removal.value()->saving().save(), "temporary bus undo did not save");
  after_removal.value().reset();
  auto after_undo = application::ProjectSession::open(root);
  REQUIRE_OK(after_undo, "project did not reopen after bus undo");
  auto replacement_bus = after_undo.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_TRUE(replacement_bus, "new bus reused undone track identity after reopen");
  REQUIRE_NE(replacement_bus.value(), removed_bus.value(),
             "new bus reused undone track identity after reopen");
}

void track_automation_points_are_stable_and_undoable() {
  Directory directory;
  const auto root = directory.path() / "automated-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "automation fixture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "automation fixture setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  const auto before_invalid = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->parameter_automation().set_channel_control_point(
                   track, channel_level, -1, 0.5),
               "invalid automation point changed the project");
  REQUIRE_TRUE(!session.value()->parameter_automation().set_channel_control_point(
                   track, channel_level, 0, 5.0),
               "invalid automation point changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before_invalid,
             "invalid automation point changed the project");
  auto first = session.value()->parameter_automation().set_channel_control_point(
      track, channel_level, 0, 0.5);
  auto second = session.value()->parameter_automation().set_channel_control_point(
      track, channel_level, 2, 1.5);
  REQUIRE_TRUE(first, "automation points were not created with stable identities");
  REQUIRE_TRUE(second, "automation points were not created with stable identities");
  REQUIRE_NE(first.value(), second.value(),
             "automation points were not created with stable identities");
  const auto& lane = session.value()->snapshot().tracks.front().automation.front();
  REQUIRE_EQ(lane.timebase, project::AutomationTimebase::musical,
             "bar placement or channel level descriptor changed");
  REQUIRE_EQ(lane.points.size(), 2, "bar placement or channel level descriptor changed");
  REQUIRE_EQ(lane.points[0].coordinate, 0, "bar placement or channel level descriptor changed");
  REQUIRE_EQ(lane.points[1].coordinate, 8 * timeline::ticks_per_quarter,
             "bar placement or channel level descriptor changed");
  REQUIRE_EQ(lane.domain.default_value, 1.0, "bar placement or channel level descriptor changed");
  const auto baseline_lane = lane;
  const auto before_toggle = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->automation().toggle_automation_lane_timebase(
                   project::AutomationLaneId::from_bits({0, 999}).value()),
               "automation timebase toggle changed a missing lane or rejected the real lane");
  REQUIRE_EQ(session.value()->snapshot().revision, before_toggle,
             "automation timebase toggle changed a missing lane or rejected the real lane");
  REQUIRE_TRUE(session.value()->automation().toggle_automation_lane_timebase(baseline_lane.id),
               "automation timebase toggle changed a missing lane or rejected the real lane");
  const auto elapsed_lane = session.value()->snapshot().tracks.front().automation.front();
  REQUIRE_EQ(elapsed_lane.timebase, project::AutomationTimebase::elapsed,
             "track automation points moved or lost identity during timebase conversion");
  REQUIRE_EQ(elapsed_lane.points[0].coordinate, 0,
             "track automation points moved or lost identity during timebase conversion");
  REQUIRE_EQ(elapsed_lane.points[1].coordinate, 192'000,
             "track automation points moved or lost identity during timebase conversion");
  REQUIRE_EQ(elapsed_lane.points[1].id, second.value(),
             "track automation points moved or lost identity during timebase conversion");
  REQUIRE_EQ(elapsed_lane.settings_generation, baseline_lane.settings_generation + 1,
             "track automation points moved or lost identity during timebase conversion");
  REQUIRE_TRUE(session.value()->history().undo(),
               "automation timebase conversion did not undo or round-trip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), baseline_lane,
             "automation timebase conversion did not undo or round-trip");
  REQUIRE_TRUE(session.value()->history().redo(),
               "automation timebase conversion did not undo or round-trip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), elapsed_lane,
             "automation timebase conversion did not undo or round-trip");
  REQUIRE_TRUE(session.value()->automation().toggle_automation_lane_timebase(baseline_lane.id),
               "automation timebase conversion did not undo or round-trip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points,
             baseline_lane.points, "automation timebase conversion did not undo or round-trip");
  const auto before_same = session.value()->snapshot().revision;
  auto same = session.value()->parameter_automation().set_channel_control_point(
      track, channel_level, 0, 0.5);
  REQUIRE_TRUE(same, "same-value automation edit created an undo step or changed point ID");
  REQUIRE_EQ(same.value(), first.value(),
             "same-value automation edit created an undo step or changed point ID");
  REQUIRE_EQ(session.value()->snapshot().revision, before_same,
             "same-value automation edit created an undo step or changed point ID");
  REQUIRE_TRUE(session.value()->parameter_automation().set_channel_level_automation_value(
                   track, first.value(), 0.75),
               "exact-ID value edit did not preserve point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].id,
             first.value(), "exact-ID value edit did not preserve point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].value, 0.75,
             "exact-ID value edit did not preserve point identity");
  REQUIRE_TRUE(session.value()->parameter_automation().remove_channel_control_point(
                   track, channel_level, second.value()),
               "point removal did not undo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.size(), 1,
             "point removal did not undo atomically");
  REQUIRE_TRUE(session.value()->history().undo(), "point removal did not undo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.size(), 2,
             "point removal did not undo atomically");
  REQUIRE_OK(session.value()->saving().save(), "automated project did not save");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "automation point value or identity changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[0].id,
             first.value(), "automation point value or identity changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[0].value, 0.75,
             "automation point value or identity changed after reopen");
  REQUIRE_TRUE(
      reopened.value()->parameter_automation().remove_channel_control_lane(track, channel_level),
      "lane removal did not undo atomically");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().automation.empty(),
               "lane removal did not undo atomically");
  REQUIRE_TRUE(reopened.value()->history().undo(), "lane removal did not undo atomically");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.size(), 1,
             "lane removal did not undo atomically");
  require(!reopened.value()->parameter_automation().remove_channel_control_point(
              track, channel_level, project::AutomationPointId::from_bits({0, 999}).value()),
          "missing automation point was removed");
  const auto original_lane = reopened.value()->snapshot().tracks.front().automation.front().id;
  REQUIRE_TRUE(
      reopened.value()->parameter_automation().remove_channel_control_lane(track, channel_level),
      "automation lane removal did not save");
  REQUIRE_TRUE(reopened.value()->saving().save(), "automation lane removal did not save");
  reopened.value().reset();
  auto after_removal = application::ProjectSession::open(root);
  REQUIRE_TRUE(after_removal, "removed automation lane reappeared after reopen");
  REQUIRE_TRUE(after_removal.value()->snapshot().tracks.front().automation.empty(),
               "removed automation lane reappeared after reopen");
  auto replacement = after_removal.value()->parameter_automation().set_channel_control_point(
      track, channel_level, 0, 0.25);
  REQUIRE_TRUE(replacement, "removed automation identities were reused after reopen");
  REQUIRE_NE(replacement.value(), first.value(),
             "removed automation identities were reused after reopen");
  REQUIRE_NE(after_removal.value()->snapshot().tracks.front().automation.front().id, original_lane,
             "removed automation identities were reused after reopen");
}

void send_automation_points_are_stable_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "send-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "send fixture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "send fixture setup failed");
  const auto source = session.value()->snapshot().tracks.front().id;
  const auto bus = session.value()->tracks().add_bus_track(project::TrackKind::return_bus);
  REQUIRE_OK(bus, "send fixture bus failed");
  const auto send = session.value()->routing().add_send(source, bus.value(), 0.5);
  REQUIRE_OK(send, "send fixture route failed");
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->parameter_automation().set_send_gain_automation_point(send.value(),
                                                                                       -1, 1.0),
               "invalid send automation mutated the project");
  REQUIRE_TRUE(
      !session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 0, 5.0),
      "invalid send automation mutated the project");
  REQUIRE_TRUE(!session.value()->parameter_automation().set_send_gain_automation_point(
                   project::SendId::from_bits({0, 999}).value(), 0, 1.0),
               "invalid send automation mutated the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid send automation mutated the project");
  const auto first =
      session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 0, 0.25);
  const auto second =
      session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 2, 0.75);
  REQUIRE_TRUE(first, "send point identity failed");
  REQUIRE_TRUE(second, "send point identity failed");
  REQUIRE_NE(first.value(), second.value(), "send point identity failed");
  const auto& lane = session.value()->snapshot().tracks.front().automation.front();
  REQUIRE_EQ(std::get<project::SendGainTarget>(lane.target).send, send.value(),
             "send lane ownership, descriptor or placement changed");
  REQUIRE_EQ(lane.timebase, project::AutomationTimebase::musical,
             "send lane ownership, descriptor or placement changed");
  REQUIRE_EQ(lane.domain.default_value, 0.5,
             "send lane ownership, descriptor or placement changed");
  REQUIRE_EQ(lane.points.size(), 2, "send lane ownership, descriptor or placement changed");
  REQUIRE_EQ(lane.points[1].coordinate, 8 * timeline::ticks_per_quarter,
             "send lane ownership, descriptor or placement changed");
  const auto unchanged = session.value()->snapshot().revision;
  const auto same =
      session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 0, 0.25);
  REQUIRE_TRUE(same, "identical send point created a revision");
  REQUIRE_EQ(same.value(), first.value(), "identical send point created a revision");
  REQUIRE_EQ(session.value()->snapshot().revision, unchanged,
             "identical send point created a revision");
  REQUIRE_TRUE(session.value()->parameter_automation().set_send_gain_automation_value(
                   send.value(), first.value(), 0.4),
               "send gain value edit or undo lost point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].id,
             first.value(), "send gain value edit or undo lost point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].value, 0.4,
             "send gain value edit or undo lost point identity");
  REQUIRE_TRUE(session.value()->history().undo(),
               "send gain value edit or undo lost point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].value, 0.25,
             "send gain value edit or undo lost point identity");
  REQUIRE_TRUE(session.value()->history().redo(),
               "send gain value edit or undo lost point identity");
  require(!session.value()->routing().remove_send(send.value()),
          "send removal silently discarded bound automation");
  REQUIRE_TRUE(session.value()->parameter_automation().remove_send_gain_automation_point(
                   send.value(), second.value()),
               "send point removal was not atomic and undoable");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.size(), 1,
             "send point removal was not atomic and undoable");
  REQUIRE_TRUE(session.value()->history().undo(), "send point removal was not atomic and undoable");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.size(), 2,
             "send point removal was not atomic and undoable");
  REQUIRE_OK(session.value()->saving().save(), "send automation save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "send automation did not reopen with stable point identity");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.size(), 1,
             "send automation did not reopen with stable point identity");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[0].id,
             first.value(), "send automation did not reopen with stable point identity");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[0].value, 0.4,
             "send automation did not reopen with stable point identity");
  REQUIRE_TRUE(
      reopened.value()->parameter_automation().remove_send_gain_automation_lane(send.value()),
      "send lane removal, undo/redo or route removal failed");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().automation.empty(),
               "send lane removal, undo/redo or route removal failed");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "send lane removal, undo/redo or route removal failed");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.size(), 1,
             "send lane removal, undo/redo or route removal failed");
  REQUIRE_TRUE(reopened.value()->history().redo(),
               "send lane removal, undo/redo or route removal failed");
  REQUIRE_TRUE(reopened.value()->routing().remove_send(send.value()),
               "send lane removal, undo/redo or route removal failed");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "send lane removal, undo/redo or route removal failed");
}

void channel_automation_edits_are_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "channel-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "channel fixture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "channel fixture setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->tracks().set_track_level_pan(track, 0.8, 0.25),
             "channel level setup failed");
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(
      !session.value()->parameter_automation().set_channel_level_automation_point(track, -1, 0.5),
      "invalid channel automation changed the project");
  REQUIRE_TRUE(
      !session.value()->parameter_automation().set_channel_level_automation_point(track, 0, 5.0),
      "invalid channel automation changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid channel automation changed the project");
  const auto first =
      session.value()->parameter_automation().set_channel_level_automation_point(track, 0, 0.5);
  const auto second =
      session.value()->parameter_automation().set_channel_level_automation_point(track, 1, 1.0);
  REQUIRE_TRUE(first, "channel lane did not retain its saved baseline and point IDs");
  REQUIRE_TRUE(second, "channel lane did not retain its saved baseline and point IDs");
  REQUIRE_NE(first.value(), second.value(),
             "channel lane did not retain its saved baseline and point IDs");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().domain.default_value,
             0.8, "channel lane did not retain its saved baseline and point IDs");
  REQUIRE_TRUE(session.value()->parameter_automation().set_channel_level_automation_value(
                   track, first.value(), 0.4),
               "channel point value or Undo/Redo changed identity");
  REQUIRE_TRUE(session.value()->history().undo(),
               "channel point value or Undo/Redo changed identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points[0].value, 0.5,
             "channel point value or Undo/Redo changed identity");
  REQUIRE_TRUE(session.value()->history().redo(),
               "channel point value or Undo/Redo changed identity");
  REQUIRE_TRUE(session.value()->parameter_automation().remove_channel_level_automation_point(
                   track, second.value()),
               "channel point removal was not atomic or durable");
  REQUIRE_TRUE(session.value()->history().undo(),
               "channel point removal was not atomic or durable");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.size(), 2,
             "channel point removal was not atomic or durable");
  REQUIRE_TRUE(session.value()->saving().save(), "channel point removal was not atomic or durable");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "saved channel automation did not reopen or remove by stable ID");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[0].id,
             first.value(), "saved channel automation did not reopen or remove by stable ID");
  REQUIRE_TRUE(reopened.value()->parameter_automation().remove_channel_level_automation_lane(track),
               "saved channel automation did not reopen or remove by stable ID");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().automation.empty(),
               "saved channel automation did not reopen or remove by stable ID");
}

void channel_automation_point_move_keeps_identity_and_timebase() {
  Directory directory;
  const auto root = directory.path() / "channel-point-move";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "channel move project creation failed");
  const auto track = session.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_OK(track, "channel move track setup failed");
  const auto first = session.value()->parameter_automation().set_channel_level_automation_point(
      track.value(), 0, 0.2);
  const auto moving = session.value()->parameter_automation().set_channel_level_automation_point(
      track.value(), 2, 0.6);
  const auto last = session.value()->parameter_automation().set_channel_level_automation_point(
      track.value(), 4, 0.8);
  REQUIRE_TRUE(first, "channel move point setup failed");
  REQUIRE_TRUE(moving, "channel move point setup failed");
  REQUIRE_TRUE(last, "channel move point setup failed");
  const auto lane_id = session.value()->snapshot().tracks.front().automation.front().id;
  const project::AutomationHandles handles{0.12, 0.04, 0.75, 0.85};
  require(session.value()
              ->automation()
              .set_automation_segment_curve(lane_id, moving.value(),
                                            project::AutomationShape::bezier, handles)
              .has_value(),
          "channel move curve setup failed");
  const auto before = session.value()->snapshot().tracks.front().automation.front();
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->parameter_automation().move_channel_level_automation_point(
                   track.value(), moving.value(), 4, 0.5),
               "colliding or invalid channel move changed the lane");
  REQUIRE_TRUE(!session.value()->parameter_automation().move_channel_level_automation_point(
                   track.value(), moving.value(), -1, 0.5),
               "colliding or invalid channel move changed the lane");
  REQUIRE_TRUE(!session.value()->parameter_automation().move_channel_level_automation_point(
                   track.value(), moving.value(), 3, 5.0),
               "colliding or invalid channel move changed the lane");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "colliding or invalid channel move changed the lane");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), before,
             "colliding or invalid channel move changed the lane");
  REQUIRE_TRUE(session.value()->parameter_automation().move_channel_level_automation_point(
                   track.value(), moving.value(), 3, 0.5),
               "channel 2D move did not produce one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "channel 2D move did not produce one revision");
  const auto moved = session.value()->snapshot().tracks.front().automation.front();
  const auto bar_four = session.value()->snapshot().meter.tick_at({4, 1, 0});
  REQUIRE_TRUE(bar_four, "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[1].id, moving.value(),
             "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[1].coordinate, bar_four.value().value,
             "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[1].value, 0.5, "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[1].to_next, project::AutomationShape::bezier,
             "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[1].handles, handles,
             "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[0], before.points[0],
             "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(moved.points[2], before.points[2],
             "channel move lost point identity, curve or undo state");
  REQUIRE_TRUE(session.value()->history().undo(),
               "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), before,
             "channel move lost point identity, curve or undo state");
  REQUIRE_TRUE(session.value()->history().redo(),
               "channel move lost point identity, curve or undo state");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), moved,
             "channel move lost point identity, curve or undo state");
  const auto bar_five = session.value()->snapshot().meter.tick_at({5, 1, 0});
  REQUIRE_TRUE(bar_five, "channel move tempo-ramp setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event(bar_five.value(), 90.0, timeline::TempoShape::step),
      "channel move tempo-ramp setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
      "channel move tempo-ramp setup failed");
  REQUIRE_OK(session.value()->automation().toggle_automation_lane_timebase(lane_id),
             "channel move elapsed lane setup failed");
  const auto elapsed_revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->parameter_automation().move_channel_level_automation_point(
                   track.value(), moving.value(), 2, 0.7),
               "elapsed channel move did not produce one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, elapsed_revision + 1,
             "elapsed channel move did not produce one revision");
  const auto bar_three = session.value()->snapshot().meter.tick_at({3, 1, 0});
  REQUIRE_OK(bar_three, "elapsed channel move bar missing");
  const auto sample = session.value()->snapshot().tempo.tick_to_sample(
      bar_three.value(), session.value()->snapshot().sample_rate);
  const auto elapsed_lane = session.value()->snapshot().tracks.front().automation.front();
  REQUIRE_TRUE(sample, "elapsed channel move used wrong tempo coordinate or lost curve");
  REQUIRE_EQ(elapsed_lane.timebase, project::AutomationTimebase::elapsed,
             "elapsed channel move used wrong tempo coordinate or lost curve");
  REQUIRE_EQ(elapsed_lane.points[1].id, moving.value(),
             "elapsed channel move used wrong tempo coordinate or lost curve");
  REQUIRE_EQ(elapsed_lane.points[1].coordinate, sample.value().value,
             "elapsed channel move used wrong tempo coordinate or lost curve");
  REQUIRE_EQ(elapsed_lane.points[1].handles, handles,
             "elapsed channel move used wrong tempo coordinate or lost curve");
  REQUIRE_TRUE(session.value()->saving().save(),
               "elapsed channel move used wrong tempo coordinate or lost curve");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "channel point move did not survive save and reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front(), elapsed_lane,
             "channel point move did not survive save and reopen");
}

void automation_segment_curve_command_is_atomic_and_durable() {
  Directory directory;
  const auto root = directory.path() / "curve-command-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "curve command project creation failed");
  const auto track = session.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_OK(track, "curve command bus creation failed");
  const auto first = session.value()->parameter_automation().set_channel_level_automation_point(
      track.value(), 0, 0.2);
  const auto second = session.value()->parameter_automation().set_channel_level_automation_point(
      track.value(), 2, 0.8);
  REQUIRE_TRUE(first, "curve command point creation failed");
  REQUIRE_TRUE(second, "curve command point creation failed");
  const auto original = session.value()->snapshot().tracks.front().automation.front();
  const auto missing_lane = project::AutomationLaneId::from_bits({0, 999}).value();
  const auto missing_point = project::AutomationPointId::from_bits({0, 999}).value();
  const project::AutomationHandles custom{0.12, 0.04, 0.75, 0.85};
  const auto before_invalid = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->automation().set_automation_segment_curve(
                   missing_lane, first.value(), project::AutomationShape::bezier, custom),
               "invalid segment command mutated the lane or history");
  REQUIRE_TRUE(!session.value()->automation().set_automation_segment_curve(
                   original.id, missing_point, project::AutomationShape::bezier, custom),
               "invalid segment command mutated the lane or history");
  REQUIRE_TRUE(!session.value()->automation().set_automation_segment_curve(
                   original.id, second.value(), project::AutomationShape::bezier, custom),
               "invalid segment command mutated the lane or history");
  REQUIRE_TRUE(
      !session.value()->automation().set_automation_segment_curve(
          original.id, first.value(), project::AutomationShape::bezier, {0.9, 0.2, 0.1, 0.8}),
      "invalid segment command mutated the lane or history");
  REQUIRE_TRUE(!session.value()->automation().set_automation_segment_curve(
                   original.id, first.value(), static_cast<project::AutomationShape>(99), custom),
               "invalid segment command mutated the lane or history");
  REQUIRE_EQ(session.value()->snapshot().revision, before_invalid,
             "invalid segment command mutated the lane or history");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), original,
             "invalid segment command mutated the lane or history");

  for (const auto shape :
       {project::AutomationShape::hold, project::AutomationShape::linear,
        project::AutomationShape::quadratic, project::AutomationShape::exponential,
        project::AutomationShape::ease_in_out, project::AutomationShape::bezier}) {
    const auto before = session.value()->snapshot().tracks.front().automation.front();
    const auto revision = session.value()->snapshot().revision;
    REQUIRE_TRUE(session.value()->automation().set_automation_segment_curve(
                     original.id, first.value(), shape, custom),
                 "supported segment shape was not one model revision");
    REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
               "supported segment shape was not one model revision");
    const auto changed = session.value()->snapshot().tracks.front().automation.front();
    REQUIRE_EQ(changed.id, original.id, "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(changed.points[0].id, first.value(),
               "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(changed.points[1], original.points[1],
               "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(changed.points[0].to_next, shape,
               "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(changed.points[0].handles, custom,
               "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(changed.settings_generation, before.settings_generation + 1,
               "segment change did not preserve lane or undo atomically");
    REQUIRE_TRUE(session.value()->history().undo(),
                 "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), before,
               "segment change did not preserve lane or undo atomically");
    REQUIRE_TRUE(session.value()->history().redo(),
                 "segment change did not preserve lane or undo atomically");
    REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front(), changed,
               "segment change did not preserve lane or undo atomically");
  }
  const auto revised = session.value()->snapshot().tracks.front().automation.front();
  const auto before_same = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->automation().set_automation_segment_curve(
                   revised.id, first.value(), project::AutomationShape::bezier, custom),
               "identical curve command created a history entry");
  REQUIRE_EQ(session.value()->snapshot().revision, before_same,
             "identical curve command created a history entry");
  REQUIRE_OK(session.value()->saving().save(), "segment curve save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "segment curve or identities changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front(), revised,
             "segment curve or identities changed after reopen");
}

void clip_automation_segment_curve_uses_stable_lane_identity() {
  Directory directory;
  const auto root = directory.path() / "clip-curve-command-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "clip curve project creation failed");
  FileDescriptor source(make_wav(directory.path(), 48'000, {48'000}));
  REQUIRE_OK(session.value()->imports().import_wav_audio(source.get()), "clip curve import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  const auto first = session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
      clip, {0.1}, 0.25);
  const auto second = session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
      clip, {0.9}, 1.5);
  REQUIRE_TRUE(first, "clip curve point creation failed");
  REQUIRE_TRUE(second, "clip curve point creation failed");
  const auto before = session.value()->snapshot().tracks.front().clips.front().automation.front();
  const project::AutomationHandles handles{0.1, 0.2, 0.8, 0.9};
  require(session.value()
              ->automation()
              .set_automation_segment_curve(before.id, first.value(),
                                            project::AutomationShape::bezier, handles)
              .has_value(),
          "clip curve edit failed");
  const auto after = session.value()->snapshot().tracks.front().clips.front().automation.front();
  REQUIRE_EQ(after.id, before.id, "clip curve edit did not round-trip through undo or save");
  REQUIRE_EQ(after.points.front().id, first.value(),
             "clip curve edit did not round-trip through undo or save");
  REQUIRE_EQ(after.points.back().id, second.value(),
             "clip curve edit did not round-trip through undo or save");
  REQUIRE_EQ(after.points.front().handles, handles,
             "clip curve edit did not round-trip through undo or save");
  REQUIRE_TRUE(session.value()->history().undo(),
               "clip curve edit did not round-trip through undo or save");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().automation.front(), before,
             "clip curve edit did not round-trip through undo or save");
  REQUIRE_TRUE(session.value()->history().redo(),
               "clip curve edit did not round-trip through undo or save");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().automation.front(), after,
             "clip curve edit did not round-trip through undo or save");
  REQUIRE_TRUE(session.value()->saving().save(),
               "clip curve edit did not round-trip through undo or save");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "clip curve or identity changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().automation.front(), after,
             "clip curve or identity changed after reopen");
}

void vst3_parameter_automation_edits_are_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "vst3-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "VST3 automation project creation failed");
  const auto track = session.value()->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_OK(track, "VST3 automation bus creation failed");
  const application::Vst3InsertIdentity identity{std::string(32, 'a'), "Fixture Vendor",
                                                 "Fixture Effect",     "1.0",
                                                 "/opt/fixture.vst3",  std::string(64, 'b')};
  const auto inserted = session.value()->effects().insert_vst3_plugin(track.value(), 0, identity);
  REQUIRE_OK(inserted, "VST3 automation plugin insertion failed");
  const auto slot = session.value()->snapshot().tracks.front().effects.front().id;
  const auto saved = std::get<project::Vst3PluginInstance>(
      session.value()->snapshot().tracks.front().effects.front().processor);
  require(session.value()
              ->effects()
              .set_vst3_editor_state(session.value()->snapshot().revision, track.value(), slot,
                                     saved, {}, {{7, 0.4}})
              .has_value(),
          "VST3 automation saved parameter reference failed");
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->parameter_automation().set_vst3_parameter_automation_point(
                   track.value(), inserted.value(), 7, -1, 0.5),
               "invalid VST3 automation changed the project");
  REQUIRE_TRUE(!session.value()->parameter_automation().set_vst3_parameter_automation_point(
                   track.value(), inserted.value(), 8, 0, 0.5),
               "invalid VST3 automation changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "invalid VST3 automation changed the project");
  const auto first = session.value()->parameter_automation().set_vst3_parameter_automation_point(
      track.value(), inserted.value(), 7, 0, 0.2);
  const auto second = session.value()->parameter_automation().set_vst3_parameter_automation_point(
      track.value(), inserted.value(), 7, 2, 0.8);
  REQUIRE_TRUE(first, "VST3 automation lane lost saved baseline or point identity");
  REQUIRE_TRUE(second, "VST3 automation lane lost saved baseline or point identity");
  REQUIRE_NE(first.value(), second.value(),
             "VST3 automation lane lost saved baseline or point identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().domain.default_value,
             0.4, "VST3 automation lane lost saved baseline or point identity");
  REQUIRE_TRUE(session.value()->parameter_automation().set_vst3_parameter_automation_value(
                   track.value(), inserted.value(), 7, first.value(), 0.3),
               "VST3 automation value, undo or save failed");
  REQUIRE_TRUE(session.value()->history().undo(), "VST3 automation value, undo or save failed");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().points.front().value,
             0.2, "VST3 automation value, undo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(), "VST3 automation value, undo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "VST3 automation value, undo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "VST3 automation did not reopen or remove by target identity");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points.front().id,
             first.value(), "VST3 automation did not reopen or remove by target identity");
  REQUIRE_TRUE(reopened.value()->parameter_automation().remove_vst3_parameter_automation_point(
                   track.value(), inserted.value(), 7, second.value()),
               "VST3 automation did not reopen or remove by target identity");
  REQUIRE_TRUE(reopened.value()->parameter_automation().remove_vst3_parameter_automation_lane(
                   track.value(), inserted.value(), 7),
               "VST3 automation did not reopen or remove by target identity");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().automation.empty(),
               "VST3 automation did not reopen or remove by target identity");
}

void vst3_instrument_parameter_automation_keeps_source_identity() {
  Directory directory;
  const auto root = directory.path() / "vst3-instrument-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "VST3 instrument automation project creation failed");
  const application::Vst3InsertIdentity identity{
      std::string(32, 'c'),           "Fixture Vendor",    "Fixture Instrument", "1.0",
      "/opt/fixture-instrument.vst3", std::string(64, 'd')};
  const auto track = session.value()->instruments().create_vst3_instrument_track(identity);
  REQUIRE_OK(track, "VST3 instrument automation source creation failed");
  const auto saved = std::get<project::Vst3PluginInstance>(
      session.value()->snapshot().tracks.front().instrument_source);
  require(session.value()
              ->instruments()
              .set_vst3_instrument_state(session.value()->snapshot().revision, track.value(), saved,
                                         {}, {{3, 0.65}})
              .has_value(),
          "VST3 instrument parameter reference save failed");
  const auto point = session.value()->parameter_automation().set_vst3_parameter_automation_point(
      track.value(), saved.id, 3, 1, 0.25);
  REQUIRE_TRUE(point, "VST3 instrument parameter lane did not retain baseline");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.size(), 1,
             "VST3 instrument parameter lane did not retain baseline");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().automation.front().domain.default_value,
             0.65, "VST3 instrument parameter lane did not retain baseline");
  REQUIRE_TRUE(session.value()->saving().save(),
               "VST3 instrument parameter lane did not retain baseline");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "VST3 instrument source and lane did not reopen together");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks.front().instrument_source)
                 .id,
             saved.id, "VST3 instrument source and lane did not reopen together");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points.front().id,
             point.value(), "VST3 instrument source and lane did not reopen together");
}

void two_bare_vst3_instrument_tracks_reopen_and_refuse_a_third() {
  Directory directory;
  const auto root = directory.path() / "two-vst3-instruments";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "two-instrument project creation failed");
  const application::Vst3InsertIdentity first_identity{
      std::string(32, 'c'),         "Fixture Vendor",    "First Instrument", "1.0",
      "/opt/first-instrument.vst3", std::string(64, 'd')};
  auto second_identity = first_identity;
  second_identity.name = "Second Instrument";
  second_identity.class_id = std::string(32, 'e');
  const auto first = session.value()->instruments().create_vst3_instrument_track(first_identity);
  const auto second = session.value()->instruments().create_vst3_instrument_track(second_identity);
  REQUIRE_TRUE(first, "two bare VST3 instrument tracks were not created separately");
  REQUIRE_TRUE(second, "two bare VST3 instrument tracks were not created separately");
  REQUIRE_NE(first.value(), second.value(),
             "two bare VST3 instrument tracks were not created separately");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "two bare VST3 instrument tracks were not created separately");
  const auto& tracks = session.value()->snapshot().tracks;
  const auto first_plugin = std::get<project::Vst3PluginInstance>(tracks[0].instrument_source);
  const auto second_plugin = std::get<project::Vst3PluginInstance>(tracks[1].instrument_source);
  REQUIRE_NE(first_plugin.id, second_plugin.id, "two VST3 source identities were conflated");
  REQUIRE_NE(first_plugin.class_id, second_plugin.class_id,
             "two VST3 source identities were conflated");
  // Any number of instrument tracks play together (ADR-0050).
  const auto third = session.value()->instruments().create_vst3_instrument_track(first_identity);
  REQUIRE_TRUE(third, "a third VST3 instrument track was refused");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 3,
             "a third VST3 instrument track was refused");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "two VST3 instrument identities did not reopen separately");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 3,
             "two VST3 instrument identities did not reopen separately");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].id, first.value(),
             "two VST3 instrument identities did not reopen separately");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[1].id, second.value(),
             "two VST3 instrument identities did not reopen separately");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks[0].instrument_source),
             first_plugin, "two VST3 instrument identities did not reopen separately");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks[1].instrument_source),
             second_plugin, "two VST3 instrument identities did not reopen separately");
}

void paired_vst3_instrument_effect_tracks_can_be_created_in_order() {
  Directory directory;
  const auto root = directory.path() / "paired-vst3-instrument-effects";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "paired instrument project creation failed");
  const application::Vst3InsertIdentity source_identity{
      std::string(32, 'c'),   "Fixture Vendor",    "Instrument", "1.0",
      "/opt/instrument.vst3", std::string(64, 'd')};
  const application::Vst3InsertIdentity effect_identity{
      std::string(32, 'e'), "Fixture Vendor",    "Effect", "1.0",
      "/opt/effect.vst3",   std::string(64, 'f')};
  const auto first = session.value()->instruments().create_vst3_instrument_track(source_identity);
  REQUIRE_TRUE(first, "first instrument/effect path was not created");
  REQUIRE_TRUE(session.value()->effects().insert_vst3_plugin(first.value(), 0, effect_identity),
               "first instrument/effect path was not created");
  const auto second = session.value()->instruments().create_vst3_instrument_track(source_identity);
  REQUIRE_TRUE(second, "second source was blocked by the first source's effect");
  REQUIRE_NE(second.value(), first.value(),
             "second source was blocked by the first source's effect");
  REQUIRE_OK(session.value()->effects().insert_vst3_plugin(second.value(), 0, effect_identity),
             "second instrument/effect path was not created");
  REQUIRE_OK(session.value()->saving().save(), "paired instrument paths did not save");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "paired instrument/effect paths did not reopen in saved order");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 2,
             "paired instrument/effect paths did not reopen in saved order");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].effects.size(), 1,
             "paired instrument/effect paths did not reopen in saved order");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[1].effects.size(), 1,
             "paired instrument/effect paths did not reopen in saved order");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].id, first.value(),
             "paired instrument/effect paths did not reopen in saved order");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[1].id, second.value(),
             "paired instrument/effect paths did not reopen in saved order");
}

void channel_gestures_coalesce_revisions_into_one_undo_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "channel-gesture-song");
  REQUIRE_TRUE(session, "channel gesture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "channel gesture setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(session.value()->tracks().set_track_level_pan(track, 0.8, 0.0, 71),
               "channel gesture revisions failed");
  REQUIRE_TRUE(session.value()->tracks().set_track_level_pan(track, 0.6, 0.0, 71),
               "channel gesture revisions failed");
  REQUIRE_TRUE(session.value()->tracks().set_master_level(0.8, 72),
               "channel gesture revisions failed");
  REQUIRE_TRUE(session.value()->tracks().set_master_level(0.5, 72),
               "channel gesture revisions failed");
  REQUIRE_TRUE(session.value()->history().undo(), "master drag required more than one Undo step");
  REQUIRE_EQ(session.value()->snapshot().master_level, 1.0,
             "master drag required more than one Undo step");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().level, 0.6,
             "master drag required more than one Undo step");
  REQUIRE_TRUE(session.value()->history().undo(), "track drag required more than one Undo step");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().level, 1.0,
             "track drag required more than one Undo step");
  REQUIRE_TRUE(session.value()->history().redo(), "channel gesture Redo lost its final value");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().level, 0.6,
             "channel gesture Redo lost its final value");
  REQUIRE_TRUE(session.value()->history().redo(), "channel gesture Redo lost its final value");
  REQUIRE_EQ(session.value()->snapshot().master_level, 0.5,
             "channel gesture Redo lost its final value");
}

void audio_clip_channel_envelope_is_local_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "clip-channel-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "clip channel fixture create failed");
  FileDescriptor source(make_wav(directory.path(), 48'000, {192'000}));
  REQUIRE_OK(session.value()->imports().import_wav_audio(source.get()),
             "clip channel fixture import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->automation().set_audio_clip_channel_level_point(clip, -1, 0.5),
               "invalid or out-of-clip channel point changed the project");
  REQUIRE_TRUE(!session.value()->automation().set_audio_clip_channel_level_point(clip, 2, 0.5),
               "invalid or out-of-clip channel point changed the project");
  REQUIRE_TRUE(!session.value()->automation().set_audio_clip_channel_level_point(clip, 0, 5.0),
               "invalid or out-of-clip channel point changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid or out-of-clip channel point changed the project");
  const auto first =
      session.value()->automation().set_audio_clip_channel_level_point(clip, 0, 0.25);
  const auto second =
      session.value()->automation().set_audio_clip_channel_level_point(clip, 1, 0.5);
  REQUIRE_TRUE(first, "clip channel point placement failed");
  REQUIRE_TRUE(second, "clip channel point placement failed");
  REQUIRE_NE(first.value(), second.value(), "clip channel point placement failed");
  const auto& saved_clip = session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().automation.empty(),
               "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_EQ(saved_clip.automation.size(), 1,
             "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_EQ(saved_clip.automation.front().points.size(), 2,
             "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_EQ(saved_clip.automation.front().points[0].coordinate, 0,
             "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_EQ(saved_clip.automation.front().points[1].coordinate, 96'000,
             "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_EQ(saved_clip.automation.front().timebase, project::AutomationTimebase::elapsed,
             "clip channel points did not use clip-local elapsed coordinates");
  REQUIRE_TRUE(
      session.value()->automation().set_audio_clip_channel_level_value(clip, first.value(), 0.4),
      "clip channel point value or Undo/Redo lost identity");
  REQUIRE_TRUE(session.value()->history().undo(),
               "clip channel point value or Undo/Redo lost identity");
  REQUIRE_TRUE(session.value()->history().redo(),
               "clip channel point value or Undo/Redo lost identity");
  REQUIRE_EQ(
      session.value()->snapshot().tracks.front().clips.front().automation.front().points[0].value,
      0.4, "clip channel point value or Undo/Redo lost identity");
  const auto lane_id =
      session.value()->snapshot().tracks.front().clips.front().automation.front().id;
  const auto bar_two = session.value()->snapshot().meter.tick_at({2, 1, 0});
  REQUIRE_TRUE(bar_two, "clip channel timebase switch did not preserve the local bar position");
  REQUIRE_TRUE(session.value()->automation().toggle_automation_lane_timebase(lane_id),
               "clip channel timebase switch did not preserve the local bar position");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().automation.front().timebase,
             project::AutomationTimebase::musical,
             "clip channel timebase switch did not preserve the local bar position");
  REQUIRE_EQ(session.value()
                 ->snapshot()
                 .tracks.front()
                 .clips.front()
                 .automation.front()
                 .points[1]
                 .coordinate,
             bar_two.value().value,
             "clip channel timebase switch did not preserve the local bar position");
  const auto updated =
      session.value()->automation().set_audio_clip_channel_level_point(clip, 1, 0.6);
  REQUIRE_TRUE(updated, "clip channel musical point upsert changed its stable identity");
  REQUIRE_EQ(updated.value(), second.value(),
             "clip channel musical point upsert changed its stable identity");
  const auto before_fraction = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, application::ClipFraction{1.0}, 0.5),
               "out-of-clip fraction changed the project");
  REQUIRE_TRUE(!session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, application::ClipFraction{-0.1}, 0.5),
               "out-of-clip fraction changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before_fraction,
             "out-of-clip fraction changed the project");
  const auto fractional =
      session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
          clip, application::ClipFraction{0.25}, 0.75);
  REQUIRE_TRUE(fractional, "fractional clip point was not added");
  REQUIRE_NE(fractional.value(), first.value(), "fractional clip point was not added");
  REQUIRE_NE(fractional.value(), second.value(), "fractional clip point was not added");
  const auto before_collision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->automation().move_audio_clip_channel_level_point(
                   clip, fractional.value(), application::ClipFraction{0.0}, 0.9),
               "point move onto an occupied coordinate changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before_collision,
             "point move onto an occupied coordinate changed the project");
  REQUIRE_TRUE(session.value()->automation().move_audio_clip_channel_level_point(
                   clip, fractional.value(), application::ClipFraction{0.375}, 0.9),
               "fractional point move did not preserve identity or one-step undo");
  REQUIRE_EQ(
      session.value()->snapshot().tracks.front().clips.front().automation.front().points[1].id,
      fractional.value(), "fractional point move did not preserve identity or one-step undo");
  REQUIRE_EQ(
      session.value()->snapshot().tracks.front().clips.front().automation.front().points[1].value,
      0.9, "fractional point move did not preserve identity or one-step undo");
  REQUIRE_TRUE(session.value()->history().undo(),
               "fractional point move did not preserve identity or one-step undo");
  REQUIRE_TRUE(session.value()->history().redo(),
               "fractional point move did not preserve identity or one-step undo");
  REQUIRE_EQ(
      session.value()->snapshot().tracks.front().clips.front().automation.front().points[1].id,
      fractional.value(), "fractional point move did not preserve identity or one-step undo");
  REQUIRE_OK(session.value()->saving().save(), "clip channel save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "clip channel envelope did not reopen or support point/lane removal");
  REQUIRE_EQ(
      reopened.value()->snapshot().tracks.front().clips.front().automation.front().points[0].id,
      first.value(), "clip channel envelope did not reopen or support point/lane removal");
  REQUIRE_TRUE(
      reopened.value()->automation().remove_audio_clip_channel_level_point(clip, second.value()),
      "clip channel envelope did not reopen or support point/lane removal");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "clip channel envelope did not reopen or support point/lane removal");
  REQUIRE_TRUE(reopened.value()->automation().remove_audio_clip_channel_level_lane(clip),
               "clip channel envelope did not reopen or support point/lane removal");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().clips.front().automation.empty(),
               "clip channel envelope did not reopen or support point/lane removal");
}

void tempo_events_are_atomic_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "tempo-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "tempo project creation failed");
  constexpr auto bar_five = 16 * timeline::ticks_per_quarter;
  constexpr auto bar_nine = 32 * timeline::ticks_per_quarter;
  const auto before_invalid = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->song().set_tempo_event({-1}, 120.0, timeline::TempoShape::step),
               "invalid tempo command changed the project");
  REQUIRE_TRUE(
      !session.value()->song().set_tempo_event({bar_five}, 0.0, timeline::TempoShape::step),
      "invalid tempo command changed the project");
  REQUIRE_TRUE(!session.value()->song().remove_tempo_event({0}),
               "invalid tempo command changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before_invalid,
             "invalid tempo command changed the project");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({bar_five}, 100.0, timeline::TempoShape::step),
      "tempo event was not inserted");
  REQUIRE_EQ(session.value()->snapshot().tempo.events().size(), 2, "tempo event was not inserted");
  const auto after_insert = session.value()->snapshot().revision;
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({bar_five}, 100.0, timeline::TempoShape::step),
      "same tempo setting created an undo step");
  REQUIRE_EQ(session.value()->snapshot().revision, after_insert,
             "same tempo setting created an undo step");
  auto mapped = session.value()->snapshot().tempo.tick_to_sample({bar_nine}, 48'000);
  REQUIRE_TRUE(mapped, "tempo event changed elapsed timing incorrectly");
  REQUIRE_EQ(mapped.value().value, 844'800, "tempo event changed elapsed timing incorrectly");
  REQUIRE_TRUE(session.value()->history().undo(), "tempo event did not undo as one operation");
  REQUIRE_EQ(session.value()->snapshot().tempo.events().size(), 1,
             "tempo event did not undo as one operation");
  mapped = session.value()->snapshot().tempo.tick_to_sample({bar_nine}, 48'000);
  REQUIRE_TRUE(mapped, "tempo undo/redo or save failed");
  REQUIRE_EQ(mapped.value().value, 768'000, "tempo undo/redo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(), "tempo undo/redo or save failed");
  REQUIRE_EQ(session.value()->snapshot().tempo.events().size(), 2,
             "tempo undo/redo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "tempo undo/redo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "tempo event changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tempo.events().size(), 2,
             "tempo event changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tempo.events()[1].tick.value, bar_five,
             "tempo event changed after reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tempo.events()[1].bpm, 100.0,
             "tempo event changed after reopen");
  REQUIRE_TRUE(reopened.value()->song().remove_tempo_event({bar_five}),
               "tempo event removal did not undo atomically");
  REQUIRE_EQ(reopened.value()->snapshot().tempo.events().size(), 1,
             "tempo event removal did not undo atomically");
  REQUIRE_TRUE(reopened.value()->history().undo(), "tempo event removal did not undo atomically");
  REQUIRE_EQ(reopened.value()->snapshot().tempo.events().size(), 2,
             "tempo event removal did not undo atomically");
}

void track_automation_bar_placement_preserves_elapsed_lane() {
  Directory directory;
  const auto root = directory.path() / "elapsed-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "elapsed automation setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "elapsed automation setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(session.value()->parameter_automation().set_channel_control_point(
                   track, channel_level, 0, 0.5),
               "elapsed automation fixture save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "elapsed automation fixture save failed");
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "elapsed automation fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "elapsed automation fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    auto& lane = snapshot.tracks.front().automation.front();
    lane.timebase = project::AutomationTimebase::elapsed;
    ++lane.settings_generation;
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "elapsed automation fixture publication failed");
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "elapsed automation reopen failed");
  auto point = reopened.value()->parameter_automation().set_channel_control_point(
      track, channel_level, 1, 0.75);
  REQUIRE_TRUE(point, "bar placement changed elapsed-time lane or mapped to the wrong sample");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().timebase,
             project::AutomationTimebase::elapsed,
             "bar placement changed elapsed-time lane or mapped to the wrong sample");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[1].coordinate,
             96'000, "bar placement changed elapsed-time lane or mapped to the wrong sample");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().automation.front().points[1].id,
             point.value(),
             "bar placement changed elapsed-time lane or mapped to the wrong sample");
}

void clip_automation_timebase_switch_keeps_local_anchor() {
  Directory directory;
  const auto root = directory.path() / "clip-automation-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "clip automation setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "clip automation setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(
      session.value()->midi_clips().add_midi_clip(track, {2 * timeline::ticks_per_quarter}),
      "clip automation fixture publication failed");
  REQUIRE_TRUE(session.value()->parameter_automation().set_channel_control_point(
                   track, channel_level, 0, 0.5),
               "clip automation fixture publication failed");
  REQUIRE_TRUE(session.value()->saving().save(), "clip automation fixture publication failed");
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "clip automation writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "clip automation load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    auto& instrument = snapshot.tracks.front();
    auto lane = instrument.automation.front();
    instrument.automation.clear();
    lane.points.front().coordinate = timeline::ticks_per_quarter;
    lane.points.push_back({project::AutomationPointId::from_bits({0, 9'999}).value(),
                           2 * timeline::ticks_per_quarter, 0.75,
                           project::AutomationShape::linear});
    instrument.midi_clips.front().automation.push_back(lane);
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "clip-local automation fixture save failed");
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "clip automation reopen failed");
  const auto original =
      reopened.value()->snapshot().tracks.front().midi_clips.front().automation.front();
  const auto before = reopened.value()->snapshot().revision;
  REQUIRE_TRUE(reopened.value()->automation().toggle_automation_lane_timebase(original.id),
               "clip-local automation timebase did not publish one edit");
  REQUIRE_EQ(reopened.value()->snapshot().revision, before + 1,
             "clip-local automation timebase did not publish one edit");
  const auto elapsed =
      reopened.value()->snapshot().tracks.front().midi_clips.front().automation.front();
  REQUIRE_EQ(elapsed.timebase, project::AutomationTimebase::elapsed,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_EQ(elapsed.points[0].coordinate, 24'000,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_EQ(elapsed.points[1].coordinate, 48'000,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_EQ(elapsed.points[0].id, original.points[0].id,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_EQ(elapsed.points[1].id, original.points[1].id,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_EQ(elapsed.settings_generation, original.settings_generation + 1,
             "clip-local point positions or identities changed during conversion");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "clip-local timebase conversion did not undo, redo or save");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front().automation.front(),
             original, "clip-local timebase conversion did not undo, redo or save");
  REQUIRE_TRUE(reopened.value()->history().redo(),
               "clip-local timebase conversion did not undo, redo or save");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front().automation.front(),
             elapsed, "clip-local timebase conversion did not undo, redo or save");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "clip-local timebase conversion did not undo, redo or save");
  reopened.value().reset();
  auto saved = application::ProjectSession::open(root);
  REQUIRE_TRUE(saved, "clip-local automation did not reopen or round-trip to musical time");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.front().automation.front(),
             elapsed, "clip-local automation did not reopen or round-trip to musical time");
  REQUIRE_TRUE(saved.value()->automation().toggle_automation_lane_timebase(original.id),
               "clip-local automation did not reopen or round-trip to musical time");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.front().automation.front().points,
             original.points, "clip-local automation did not reopen or round-trip to musical time");
}

void session_commands_and_reopen() {
  Directory directory;
  const auto root = directory.path() / "song";
  require(!application::ProjectSession::open(root), "missing project opened");
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "create failed");
  const auto original_revision = session.value()->snapshot().revision;
  const auto absent_track = project::TrackId::from_bits({0, 99});
  const auto absent_clip = project::ClipId::from_bits({0, 99});
  REQUIRE_TRUE(absent_track, "test IDs invalid");
  REQUIRE_TRUE(absent_clip, "test IDs invalid");
  require(!session.value()->midi_clips().add_midi_clip(absent_track.value(), {0}),
          "missing track accepted clip");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "instrument track add failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  require(!session.value()->midi_clips().add_midi_clip(track, {-1}), "negative placement accepted");
  require(!session.value()->notes().add_note(absent_clip.value(), {0}, 60),
          "missing clip accepted note");
  REQUIRE_EQ(session.value()->snapshot().revision, original_revision + 1,
             "failed command changed project");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().midi_clips.empty(),
               "failed command changed project");
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}), "clip add failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session.value()->notes().add_note(clip, {0}, 60), "note add failed");
  REQUIRE_OK(session.value()->saving().save(), "save failed");
  session.value().reset();
  require(!application::ProjectSession::create(root), "create replaced existing project");
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "saved project did not reopen");
  const auto& tracks = reopened.value()->snapshot().tracks;
  REQUIRE_EQ(tracks.size(), 1, "session MIDI state did not round-trip");
  REQUIRE_EQ(tracks[0].midi_clips.size(), 1, "session MIDI state did not round-trip");
  REQUIRE_EQ(tracks[0].midi_clips[0].notes.size(), 1, "session MIDI state did not round-trip");
  REQUIRE_EQ(tracks[0].midi_clips[0].notes[0].key, 60, "session MIDI state did not round-trip");
}

void session_redo_restores_undone_edit_and_rejects_stale_history() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "redo project setup failed");
  const auto first = session.value()->tracks().add_instrument_track();
  REQUIRE_OK(first, "redo first track setup failed");
  const auto second = session.value()->tracks().add_instrument_track();
  REQUIRE_OK(second, "redo second track setup failed");
  const auto second_id = session.value()->snapshot().tracks[1].id;
  REQUIRE_OK(session.value()->history().undo(), "undo did not remove the second track");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1, "undo did not remove the second track");
  REQUIRE_OK(session.value()->history().redo(), "redo did not restore the same track identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "redo did not restore the same track identity");
  REQUIRE_EQ(session.value()->snapshot().tracks[1].id, second_id,
             "redo did not restore the same track identity");
  require(!session.value()->history().redo(), "redo repeated an already restored edit");
  REQUIRE_OK(session.value()->history().undo(), "second undo failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(),
             "a new edit did not invalidate the redo branch");
  REQUIRE_TRUE(!session.value()->history().redo(), "a new edit did not invalidate the redo branch");
  REQUIRE_OK(session.value()->saving().save(), "redone project save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "redone project did not reopen with the visible edits");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 2,
             "redone project did not reopen with the visible edits");
}

// The managed media of a built-in synth's slot (Anvil's click here).
const project::ManagedAudioReference* slot_media(const project::Track& track) {
  const auto* patch = native_patch(track);
  if (patch == nullptr)
    return nullptr;
  const auto found = std::ranges::find(patch->document.media, native_dsp::synth::anvil::click_slot,
                                       &project::NativeMediaSlot::id);
  return found == patch->document.media.end()
             ? nullptr
             : std::get_if<project::ManagedAudioReference>(&found->media);
}

void native_synth_media_import_is_atomic_and_undoable() {
  constexpr auto slot = native_dsp::synth::anvil::click_slot;
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "session setup failed");
  const auto added =
      session.value()->instruments().add_native_instrument_track("anvil", std::nullopt);
  REQUIRE_OK(added, "Anvil track setup failed");
  const auto track = added.value();
  const auto before = session.value()->snapshot().revision;
  require(!session.value()->imports().import_native_synth_media(track, slot, -1),
          "invalid fd accepted");
  const auto invalid = directory.path() / "invalid.bin";
  {
    std::ofstream output(invalid, std::ios::binary);
    output << "not audio";
  }
  {
    FileDescriptor descriptor(invalid);
    require(!session.value()->imports().import_native_synth_media(track, slot, descriptor.get()),
            "invalid media accepted");
  }
  REQUIRE_EQ(session.value()->snapshot().revision, before, "invalid input mutated project");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(), "invalid input mutated project");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_native_synth_media(track, slot, descriptor.get()),
             "media import failed");
  const auto& bound = session.value()->snapshot();
  REQUIRE_EQ(bound.revision, before + 1, "source and document were not one accepted revision");
  REQUIRE_EQ(bound.sources.size(), 1, "source and document were not one accepted revision");
  REQUIRE_NE(slot_media(bound.tracks.front()), nullptr,
             "source and document were not one accepted revision");
  REQUIRE_EQ(slot_media(bound.tracks.front())->source, bound.sources.front().id,
             "source and document were not one accepted revision");
  REQUIRE_OK(session.value()->imports().import_native_synth_media(track, slot, descriptor.get()),
             "importing the same sound again added a source");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "importing the same sound again added a source");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "imported synth media did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1, "imported synth media did not reopen");
  REQUIRE_NE(slot_media(reopened.value()->snapshot().tracks.front()), nullptr,
             "imported synth media did not reopen");
  reopened.value().reset();
  auto undo_session = application::ProjectSession::create(directory.path() / "undo-song");
  REQUIRE_OK(undo_session, "undo session setup failed");
  const auto undo_track =
      undo_session.value()->instruments().add_native_instrument_track("anvil", std::nullopt);
  REQUIRE_OK(undo_track, "undo session import failed");
  REQUIRE_TRUE(undo_session.value()
                   ->imports()
                   .import_native_synth_media(undo_track.value(), slot, descriptor.get())
                   .has_value(),
               "undo session import failed");
  REQUIRE_OK(undo_session.value()->history().undo(), "import undo failed");
  REQUIRE_TRUE(undo_session.value()->snapshot().sources.empty(),
               "undo did not remove source and slot together");
  REQUIRE_EQ(slot_media(undo_session.value()->snapshot().tracks.front()), nullptr,
             "undo did not remove source and slot together");
  REQUIRE_OK(undo_session.value()->saving().save(), "save after undo failed");
}

// A user's impulse response in a built-in effect's media slot (Cast,
// ADR-0115), on a track and on the master: the source and the document are
// one undoable revision, and a slot without a built-in effect is refused.
void native_effect_media_import_is_atomic_and_undoable() {
  constexpr std::string_view engine = "cast";
  constexpr std::string_view slot = "ir";
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  auto& opened = *session.value();
  const auto track = take(opened.tracks().add_audio_track());
  const project::SignalPathId track_path{track};
  const project::SignalPathId master_path{project::MasterSignalPath{}};
  const auto on_track = take(opened.effects().add_effect_engine(track_path, 0, engine));
  const auto on_master = take(opened.effects().add_effect_engine(master_path, 0, engine));
  const auto media_of = [&](const project::EffectInsert& insert) {
    const auto& document = std::get<project::NativeEffectPatch>(insert.processor).document;
    const auto found = std::ranges::find(document.media, slot, &project::NativeMediaSlot::id);
    return found == document.media.end()
               ? nullptr
               : std::get_if<project::ManagedAudioReference>(&found->media);
  };
  const auto before = opened.snapshot().revision;
  FileDescriptor descriptor(source);
  require(
      !opened.imports().import_native_effect_media(track_path, on_master, slot, descriptor.get()),
      "a slot that is not on the path was accepted");
  REQUIRE_EQ(opened.snapshot().revision, before, "a refused import changed the project");
  REQUIRE_OK(
      opened.imports().import_native_effect_media(track_path, on_track, slot, descriptor.get()),
      "an impulse response did not import into a track's effect");
  const auto& bound = opened.snapshot();
  REQUIRE_EQ(bound.revision, before + 1, "source and document were not one accepted revision");
  REQUIRE_EQ(bound.sources.size(), 1, "source and document were not one accepted revision");
  const auto* imported = media_of(bound.tracks.front().effects.front());
  REQUIRE_NE(imported, nullptr, "the effect's media slot does not play the import");
  REQUIRE_EQ(imported->source, bound.sources.front().id,
             "the effect's media slot does not play the import");
  REQUIRE_OK(
      opened.imports().import_native_effect_media(master_path, on_master, slot, descriptor.get()),
      "an impulse response did not import into a master effect");
  REQUIRE_EQ(opened.snapshot().sources.size(), 1, "importing the same sound again added a source");
  REQUIRE_NE(media_of(opened.snapshot().master_effects.front()), nullptr,
             "the master effect's media slot does not play the import");
  REQUIRE_OK(opened.history().undo(), "master import undo failed");
  REQUIRE_OK(opened.history().undo(), "track import undo failed");
  REQUIRE_TRUE(opened.snapshot().sources.empty(), "undo did not remove source and slot together");
  REQUIRE_EQ(media_of(opened.snapshot().tracks.front().effects.front()), nullptr,
             "undo did not remove source and slot together");
  REQUIRE_OK(opened.saving().save(), "save after undo failed");
}

// Begin on control, run on a worker, finish on control; a stale completion
// changes nothing and publishes no asset.
void native_synth_media_import_runs_on_a_worker() {
  constexpr auto slot = native_dsp::synth::anvil::click_slot;
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {64});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  const auto track =
      session.value()->instruments().add_native_instrument_track("anvil", std::nullopt);
  REQUIRE_OK(track, "Anvil track setup failed");
  require(!session.value()->imports().begin_native_synth_media_import(
              session.value()->snapshot().tracks.front().id, slot, -1),
          "begin accepted an invalid descriptor");
  FileDescriptor descriptor(source);

  auto stale = session.value()->imports().begin_native_synth_media_import(track.value(), slot,
                                                                          descriptor.get());
  REQUIRE_OK(stale, "stale import did not begin");
  auto stale_prepared = std::async(std::launch::async, [task = std::move(stale).value()]() mutable {
                          return std::move(task).run();
                        }).get();
  REQUIRE_OK(stale_prepared, "worker staging failed or lost the decoded frame count");
  REQUIRE_EQ(stale_prepared.value().source().frames, 64,
             "worker staging failed or lost the decoded frame count");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "intervening edit failed");
  const auto revision = session.value()->snapshot().revision;
  const auto finished_stale = session.value()->imports().finish_native_synth_media_import(
      std::move(stale_prepared).value());
  REQUIRE_TRUE(!finished_stale, "a stale import changed the project");
  REQUIRE_EQ(finished_stale.error().code, foundation::ErrorCode::conflict,
             "a stale import changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, revision, "a stale import changed the project");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(), "a stale import changed the project");

  auto task = session.value()->imports().begin_native_synth_media_import(track.value(), slot,
                                                                         descriptor.get());
  REQUIRE_OK(task, "import did not begin");
  REQUIRE_EQ(task.value().origin_revision(), revision, "import did not begin");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "beginning an import changed the project");
  auto prepared = std::async(std::launch::async, [task = std::move(task).value()]() mutable {
                    return std::move(task).run();
                  }).get();
  REQUIRE_OK(prepared, "worker staging failed");
  REQUIRE_TRUE(
      session.value()->imports().finish_native_synth_media_import(std::move(prepared).value()),
      "a current import did not finish as one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "a current import did not finish as one revision");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "a current import did not finish as one revision");
  REQUIRE_TRUE(session.value()->history().undo(), "the finished import is not one undo step");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "the finished import is not one undo step");
}

// A new sample with its first chops, tempo, name and root lands as one
// revision and one undo step (ADR-0063); a state the engine refuses changes
// nothing.
void crate_sample_import_sets_state_in_one_step() {
  namespace crate = native_dsp::synth::crate;
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {4'800});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  const auto track =
      session.value()->instruments().add_native_instrument_track(crate::engine_id, std::nullopt);
  REQUIRE_OK(track, "Crate track setup failed");
  const auto before = session.value()->snapshot();
  FileDescriptor descriptor(source);
  const auto prepare = [&] {
    auto task = session.value()->imports().begin_native_synth_media_import(
        track.value(), crate::sample_slot, descriptor.get());
    REQUIRE_OK(task, "Crate import did not begin");
    auto prepared = std::async(std::launch::async, [task = std::move(task).value()]() mutable {
                      return std::move(task).run();
                    }).get();
    REQUIRE_OK(prepared, "Crate import staging failed");
    return std::move(prepared).value();
  };
  const std::vector<project::NativeRegion> regions{
      {0, std::string(crate::sample_slot), 0, "kick", "Kick"},
      {3, std::string(crate::sample_slot), 2'400, "other", "Chop"}};
  const auto gain = crate::chop_parameter_id(3, "gain");

  application::NativeSynthMediaState refused{{}, {{std::string(crate::bpm_label), "fast"}}};
  REQUIRE_TRUE(
      !session.value()->imports().finish_native_synth_media_import(prepare(), regions, refused),
      "a refused state changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before.revision,
             "a refused state changed the project");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(), "a refused state changed the project");

  application::NativeSynthMediaState state{
      {{"crate.root", 48.0}, {gain, -6.0}},
      {{std::string(crate::bpm_label), "96"}, {std::string(crate::sample_name_label), "loop.wav"}}};
  REQUIRE_OK(session.value()->imports().finish_native_synth_media_import(prepare(), regions, state),
             "Crate import with state failed");
  const auto& after = session.value()->snapshot();
  REQUIRE_EQ(after.revision, before.revision + 1, "the sample and its state were not one revision");
  REQUIRE_EQ(after.sources.size(), 1, "the sample and its state were not one revision");
  const auto* patch = native_patch(after.tracks.front());
  REQUIRE_NE(patch, nullptr, "regions and labels were not written with the sample");
  REQUIRE_EQ(patch->document.regions, regions,
             "regions and labels were not written with the sample");
  REQUIRE_EQ(patch->document.labels.size(), 2,
             "regions and labels were not written with the sample");
  REQUIRE_EQ(patch->document.labels.front().text, "96",
             "regions and labels were not written with the sample");
  const auto values = session.value()->instruments().native_instrument_values(track.value());
  const auto& table = crate::engine().descriptor->parameters;
  const auto at = [&](std::string_view id) {
    const auto index = native_dsp::find_parameter(*crate::engine().descriptor, id);
    REQUIRE_OK(index, "Crate parameter missing");
    return *index;
  };
  REQUIRE_TRUE(values, "values were not replaced with the sample");
  REQUIRE_EQ(values.value()[at("crate.root")], 48.0, "values were not replaced with the sample");
  REQUIRE_EQ(values.value()[at(gain)], -6.0, "values were not replaced with the sample");
  REQUIRE_EQ(values.value()[at("crate.drive")], table[at("crate.drive")].default_value,
             "values were not replaced with the sample");
  REQUIRE_TRUE(session.value()->history().undo(), "the sample and its state are not one undo step");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "the sample and its state are not one undo step");
  REQUIRE_EQ(native_patch(session.value()->snapshot().tracks.front())->document,
             native_patch(before.tracks.front())->document,
             "the sample and its state are not one undo step");
}

void accepted_media_import_survives_save_failure() {
  constexpr auto slot = native_dsp::synth::anvil::click_slot;
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto initial = application::ProjectSession::create(root);
  REQUIRE_OK(initial, "fault fixture setup failed");
  const auto track =
      initial.value()->instruments().add_native_instrument_track("anvil", std::nullopt);
  REQUIRE_OK(track, "fault fixture setup failed");
  REQUIRE_OK(initial.value()->saving().save(), "fault fixture setup failed");
  initial.value().reset();
  bool fail_save = true;
  auto faulted =
      application::ProjectSession::open(root, [&fail_save](persistence::SaveStage stage) {
        return fail_save && stage == persistence::SaveStage::manifest_written;
      });
  REQUIRE_OK(faulted, "faulted session reopen failed");
  FileDescriptor descriptor(source);
  auto result =
      faulted.value()->imports().import_native_synth_media(track.value(), slot, descriptor.get());
  REQUIRE_TRUE(!result, "failed save was not reported after accepted edit");
  REQUIRE_NE(result.error().message.find("save failed"), std::string::npos,
             "failed save was not reported after accepted edit");
  REQUIRE_EQ(faulted.value()->snapshot().sources.size(), 1,
             "failed save discarded the accepted import");
  REQUIRE_NE(slot_media(faulted.value()->snapshot().tracks.front()), nullptr,
             "failed save discarded the accepted import");
  fail_save = false;
  REQUIRE_OK(faulted.value()->saving().save(), "retry save lost source receipt");
  faulted.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "retry did not persist the accepted import");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
             "retry did not persist the accepted import");
  REQUIRE_NE(slot_media(reopened.value()->snapshot().tracks.front()), nullptr,
             "retry did not persist the accepted import");
}

// A re-chop (ADR-0063): values and document change together as one undo step.
void native_instrument_state_is_one_undo_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  const auto track =
      session.value()->instruments().add_native_instrument_track("nova", std::nullopt);
  REQUIRE_OK(track, "Nova track setup failed");
  const auto original = *native_patch(session.value()->snapshot().tracks.front());
  auto document = original.document;
  std::uint32_t free_slot = 0;
  while (std::ranges::find(document.routes, free_slot, &project::NativeModRoute::slot) !=
         document.routes.end())
    ++free_slot;
  document.routes.push_back({free_slot, "macro.1", "", "nova.filter.1.cutoff", false, 0.0, 1.0});
  auto values = original.parameters;
  const auto amount = "nova.mod." + std::to_string(free_slot) + ".amount";
  values.push_back({amount, 0.5});
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->instruments().set_native_instrument_state(track.value(), values,
                                                                          document, 7),
               "values and document were not one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "values and document were not one revision");
  values.back().value = 0.75;
  require(session.value()
              ->instruments()
              .set_native_instrument_state(track.value(), values, document, 7)
              .has_value(),
          "a second step of the same gesture failed");
  const auto edited = *native_patch(session.value()->snapshot().tracks.front());
  REQUIRE_EQ(edited.document, document, "the state was not stored");
  REQUIRE_TRUE(std::ranges::any_of(edited.parameters,
                                   [&](const project::NativeInstrumentParameter& parameter) {
                                     return parameter.id == amount && parameter.value == 0.75;
                                   }),
               "the state was not stored");
  const auto revision = session.value()->snapshot().revision;
  auto unknown = values;
  unknown.push_back({"nova.not_a_parameter", 1.0});
  REQUIRE_TRUE(
      !session.value()->instruments().set_native_instrument_state(track.value(), unknown, document),
      "an invalid state or zero gesture changed the project");
  REQUIRE_TRUE(!session.value()->instruments().set_native_instrument_state(track.value(), values,
                                                                           document, 0),
               "an invalid state or zero gesture changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "an invalid state or zero gesture changed the project");
  REQUIRE_TRUE(session.value()->instruments().set_native_instrument_state(track.value(), values,
                                                                          document, 8),
               "an unchanged state added a revision");
  REQUIRE_EQ(session.value()->snapshot().revision, revision, "an unchanged state added a revision");
  REQUIRE_TRUE(session.value()->history().undo(),
               "the gesture did not undo values and document together");
  REQUIRE_EQ(*native_patch(session.value()->snapshot().tracks.front()), original,
             "the gesture did not undo values and document together");
}

// Chop to MIDI (ADR-0063): a clip with its notes as one undo step.
void midi_clip_with_notes_is_one_undo_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  const auto track = session.value()->instruments().add_synth_track();
  REQUIRE_OK(track, "synth track setup failed");
  constexpr auto quarter = timeline::ticks_per_quarter;
  const auto placeholder = project::NoteId::from_bits({0, 1}).value();
  std::vector<project::MidiNote> notes{{placeholder, {0}, {quarter}, 36, 100},
                                       {placeholder, {quarter}, {2 * quarter}, 37, 100}};
  REQUIRE_TRUE(
      !session.value()->midi_clips().add_midi_clip_with_notes(track.value(), {0}, {0}, notes),
      "an empty or negative clip was accepted");
  REQUIRE_TRUE(!session.value()->midi_clips().add_midi_clip_with_notes(track.value(), {-1},
                                                                       {quarter}, notes),
               "an empty or negative clip was accepted");
  const auto before = session.value()->snapshot().revision;
  auto clip = session.value()->midi_clips().add_midi_clip_with_notes(track.value(), {4 * quarter},
                                                                     {4 * quarter}, notes);
  REQUIRE_OK(clip, "the clip and its notes were not one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "the clip and its notes were not one revision");
  const auto& clips = session.value()->snapshot().tracks.front().midi_clips;
  REQUIRE_EQ(clips.size(), 1, "the clip did not keep its notes with fresh ids");
  REQUIRE_EQ(clips.front().id, clip.value(), "the clip did not keep its notes with fresh ids");
  REQUIRE_EQ(clips.front().notes.size(), 2, "the clip did not keep its notes with fresh ids");
  REQUIRE_NE(clips.front().notes[0].id, clips.front().notes[1].id,
             "the clip did not keep its notes with fresh ids");
  REQUIRE_EQ(clips.front().notes[1].key, 37, "the clip did not keep its notes with fresh ids");
  REQUIRE_EQ(clips.front().pattern_length.value, 4 * quarter,
             "the clip did not keep its notes with fresh ids");
  REQUIRE_TRUE(session.value()->history().undo(), "one undo did not remove the clip and its notes");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().midi_clips.empty(),
               "one undo did not remove the clip and its notes");
}

// A generated pattern (Knock, ADR-0068) replaces a clip's notes and its
// pattern length as one undo step.
void midi_clip_notes_replace_in_one_undo_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "session setup failed");
  const auto track = session.value()->instruments().add_synth_track();
  REQUIRE_OK(track, "synth track setup failed");
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr auto bar = 4 * quarter;
  const auto placeholder = project::NoteId::from_bits({0, 1}).value();
  const std::vector<project::MidiNote> first{{placeholder, {0}, {quarter}, 36, 100}};
  auto clip =
      session.value()->midi_clips().add_midi_clip_with_notes(track.value(), {bar}, {bar}, first);
  REQUIRE_OK(clip, "clip setup failed");
  const std::vector<project::MidiNote> pattern{
      {placeholder, {0}, {quarter / 8}, 37, 84},
      {placeholder, {bar}, {bar + quarter / 8}, 38, 116},
      {placeholder, {bar + quarter}, {bar + quarter + quarter / 8}, 39, 52}};
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->midi_clips().replace_midi_clip_notes(clip.value(), {0}, pattern),
               "an empty pattern or notes past its end were accepted");
  REQUIRE_TRUE(!session.value()->midi_clips().replace_midi_clip_notes(clip.value(), {bar}, pattern),
               "an empty pattern or notes past its end were accepted");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "an empty pattern or notes past its end were accepted");
  const auto replaced_ok =
      session.value()->midi_clips().replace_midi_clip_notes(clip.value(), {2 * bar}, pattern);
  REQUIRE_OK(replaced_ok, "the replacement failed: " +
                              (replaced_ok ? std::string() : replaced_ok.error().message));
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "the replacement was not one revision");
  const auto& replaced = session.value()->snapshot().tracks.front().midi_clips.front();
  const auto* placement = std::get_if<project::MusicalInterval>(&replaced.placement);
  REQUIRE_EQ(replaced.notes.size(), 3,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_EQ(replaced.notes[0].key, 37,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_EQ(replaced.notes[2].velocity, 52,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_NE(replaced.notes[0].id, replaced.notes[1].id,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_EQ(replaced.pattern_length.value, 2 * bar,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_NE(placement, nullptr, "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_EQ(placement->start.value, bar,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_EQ(placement->end.value, 3 * bar,
             "the clip did not take the pattern, its length and fresh ids");
  REQUIRE_OK(session.value()->history().undo(), "the replacement did not undo");
  const auto& restored = session.value()->snapshot().tracks.front().midi_clips.front();
  REQUIRE_EQ(restored.notes.size(), 1, "one undo did not restore the notes and the length");
  REQUIRE_EQ(restored.notes.front().key, 36, "one undo did not restore the notes and the length");
  REQUIRE_EQ(restored.pattern_length.value, bar,
             "one undo did not restore the notes and the length");
}

void wav_import_is_one_undoable_revision_and_reopens() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio import project setup failed");
  const auto before = session.value()->snapshot().revision;
  require(!session.value()->imports().import_wav_audio(-1), "invalid audio input fd was accepted");
  const auto invalid = directory.path() / "invalid.bin";
  {
    std::ofstream output(invalid, std::ios::binary);
    output << "not WAV";
  }
  {
    FileDescriptor descriptor(invalid);
    require(!session.value()->imports().import_wav_audio(descriptor.get()),
            "invalid WAV was accepted");
  }
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid audio import mutated the model");
  REQUIRE_TRUE(session.value()->snapshot().tracks.empty(),
               "invalid audio import mutated the model");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "invalid audio import mutated the model");
  {
    FileDescriptor descriptor(source);
    auto imported = session.value()->imports().import_wav_audio(descriptor.get());
    REQUIRE_OK(imported, "valid WAV import failed");
    const auto& snapshot = session.value()->snapshot();
    REQUIRE_EQ(snapshot.revision, before + 1,
               "source, audio track and clip were not one accepted revision");
    REQUIRE_EQ(snapshot.sources.size(), 1,
               "source, audio track and clip were not one accepted revision");
    REQUIRE_EQ(snapshot.tracks.size(), 1,
               "source, audio track and clip were not one accepted revision");
    REQUIRE_EQ(snapshot.tracks[0].id, imported.value(),
               "source, audio track and clip were not one accepted revision");
    REQUIRE_EQ(snapshot.tracks[0].kind, project::TrackKind::audio,
               "source, audio track and clip were not one accepted revision");
    REQUIRE_EQ(snapshot.tracks[0].clips.size(), 1,
               "source, audio track and clip were not one accepted revision");
    const auto& clip = snapshot.tracks[0].clips[0];
    REQUIRE_EQ(clip.source, snapshot.sources[0].id,
               "imported clip does not preserve source duration at timeline zero");
    REQUIRE_EQ(clip.source_range.start.value, 0,
               "imported clip does not preserve source duration at timeline zero");
    REQUIRE_EQ(clip.source_range.end.value, 4,
               "imported clip does not preserve source duration at timeline zero");
    REQUIRE_EQ(std::get<project::ElapsedInterval>(clip.placement),
               (project::ElapsedInterval{{0}, {4}}),
               "imported clip does not preserve source duration at timeline zero");
  }
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "managed WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1, "managed WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1, "managed WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].clips.size(), 1,
             "managed WAV import did not reopen");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(reopened.value()->imports().import_wav_audio(descriptor.get()),
               "duplicate-content import failed");
  }
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
             "duplicate-content import did not reuse the source record");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 2,
             "duplicate-content import did not reuse the source record");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].clips[0].source,
             reopened.value()->snapshot().tracks[1].clips[0].source,
             "duplicate-content import did not reuse the source record");
  REQUIRE_OK(reopened.value()->history().undo(),
             "one undo did not remove the duplicate audio track and clip");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1,
             "one undo did not remove the duplicate audio track and clip");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
             "one undo did not remove the duplicate audio track and clip");
  REQUIRE_OK(reopened.value()->saving().save(), "save after audio import undo failed");

  auto undo_session = application::ProjectSession::create(directory.path() / "undo-song");
  REQUIRE_OK(undo_session, "audio import undo fixture setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(undo_session.value()->imports().import_wav_audio(descriptor.get()),
               "audio import undo fixture failed");
  }
  REQUIRE_OK(undo_session.value()->history().undo(),
             "one undo did not remove source, track and clip together");
  REQUIRE_TRUE(undo_session.value()->snapshot().sources.empty(),
               "one undo did not remove source, track and clip together");
  REQUIRE_TRUE(undo_session.value()->snapshot().tracks.empty(),
               "one undo did not remove source, track and clip together");
}

bool fails_parent_flush(persistence::SaveStage stage) {
  return stage == persistence::SaveStage::creation_published;
}

// #226: once the new folder is renamed into place, a failed flush of its
// parent leaves the project created, with a storage warning, not an error.
void creation_reports_an_uncertain_folder_flush() {
  Directory directory;
  const auto root = directory.path() / "song";
  application::ProjectSession::CreateOptions options;
  options.fault = fails_parent_flush;
  auto created = application::ProjectSession::create(root, options);
  REQUIRE_OK(created, "a project renamed into place was reported as not created");
  REQUIRE_TRUE(created.value()->creation_warning().has_value(),
               "the failed parent flush was not reported");
  created.value().reset();
  REQUIRE_OK(application::ProjectSession::open(root), "the created project does not open");
}

void wav_import_rejects_rate_mismatch_without_model_edit() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "rate rejection fixture setup failed");
  const auto mismatched = make_wav(directory.path(), 44'117);
  const auto before = session.value()->snapshot().revision;
  FileDescriptor descriptor(mismatched);
  auto rejected = session.value()->imports().import_wav_audio(descriptor.get());
  REQUIRE_TRUE(!rejected, "rate-mismatched WAV was silently placed on the project timeline");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
             "rate-mismatched WAV was silently placed on the project timeline");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "rate-mismatched WAV was silently placed on the project timeline");
  REQUIRE_TRUE(session.value()->snapshot().tracks.empty(),
               "rate-mismatched WAV was silently placed on the project timeline");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "rate-mismatched WAV was silently placed on the project timeline");
}

void wav_import_converts_source_duration_and_reopens() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path(), 44'100, {44'100});
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "converted import fixture setup failed");
  FileDescriptor descriptor(source);
  const auto supported = media::source_rate_supported(44'100, 48'000);
  auto imported = session.value()->imports().import_wav_audio(descriptor.get());
  if (!supported) {
    REQUIRE_TRUE(!imported, "converter-OFF import published an asset");
    REQUIRE_EQ(imported.error().code, foundation::ErrorCode::unsupported,
               "converter-OFF import published an asset");
    REQUIRE_TRUE(session.value()->snapshot().tracks.empty(),
                 "converter-OFF import published an asset");
    REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
                 "converter-OFF import published an asset");
    return;
  }
  REQUIRE_TRUE(imported, "converted managed WAV import failed");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1, "converted managed WAV import failed");
  const auto& snapshot = session.value()->snapshot();
  REQUIRE_EQ(snapshot.sources[0].sample_rate, 44'100,
             "import did not retain source frames and project placement separately");
  REQUIRE_EQ(snapshot.sources[0].frames, 44'100,
             "import did not retain source frames and project placement separately");
  REQUIRE_EQ(snapshot.tracks[0].clips[0].source_range.end.value, 44'100,
             "import did not retain source frames and project placement separately");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(snapshot.tracks[0].clips[0].placement).end.value,
             48'000, "import did not retain source frames and project placement separately");
  session.value().reset();
  fs::remove(source);
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "converted managed WAV did not reopen without external source");
  REQUIRE_EQ(reopened.value()->snapshot().sources[0].frames, 44'100,
             "converted managed WAV did not reopen without external source");
  REQUIRE_EQ(
      std::get<project::ElapsedInterval>(reopened.value()->snapshot().tracks[0].clips[0].placement)
          .end.value,
      48'000, "converted managed WAV did not reopen without external source");
}

// Dragged onto another audio track: the same clip, one undo step.
void audio_clip_moves_to_another_track() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "track move session setup failed");
  for (int track = 0; track < 2; ++track) {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "track move import failed");
  }
  const auto& before = session.value()->snapshot();
  const auto from = before.tracks.front();
  const auto to = before.tracks.back();
  const auto clip = from.clips.front();
  const auto revision = before.revision;
  REQUIRE_TRUE(session.value()
                   ->audio_clips()
                   .move_audio_clip_to_track(clip.id, to.id, {timeline::ticks_per_quarter})
                   .has_value(),
               "moving a clip to another track was not one edit");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "moving a clip to another track was not one edit");
  const auto& after = session.value()->snapshot();
  const auto& source_track = *std::ranges::find(after.tracks, from.id, &project::Track::id);
  const auto& target_track = *std::ranges::find(after.tracks, to.id, &project::Track::id);
  const auto moved = std::ranges::find(target_track.clips, clip.id, &project::Clip::id);
  REQUIRE_TRUE(source_track.clips.empty(), "the clip did not arrive on the other track intact");
  REQUIRE_NE(moved, target_track.clips.end(), "the clip did not arrive on the other track intact");
  REQUIRE_EQ(moved->source_range, clip.source_range,
             "the clip did not arrive on the other track intact");
  REQUIRE_EQ(moved->source, clip.source, "the clip did not arrive on the other track intact");
  REQUIRE_OK(session.value()->history().undo(), "moving a clip to another track did not undo");
  REQUIRE_NE(std::ranges::find(session.value()->snapshot().tracks.front().clips, clip.id,
                               &project::Clip::id),
             session.value()->snapshot().tracks.front().clips.end(),
             "moving a clip to another track did not undo");
}

void move_before_zero_adds_a_bar_before_bar_one() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio move session setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio move import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  // Before zero adds a bar before bar 1 (ADR-0053), as one edit.
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_OK(session.value()->audio_clips().move_audio_clip(clip, {-1}),
             "a move before zero did not add a bar before bar 1 in one step");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "a move before zero did not add a bar before bar 1 in one step");
  REQUIRE_EQ(session.value()->snapshot().lead_bars, 1,
             "a move before zero did not add a bar before bar 1 in one step");
  REQUIRE_OK(session.value()->history().undo(),
             "a move before zero did not add a bar before bar 1 in one step");
  REQUIRE_EQ(session.value()->snapshot().lead_bars, 0,
             "a move before zero did not add a bar before bar 1 in one step");
}

void audio_move_preserves_elapsed_duration_and_reopens() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio move session setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio move import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  const auto source_range = session.value()->snapshot().tracks.front().clips.front().source_range;
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->audio_clips().move_audio_clip(
                   project::ClipId::from_bits({0, 999}).value(), {0}),
               "invalid audio movement changed project state");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid audio movement changed project state");
  REQUIRE_OK(session.value()->audio_clips().move_audio_clip(clip, {timeline::ticks_per_quarter}),
             "audio move to second beat failed");
  const auto& moved = session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "audio move changed duration, source range or revision count");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(moved.placement),
             (project::ElapsedInterval{{24'000}, {24'004}}),
             "audio move changed duration, source range or revision count");
  REQUIRE_EQ(moved.source_range, source_range,
             "audio move changed duration, source range or revision count");
  REQUIRE_OK(session.value()->audio_clips().move_audio_clip(clip, {timeline::ticks_per_quarter}),
             "same-position audio move created an undo step");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "same-position audio move created an undo step");
  REQUIRE_OK(session.value()->saving().save(), "moved audio save failed");
  auto reader = persistence::ProjectStore::open_reader(root);
  auto reopened = reader
                      ? reader.value().load_current()
                      : foundation::Result<persistence::LoadedGeneration>::failure(reader.error());
  REQUIRE_TRUE(reopened, "moved audio did not reopen at elapsed position");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 reopened.value().snapshot.tracks.front().clips.front().placement),
             (project::ElapsedInterval{{24'000}, {24'004}}),
             "moved audio did not reopen at elapsed position");
  REQUIRE_OK(session.value()->history().undo(),
             "one audio move undo did not restore original placement");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 session.value()->snapshot().tracks.front().clips.front().placement),
             (project::ElapsedInterval{{0}, {4}}),
             "one audio move undo did not restore original placement");
}

void audio_duplicate_is_atomic_and_preserves_source() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio duplicate session setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio duplicate import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->audio_clips().duplicate_audio_clip(original.id, {0}),
               "invalid or overlapping audio duplicate changed the project");
  REQUIRE_TRUE(!session.value()->audio_clips().duplicate_audio_clip(original.id, {-1}),
               "invalid or overlapping audio duplicate changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid or overlapping audio duplicate changed the project");
  auto duplicate = session.value()->audio_clips().duplicate_audio_clip(
      original.id, {timeline::ticks_per_quarter});
  REQUIRE_OK(duplicate, "audio duplicate to second beat failed");
  const auto& state = session.value()->snapshot();
  REQUIRE_EQ(state.revision, before + 1,
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(state.sources.size(), 1,
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(state.tracks.front().clips.size(), 2,
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(state.tracks.front().clips.back().id, duplicate.value(),
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(state.tracks.front().clips.back().source, original.source,
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(state.tracks.front().clips.back().source_range, original.source_range,
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(state.tracks.front().clips.back().placement),
             (project::ElapsedInterval{{24'000}, {24'004}}),
             "audio duplicate changed source, duration, placement or revision count");
  REQUIRE_OK(session.value()->saving().save(), "audio duplicate save failed");
  auto reader = persistence::ProjectStore::open_reader(root);
  auto loaded = reader ? reader.value().load_current()
                       : foundation::Result<persistence::LoadedGeneration>::failure(reader.error());
  REQUIRE_TRUE(loaded, "audio duplicate did not reopen");
  REQUIRE_EQ(loaded.value().snapshot.tracks.front().clips.size(), 2,
             "audio duplicate did not reopen");
  REQUIRE_OK(session.value()->history().undo(), "one undo did not remove audio duplicate");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.size(), 1,
             "one undo did not remove audio duplicate");
}

void adjacent_audio_copy_and_delete_are_reversible() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "adjacent clip fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path()));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "adjacent clip fixture import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const project::ClipFades fades{0.25, 0.25, project::FadeCurve::linear,
                                 project::FadeCurve::smoothstep};
  REQUIRE_OK(session.value()->audio_clips().set_audio_clip_fades(original.id, fades),
             "adjacent clip fixture fade failed");
  const auto before = session.value()->snapshot().revision;
  auto copy = session.value()->audio_clips().duplicate_audio_clip_after(original.id);
  REQUIRE_TRUE(copy, "adjacent duplicate did not create one atomic revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "adjacent duplicate did not create one atomic revision");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.size(), 2,
             "adjacent duplicate did not create one atomic revision");
  const auto& duplicated = session.value()->snapshot().tracks.front().clips.back();
  REQUIRE_EQ(duplicated.id, copy.value(),
             "adjacent duplicate lost clip settings, source identity or exact sample boundary");
  REQUIRE_EQ(duplicated.source, original.source,
             "adjacent duplicate lost clip settings, source identity or exact sample boundary");
  REQUIRE_EQ(duplicated.source_range, original.source_range,
             "adjacent duplicate lost clip settings, source identity or exact sample boundary");
  REQUIRE_EQ(duplicated.fades, fades,
             "adjacent duplicate lost clip settings, source identity or exact sample boundary");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(duplicated.placement),
             (project::ElapsedInterval{{4}, {8}}),
             "adjacent duplicate lost clip settings, source identity or exact sample boundary");
  REQUIRE_TRUE(!session.value()->audio_clips().duplicate_audio_clip_after(original.id),
               "occupied adjacent position changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "occupied adjacent position changed the project");
  REQUIRE_TRUE(session.value()->audio_clips().remove_audio_clip(original.id),
               "delete removed the media source or wrong clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.size(), 1,
             "delete removed the media source or wrong clip");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "delete removed the media source or wrong clip");
  REQUIRE_TRUE(session.value()->history().undo(), "one undo did not restore deleted clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.size(), 2,
             "one undo did not restore deleted clip");
  REQUIRE_OK(session.value()->saving().save(), "adjacent clip save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "adjacent copy and fade settings did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.size(), 2,
             "adjacent copy and fade settings did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.back().fades, fades,
             "adjacent copy and fade settings did not reopen");
  REQUIRE_TRUE(reopened.value()->audio_clips().remove_audio_clip(copy.value()),
               "reopened duplicate could not be deleted without losing its source");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "reopened duplicate could not be deleted without losing its source");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.size(), 1,
             "reopened duplicate could not be deleted without losing its source");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
             "reopened duplicate could not be deleted without losing its source");
}

void audio_copy_reidentifies_clip_automation() {
  Directory directory;
  const auto root = directory.path() / "automated-audio-copy";
  auto session = application::ProjectSession::create(root);
  FileDescriptor source(make_wav(directory.path()));
  REQUIRE_TRUE(session, "copy automation import failed");
  REQUIRE_TRUE(session.value()->imports().import_wav_audio(source.get()),
               "copy automation import failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  auto bus = session.value()->tracks().add_bus_track(project::TrackKind::return_bus);
  auto send = bus ? session.value()->routing().add_send(track, bus.value(), 0.5)
                  : foundation::Result<project::SendId>::failure(bus.error());
  REQUIRE_TRUE(send, "copy automation source setup failed");
  REQUIRE_TRUE(
      session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 0, 0.5),
      "copy automation source setup failed");
  REQUIRE_TRUE(
      session.value()->parameter_automation().set_send_gain_automation_point(send.value(), 1, 0.75),
      "copy automation source setup failed");
  REQUIRE_TRUE(session.value()->saving().save(), "copy automation source setup failed");
  const auto original_id = session.value()->snapshot().tracks.front().clips.front().id;
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "copy automation writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "copy automation load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    auto& audio = snapshot.tracks.front();
    auto lane = audio.automation.front();
    audio.automation.clear();
    lane.timebase = project::AutomationTimebase::elapsed;
    lane.points[0].coordinate = 1;
    lane.points[1].coordinate = 2;
    audio.clips.front().automation.push_back(lane);
    ++snapshot.revision;
    auto saved = writer.value().save(snapshot, persistence::SaveKind::explicit_save);
    REQUIRE_OK(saved, saved ? "clip automation fixture save failed" : saved.error().message);
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "automated audio clip did not reopen");
  const auto before = reopened.value()->snapshot().revision;
  auto placed = reopened.value()->audio_clips().duplicate_audio_clip(original_id,
                                                                     {timeline::ticks_per_quarter});
  auto adjacent = reopened.value()->audio_clips().duplicate_audio_clip_after(original_id);
  REQUIRE_TRUE(placed, "automated audio duplicate failed or made extra revisions");
  REQUIRE_TRUE(adjacent, "automated audio duplicate failed or made extra revisions");
  REQUIRE_EQ(reopened.value()->snapshot().revision, before + 2,
             "automated audio duplicate failed or made extra revisions");
  const auto& clips = reopened.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.size(), 3, "automated audio duplication lost a clip");
  const auto original = clips[0].automation.front();
  const auto first = clips[1].automation.front();
  const auto second = clips[2].automation.front();
  REQUIRE_NE(original.id, first.id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(original.id, second.id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(first.id, second.id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(original.points[0].id, first.points[0].id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(original.points[0].id, second.points[0].id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(first.points[0].id, second.points[0].id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_NE(first.points[1].id, second.points[1].id,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_EQ(first.points[0].coordinate, original.points[0].coordinate,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_EQ(first.points[1].value, original.points[1].value,
             "automated clip copies reused identities or lost envelope settings");
  REQUIRE_EQ(second.timebase, original.timebase,
             "automated clip copies reused identities or lost envelope settings");
  const auto copied_lane = first.id;
  REQUIRE_TRUE(reopened.value()->automation().toggle_automation_lane_timebase(copied_lane),
               "editing copied automation changed the original clip");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips[0].automation.front(), original,
             "editing copied automation changed the original clip");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips[1].automation.front().timebase,
             project::AutomationTimebase::musical,
             "editing copied automation changed the original clip");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "copy automation edit or adjacent duplication did not undo");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips[1].automation.front(), first,
             "copy automation edit or adjacent duplication did not undo");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "copy automation edit or adjacent duplication did not undo");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.size(), 2,
             "copy automation edit or adjacent duplication did not undo");
  auto recreated = reopened.value()->audio_clips().duplicate_audio_clip_after(original_id);
  REQUIRE_TRUE(recreated, "undo and recreate reused retired automation IDs or failed to save");
  REQUIRE_NE(reopened.value()->snapshot().tracks.front().clips[2].automation.front().id, second.id,
             "undo and recreate reused retired automation IDs or failed to save");
  REQUIRE_NE(reopened.value()->snapshot().tracks.front().clips[2].automation.front().points[0].id,
             second.points[0].id,
             "undo and recreate reused retired automation IDs or failed to save");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "undo and recreate reused retired automation IDs or failed to save");
  reopened.value().reset();
  auto saved = application::ProjectSession::open(root);
  REQUIRE_TRUE(saved, "copied automation identities did not survive reopen");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().clips.size(), 3,
             "copied automation identities did not survive reopen");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().clips[1].automation.front().id, copied_lane,
             "copied automation identities did not survive reopen");
}

void midi_copy_retains_pattern_and_uses_fresh_note_ids() {
  Directory directory;
  const auto root = directory.path() / "midi-copy";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI copy setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI copy setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}),
             "MIDI copy clip setup failed");
  const auto original_id = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_TRUE(
      session.value()->notes().add_note(original_id, {timeline::ticks_per_quarter / 4}, 64),
      "MIDI copy note setup failed");
  REQUIRE_TRUE(session.value()->saving().save(), "MIDI copy note setup failed");
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "MIDI loop fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "MIDI loop fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    auto& clip = snapshot.tracks.front().midi_clips.front();
    clip.pattern_length = {timeline::ticks_per_quarter};
    clip.loop = true;
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "MIDI loop fixture save failed");
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "MIDI loop fixture did not reopen");
  const auto before = reopened.value()->snapshot().revision;
  const auto original = reopened.value()->snapshot().tracks.front().midi_clips.front();
  auto duplicated = reopened.value()->midi_clips().duplicate_midi_clip_after(original_id);
  REQUIRE_TRUE(duplicated, "MIDI duplicate did not publish one revision");
  REQUIRE_EQ(reopened.value()->snapshot().revision, before + 1,
             "MIDI duplicate did not publish one revision");
  const auto& clips = reopened.value()->snapshot().tracks.front().midi_clips;
  REQUIRE_EQ(clips.size(), 2, "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(clips.back().id, duplicated.value(),
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(clips.back().pattern_length, original.pattern_length,
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_TRUE(clips.back().loop,
               "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(clips.back().notes.size(), 1,
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_NE(clips.back().notes.front().id, original.notes.front().id,
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(clips.back().notes.front().start, original.notes.front().start,
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(clips.back().notes.front().end, original.notes.front().end,
             "MIDI duplicate changed note timing, loop settings or destination");
  REQUIRE_EQ(std::get<project::MusicalInterval>(clips.back().placement),
             (project::MusicalInterval{{4 * timeline::ticks_per_quarter},
                                       {8 * timeline::ticks_per_quarter}}),
             "MIDI duplicate changed note timing, loop settings or destination");
  const auto first_note_id = clips.back().notes.front().id;
  const auto occupied_revision = reopened.value()->snapshot().revision;
  REQUIRE_TRUE(!reopened.value()->midi_clips().duplicate_midi_clip_after(original_id),
               "occupied MIDI destination changed the project");
  REQUIRE_EQ(reopened.value()->snapshot().revision, occupied_revision,
             "occupied MIDI destination changed the project");
  REQUIRE_TRUE(reopened.value()->history().undo(),
               "MIDI duplicate undo did not remove only the copy");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.size(), 1,
             "MIDI duplicate undo did not remove only the copy");
  auto recreated = reopened.value()->midi_clips().duplicate_midi_clip_after(original_id);
  REQUIRE_TRUE(recreated, "recreated MIDI clip reused a retired note ID or failed to save");
  REQUIRE_NE(reopened.value()->snapshot().tracks.front().midi_clips.back().notes.front().id,
             first_note_id, "recreated MIDI clip reused a retired note ID or failed to save");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "recreated MIDI clip reused a retired note ID or failed to save");
  reopened.value().reset();
  auto saved = application::ProjectSession::open(root);
  REQUIRE_TRUE(saved, "MIDI duplicate did not preserve loop and fresh note IDs on reopen");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.size(), 2,
             "MIDI duplicate did not preserve loop and fresh note IDs on reopen");
  REQUIRE_TRUE(saved.value()->snapshot().tracks.front().midi_clips.back().loop,
               "MIDI duplicate did not preserve loop and fresh note IDs on reopen");
  REQUIRE_NE(saved.value()->snapshot().tracks.front().midi_clips.back().notes.front().id,
             first_note_id, "MIDI duplicate did not preserve loop and fresh note IDs on reopen");
  const auto before_delete = saved.value()->snapshot().revision;
  REQUIRE_TRUE(
      !saved.value()->midi_clips().remove_midi_clip(project::ClipId::from_bits({99, 99}).value()),
      "missing MIDI delete changed the project");
  REQUIRE_EQ(saved.value()->snapshot().revision, before_delete,
             "missing MIDI delete changed the project");
  REQUIRE_TRUE(saved.value()->midi_clips().remove_midi_clip(original_id),
               "MIDI delete did not remove only the selected clip or restore its notes on undo");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.size(), 1,
             "MIDI delete did not remove only the selected clip or restore its notes on undo");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.front().id, recreated.value(),
             "MIDI delete did not remove only the selected clip or restore its notes on undo");
  REQUIRE_TRUE(saved.value()->history().undo(),
               "MIDI delete did not remove only the selected clip or restore its notes on undo");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.size(), 2,
             "MIDI delete did not remove only the selected clip or restore its notes on undo");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().midi_clips.front().notes.front().id,
             original.notes.front().id,
             "MIDI delete did not remove only the selected clip or restore its notes on undo");
}

void midi_split_divides_notes_pedals_and_undoes_in_one_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "midi-split");
  REQUIRE_TRUE(session, "MIDI split setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI split setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}),
             "MIDI split clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  constexpr auto quarter = timeline::ticks_per_quarter;
  REQUIRE_TRUE(session.value()->notes().add_note_with_duration(clip, {0}, {quarter / 2}, 60),
               "MIDI split notes or pedal setup failed");
  REQUIRE_TRUE(session.value()->notes().add_note_with_duration(clip, {quarter}, {2 * quarter}, 62),
               "MIDI split notes or pedal setup failed");
  REQUIRE_TRUE(
      session.value()->notes().add_note_with_duration(clip, {3 * quarter}, {quarter / 2}, 67),
      "MIDI split notes or pedal setup failed");
  REQUIRE_TRUE(session.value()->pedals().set_midi_pedal_lane(clip, project::MidiPedal::sustain,
                                                             {{{quarter / 2}, 1.0}}),
               "MIDI split notes or pedal setup failed");
  const auto original = session.value()->snapshot().tracks.front().midi_clips.front();
  const auto before = session.value()->snapshot().revision;

  auto split = session.value()->midi_clips().split_midi_clip_at_tick(clip, {2 * quarter});
  REQUIRE_TRUE(split, "MIDI split did not publish one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "MIDI split did not publish one revision");
  const auto& clips = session.value()->snapshot().tracks.front().midi_clips;
  REQUIRE_EQ(clips.size(), 2, "MIDI split did not add the right half");
  const auto& left = clips.front();
  const auto& right = clips.back();
  REQUIRE_EQ(left.id, clip, "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(right.id, split.value(), "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(std::get<project::MusicalInterval>(left.placement),
             (project::MusicalInterval{{0}, {2 * quarter}}),
             "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(std::get<project::MusicalInterval>(right.placement),
             (project::MusicalInterval{{2 * quarter}, {4 * quarter}}),
             "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(left.pattern_length.value, 2 * quarter,
             "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(right.pattern_length.value, 2 * quarter,
             "MIDI split halves have the wrong placement or pattern");
  REQUIRE_EQ(left.notes.size(), 2,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(left.notes[1].key, 62,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(left.notes[1].end.value, 2 * quarter,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(right.notes.size(), 1,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(right.notes.front().key, 67,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(right.notes.front().start.value, quarter,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_NE(right.notes.front().id, original.notes[2].id,
             "notes crossing the cut must end there and later notes move to the right half");
  REQUIRE_EQ(left.pedals, original.pedals,
             "a pedal held across the cut must go down again at the right half's start");
  REQUIRE_EQ(right.pedals.size(), 1,
             "a pedal held across the cut must go down again at the right half's start");
  REQUIRE_EQ(right.pedals.front().points, (std::vector<project::MidiPedalPoint>{{{0}, 1.0}}),
             "a pedal held across the cut must go down again at the right half's start");

  REQUIRE_TRUE(session.value()->history().undo(), "one undo must rejoin the MIDI clip exactly");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().midi_clips,
             std::vector<project::MidiClip>{original},
             "one undo must rejoin the MIDI clip exactly");
  const auto unchanged = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->midi_clips().split_midi_clip_at_tick(clip, {0}),
               "splits at the edges or of unknown clips must be rejected without changes");
  REQUIRE_TRUE(!session.value()->midi_clips().split_midi_clip_at_tick(clip, {4 * quarter}),
               "splits at the edges or of unknown clips must be rejected without changes");
  REQUIRE_TRUE(!session.value()->midi_clips().split_midi_clip_at_tick(
                   project::ClipId::from_bits({99, 99}).value(), {quarter}),
               "splits at the edges or of unknown clips must be rejected without changes");
  REQUIRE_EQ(session.value()->snapshot().revision, unchanged,
             "splits at the edges or of unknown clips must be rejected without changes");
}

void native_instrument_track_arrives_with_its_sound_in_one_undo_step() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "native-track");
  REQUIRE_OK(session, "native instrument track project setup failed");
  const auto before = session.value()->snapshot().revision;
  const auto track =
      session.value()->instruments().add_native_instrument_track("ember", std::size_t{1});
  REQUIRE_TRUE(track, "a native instrument track took more than one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "a native instrument track took more than one revision");
  const auto row = session.value()->tracks().track_index(track.value());
  REQUIRE_OK(row, "the new native instrument track has no row");
  const auto& source = session.value()->snapshot().tracks[*row].instrument_source;
  REQUIRE_TRUE(std::holds_alternative<project::NativeInstrumentPatch>(source),
               "the new track does not play the chosen engine");
  REQUIRE_EQ(std::get<project::NativeInstrumentPatch>(source).engine, "ember",
             "the new track does not play the chosen engine");
  REQUIRE_TRUE(
      !session.value()->instruments().add_native_instrument_track("no-such-engine", std::nullopt),
      "an unknown engine must be rejected without changes");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "an unknown engine must be rejected without changes");
  REQUIRE_TRUE(session.value()->history().undo(),
               "one undo must remove the track and its sound together");
  REQUIRE_TRUE(session.value()->snapshot().tracks.empty(),
               "one undo must remove the track and its sound together");
}

void project_key_is_set_cleared_and_undone() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "project-key");
  REQUIRE_OK(session, "a new project has no key and nothing to suggest");
  REQUIRE_TRUE(!session.value()->snapshot().key, "a new project has no key and nothing to suggest");
  REQUIRE_TRUE(!session.value()->song().suggested_project_key(),
               "a new project has no key and nothing to suggest");
  const project::MusicalKey a_minor{9, project::KeyMode::minor};
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->song().set_project_key(a_minor),
               "setting the key took other than one revision");
  REQUIRE_EQ(session.value()->snapshot().key, a_minor,
             "setting the key took other than one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "setting the key took other than one revision");
  REQUIRE_TRUE(!session.value()->song().set_project_key(a_minor),
               "an unchanged or invalid key must be rejected");
  REQUIRE_TRUE(
      !session.value()->song().set_project_key(project::MusicalKey{12, project::KeyMode::major}),
      "an unchanged or invalid key must be rejected");
  REQUIRE_TRUE(session.value()->song().set_project_key(std::nullopt), "clearing the key failed");
  REQUIRE_TRUE(!session.value()->snapshot().key, "clearing the key failed");
  REQUIRE_TRUE(session.value()->history().undo(), "undo must restore each earlier key");
  REQUIRE_EQ(session.value()->snapshot().key, a_minor, "undo must restore each earlier key");
  REQUIRE_TRUE(session.value()->history().undo(), "undo must restore each earlier key");
  REQUIRE_TRUE(!session.value()->snapshot().key, "undo must restore each earlier key");
}

void presentation_edits_keep_playback_current() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "audible-revision");
  REQUIRE_TRUE(session, "audible revision setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "audible revision setup failed");
  const auto& state = [&]() -> const project::Snapshot& { return session.value()->snapshot(); };
  const auto track = state().tracks.front().id;
  const auto audible = state().audible_revision;
  REQUIRE_EQ(audible, state().revision, "an audible edit advances the audible revision");
  REQUIRE_TRUE(session.value()->tracks().set_track_name(track, "Lead"),
               "presentation edits failed");
  REQUIRE_TRUE(session.value()->tracks().set_track_color(track, 0x5F'E0'BF),
               "presentation edits failed");
  REQUIRE_EQ(state().revision, audible + 2, "names and colours must not need new playback");
  REQUIRE_EQ(state().audible_revision, audible, "names and colours must not need new playback");
  REQUIRE_TRUE(session.value()->history().undo(),
               "undoing a presentation edit must not need new playback either");
  REQUIRE_EQ(state().audible_revision, audible,
             "undoing a presentation edit must not need new playback either");
  REQUIRE_TRUE(session.value()->tracks().set_track_level_pan(track, 0.5, 0.0),
               "a level change is audible");
  REQUIRE_EQ(state().audible_revision, state().revision, "a level change is audible");
  // Knock's pads tune to the key, so a key change is heard, and so is its undo.
  require(
      session.value()->song().set_project_key(project::MusicalKey{0, project::KeyMode::major}) &&
          state().audible_revision == state().revision,
      "a key change is audible");
  const auto keyed = state().audible_revision;
  REQUIRE_TRUE(session.value()->history().undo(), "undoing a key change is audible");
  REQUIRE_GT(state().audible_revision, keyed, "undoing a key change is audible");
  REQUIRE_TRUE(!state().key, "undoing a key change is audible");
}

void project_key_suggestion_follows_most_of_the_audio() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "key-suggestion");
  REQUIRE_OK(session, "key suggestion project setup failed");
  for (const auto frames : {48'000U, 24'000U, 12'000U}) {
    FileDescriptor input(make_wav(directory.path(), 48'000, {frames}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
               "key suggestion import failed");
  }
  const auto& sources = session.value()->snapshot().sources;
  REQUIRE_EQ(sources.size(), 3, "unreviewed audio must not suggest a key");
  REQUIRE_TRUE(!session.value()->song().suggested_project_key(),
               "unreviewed audio must not suggest a key");
  const project::MusicalKey a_minor{9, project::KeyMode::minor};
  const project::MusicalKey c_major{0, project::KeyMode::major};
  REQUIRE_TRUE(session.value()->source_analysis().set_source_key_correction(sources[0].id, c_major),
               "one reviewed source suggests its key");
  REQUIRE_EQ(session.value()->song().suggested_project_key(), c_major,
             "one reviewed source suggests its key");
  REQUIRE_TRUE(session.value()->source_analysis().set_source_key_correction(sources[1].id, a_minor),
               "a tie keeps the first reviewed key");
  REQUIRE_EQ(session.value()->song().suggested_project_key(), c_major,
             "a tie keeps the first reviewed key");
  REQUIRE_TRUE(session.value()->source_analysis().set_source_key_correction(sources[2].id, a_minor),
               "the key most of the audio is in wins");
  REQUIRE_EQ(session.value()->song().suggested_project_key(), a_minor,
             "the key most of the audio is in wins");
}

void midi_copy_preserves_elapsed_anchor() {
  Directory directory;
  const auto root = directory.path() / "elapsed-midi-copy";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "elapsed MIDI setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "elapsed MIDI setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "elapsed MIDI clip setup failed");
  REQUIRE_TRUE(session.value()->saving().save(), "elapsed MIDI clip setup failed");
  const auto clip_id = session.value()->snapshot().tracks.front().midi_clips.front().id;
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "elapsed MIDI writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "elapsed MIDI fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.tracks.front().midi_clips.front().placement = project::ElapsedInterval{{0}, {96'000}};
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "elapsed MIDI fixture save failed");
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "elapsed MIDI fixture did not reopen");
  auto copy = reopened.value()->midi_clips().duplicate_midi_clip_after(clip_id);
  REQUIRE_TRUE(copy, "elapsed MIDI duplicate moved from its exact sample boundary");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.size(), 2,
             "elapsed MIDI duplicate moved from its exact sample boundary");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 reopened.value()->snapshot().tracks.front().midi_clips.back().placement),
             (project::ElapsedInterval{{96'000}, {192'000}}),
             "elapsed MIDI duplicate moved from its exact sample boundary");
}

void midi_timebase_switch_preserves_events_and_roundtrips_samples() {
  Directory directory;
  const auto root = directory.path() / "midi-timebase";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI timebase setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI timebase setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}),
             "MIDI timebase clip setup failed");
  const auto original = session.value()->snapshot().tracks.front().midi_clips.front();
  REQUIRE_OK(session.value()->notes().add_note(original.id, {timeline::ticks_per_quarter / 4}, 60),
             "MIDI timebase note setup failed");
  const auto prepared = session.value()->snapshot().tracks.front().midi_clips.front();
  const auto event_samples = [](const project::Snapshot& snapshot) {
    auto plan = engine::compile_midi_events(snapshot);
    REQUIRE_TRUE(plan, "MIDI timebase event plan unavailable");
    REQUIRE_EQ(plan.value().instruments().size(), 1, "MIDI timebase event plan unavailable");
    REQUIRE_EQ(plan.value().instruments().front().events().size(), 2,
               "MIDI timebase event plan unavailable");
    const auto& events = plan.value().instruments().front().events();
    return std::array{events.front().sample.value, events.back().sample.value};
  };
  const auto before_events = event_samples(session.value()->snapshot());
  const auto before_revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->midi_clips().toggle_midi_clip_timebase(original.id),
               "MIDI timebase did not publish one edit");
  REQUIRE_EQ(session.value()->snapshot().revision, before_revision + 1,
             "MIDI timebase did not publish one edit");
  const auto elapsed = session.value()->snapshot().tracks.front().midi_clips.front();
  REQUIRE_EQ(std::get<project::ElapsedInterval>(elapsed.placement),
             (project::ElapsedInterval{{0}, {96'000}}),
             "MIDI timebase moved boundaries, notes or scheduled events");
  REQUIRE_EQ(elapsed.notes, prepared.notes,
             "MIDI timebase moved boundaries, notes or scheduled events");
  REQUIRE_EQ(event_samples(session.value()->snapshot()), before_events,
             "MIDI timebase moved boundaries, notes or scheduled events");
  REQUIRE_TRUE(session.value()->history().undo(), "MIDI timebase did not undo, redo or save");
  REQUIRE_TRUE(std::holds_alternative<project::MusicalInterval>(
                   session.value()->snapshot().tracks.front().midi_clips.front().placement),
               "MIDI timebase did not undo, redo or save");
  REQUIRE_TRUE(session.value()->history().redo(), "MIDI timebase did not undo, redo or save");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().midi_clips.front(), elapsed,
             "MIDI timebase did not undo, redo or save");
  REQUIRE_TRUE(session.value()->saving().save(), "MIDI timebase did not undo, redo or save");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "MIDI timebase reopen or exact return to beats failed");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front(), elapsed,
             "MIDI timebase reopen or exact return to beats failed");
  REQUIRE_TRUE(reopened.value()->midi_clips().toggle_midi_clip_timebase(original.id),
               "MIDI timebase reopen or exact return to beats failed");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 reopened.value()->snapshot().tracks.front().midi_clips.front().placement),
             std::get<project::MusicalInterval>(original.placement),
             "MIDI timebase reopen or exact return to beats failed");
  REQUIRE_EQ(event_samples(reopened.value()->snapshot()), before_events,
             "MIDI timebase reopen or exact return to beats failed");
  reopened.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "offset MIDI timebase writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "offset MIDI timebase load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.tracks.front().midi_clips.front().placement = project::ElapsedInterval{{1}, {96'001}};
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "offset MIDI timebase fixture save failed");
  }
  auto offset = application::ProjectSession::open(root);
  REQUIRE_OK(offset, "offset MIDI timebase fixture did not reopen");
  const auto offset_events = event_samples(offset.value()->snapshot());
  const auto revision = offset.value()->snapshot().revision;
  REQUIRE_TRUE(!offset.value()->midi_clips().toggle_midi_clip_timebase(
                   project::ClipId::from_bits({99, 99}).value()),
               "missing MIDI timebase target changed the project");
  REQUIRE_EQ(offset.value()->snapshot().revision, revision,
             "missing MIDI timebase target changed the project");
  REQUIRE_TRUE(offset.value()->midi_clips().toggle_midi_clip_timebase(original.id),
               "one-sample offset MIDI timebase moved scheduled notes");
  REQUIRE_EQ(event_samples(offset.value()->snapshot()), offset_events,
             "one-sample offset MIDI timebase moved scheduled notes");
  const auto& placement = std::get<project::MusicalInterval>(
      offset.value()->snapshot().tracks.front().midi_clips.front().placement);
  const auto sample_start = offset.value()->snapshot().tempo.tick_to_sample(
      placement.start, offset.value()->snapshot().sample_rate);
  const auto sample_end = offset.value()->snapshot().tempo.tick_to_sample(
      placement.end, offset.value()->snapshot().sample_rate);
  REQUIRE_TRUE(sample_start, "one-sample offset MIDI boundaries did not round-trip");
  REQUIRE_TRUE(sample_end, "one-sample offset MIDI boundaries did not round-trip");
  REQUIRE_EQ(sample_start.value().value, 1, "one-sample offset MIDI boundaries did not round-trip");
  REQUIRE_EQ(sample_end.value().value, 96'001,
             "one-sample offset MIDI boundaries did not round-trip");
}

void midi_note_removal_is_undoable_and_durable() {
  Directory directory;
  const auto root = directory.path() / "midi-note-removal";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI note removal setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI note removal setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}), "MIDI clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session.value()->notes().add_note(clip, {0}, 60), "MIDI note setup failed");
  const auto note = session.value()->snapshot().tracks.front().midi_clips.front().notes.front();
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(
      !session.value()->notes().remove_note(clip, project::NoteId::from_bits({99, 99}).value()),
      "missing MIDI note or clip changed the project");
  REQUIRE_TRUE(
      !session.value()->notes().remove_note(project::ClipId::from_bits({99, 99}).value(), note.id),
      "missing MIDI note or clip changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "missing MIDI note or clip changed the project");
  REQUIRE_TRUE(session.value()->notes().remove_note(clip, note.id),
               "MIDI note removal did not publish one edit");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "MIDI note removal did not publish one edit");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().midi_clips.front().notes.empty(),
               "MIDI note removal did not publish one edit");
  REQUIRE_TRUE(session.value()->history().undo(), "MIDI note removal undo, redo or save failed");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().midi_clips.front().notes, std::vector{note},
             "MIDI note removal undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(), "MIDI note removal undo, redo or save failed");
  REQUIRE_TRUE(session.value()->snapshot().tracks.front().midi_clips.front().notes.empty(),
               "MIDI note removal undo, redo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "MIDI note removal undo, redo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "removed MIDI note reappeared on reopen");
  REQUIRE_TRUE(reopened.value()->snapshot().tracks.front().midi_clips.front().notes.empty(),
               "removed MIDI note reappeared on reopen");
}

// A Ctrl-drag copies notes by an offset as one undo step: the originals
// stay, copies get fresh ids, and a copy may not leave the pattern or land on
// any existing note.
void midi_note_copies_land_at_the_drop() {
  Directory directory;
  const auto root = directory.path() / "midi-note-copies";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "copy setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "copy setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}), "copy clip failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  constexpr auto beat = timeline::ticks_per_quarter;
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {0}, 60), "copy notes failed");
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {beat}, 64), "copy notes failed");
  const auto notes = [&] {
    return session.value()->snapshot().tracks.front().midi_clips.front().notes;
  };
  const std::vector<project::NoteId> pair{notes()[0].id, notes()[1].id};
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->notes().copy_notes_by(clip, pair, 0, 0),
               "an invalid or colliding copy changed the clip");
  REQUIRE_TRUE(!session.value()->notes().copy_notes_by(clip, pair, beat, 4),
               "an invalid or colliding copy changed the clip");
  REQUIRE_TRUE(!session.value()->notes().copy_notes_by(clip, pair, -beat, 0),
               "an invalid or colliding copy changed the clip");
  REQUIRE_TRUE(!session.value()->notes().copy_notes_by(clip, pair, 0, 100),
               "an invalid or colliding copy changed the clip");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "an invalid or colliding copy changed the clip");
  const auto copies = session.value()->notes().copy_notes_by(clip, pair, 2 * beat, 7);
  REQUIRE_TRUE(copies, "a copy did not add both notes in one step");
  REQUIRE_EQ(copies.value().size(), 2, "a copy did not add both notes in one step");
  REQUIRE_EQ(notes().size(), 4, "a copy did not add both notes in one step");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "a copy did not add both notes in one step");
  const auto all = notes();
  for (const auto& id : copies.value()) {
    REQUIRE_EQ(std::ranges::find(pair, id), pair.end(), "a copy reused an original's id");
    const auto copy = std::ranges::find(all, id, &project::MidiNote::id);
    REQUIRE_NE(copy, all.end(), "a copy did not land at the drop");
    REQUIRE_TRUE((copy->start.value == 2 * beat || copy->start.value == 3 * beat),
                 "a copy did not land at the drop");
    REQUIRE_TRUE((copy->key == 67 || copy->key == 71), "a copy did not land at the drop");
  }
  REQUIRE_TRUE(session.value()->history().undo(), "a copy did not undo in one step");
  REQUIRE_EQ(notes().size(), 2, "a copy did not undo in one step");
}

// A note selection moves and deletes as one undo step each; a move that
// would leave the pattern or land on a note left behind changes nothing.
void midi_note_sets_move_and_delete_in_one_step() {
  Directory directory;
  const auto root = directory.path() / "midi-note-sets";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "note set setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "note set setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}), "note set clip failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  constexpr auto beat = timeline::ticks_per_quarter;
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {0}, 60), "note set notes failed");
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {beat}, 64), "note set notes failed");
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {2 * beat}, 67), "note set notes failed");
  const auto notes = [&] {
    return session.value()->snapshot().tracks.front().midi_clips.front().notes;
  };
  const auto find = [&](std::int64_t start, int key) {
    const auto all = notes();
    return std::ranges::any_of(
        all, [&](const auto& n) { return n.start.value == start && n.key == key; });
  };
  const std::vector<project::NoteId> pair{notes()[0].id, notes()[1].id};
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->notes().move_notes(clip, pair, -beat, 0),
               "an invalid or colliding set move changed the clip");
  REQUIRE_TRUE(!session.value()->notes().move_notes(clip, pair, 4 * beat, 0),
               "an invalid or colliding set move changed the clip");
  REQUIRE_TRUE(!session.value()->notes().move_notes(clip, pair, 2 * beat, 7),
               "an invalid or colliding set move changed the clip");
  REQUIRE_TRUE(!session.value()->notes().move_notes(clip, pair, 0, 100),
               "an invalid or colliding set move changed the clip");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "an invalid or colliding set move changed the clip");
  // Onto each other's places is fine: only notes left behind block.
  require(session.value()->notes().move_notes(clip, pair, beat, 4).has_value() &&
              session.value()->snapshot().revision == revision + 1 && find(beat, 64) &&
              find(2 * beat, 68) && find(2 * beat, 67) && notes().size() == 3,
          "a set move did not shift every note together in one step");
  REQUIRE_TRUE(session.value()->history().undo(), "a set move did not undo in one step");
  REQUIRE_TRUE(find(0, 60), "a set move did not undo in one step");
  REQUIRE_TRUE(find(beat, 64), "a set move did not undo in one step");
  const std::vector<project::NoteId> all{notes()[0].id, notes()[1].id, notes()[2].id};
  REQUIRE_TRUE(
      !session.value()->notes().remove_notes(
          clip, std::vector<project::NoteId>{all[0], project::NoteId::from_bits({99, 99}).value()}),
      "a delete with an unknown note removed anything");
  REQUIRE_EQ(notes().size(), 3, "a delete with an unknown note removed anything");
  REQUIRE_OK(session.value()->notes().remove_notes(clip, all), "deleting a set left notes");
  REQUIRE_TRUE(notes().empty(), "deleting a set left notes");
  REQUIRE_TRUE(session.value()->history().undo(), "a set delete did not undo in one step");
  REQUIRE_EQ(notes().size(), 3, "a set delete did not undo in one step");
}

void midi_note_move_keeps_identity_duration_and_one_undo_step() {
  Directory directory;
  const auto root = directory.path() / "midi-note-move";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI note move setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI note move setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "MIDI note move clip setup failed");
  REQUIRE_TRUE(session.value()->notes().add_note(
                   session.value()->snapshot().tracks.front().midi_clips.front().id,
                   {timeline::ticks_per_quarter / 4}, 60),
               "MIDI note move clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session.value()->notes().add_note(clip, {2 * timeline::ticks_per_quarter / 4}, 61),
             "MIDI note move collision setup failed");
  const auto original = session.value()->snapshot().tracks.front().midi_clips.front().notes.front();
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->notes().move_note(clip, original.id,
                                                   {2 * timeline::ticks_per_quarter / 4}, 61),
               "invalid MIDI note move changed project state");
  REQUIRE_TRUE(
      !session.value()->notes().move_note(clip, original.id, {timeline::ticks_per_quarter * 4}, 60),
      "invalid MIDI note move changed project state");
  REQUIRE_TRUE(!session.value()->notes().move_note(
                   clip, project::NoteId::from_bits({99, 99}).value(), {0}, 60),
               "invalid MIDI note move changed project state");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "invalid MIDI note move changed project state");
  const auto moved_start = timeline::TickPosition{3 * timeline::ticks_per_quarter / 4};
  REQUIRE_OK(session.value()->notes().move_note(clip, original.id, moved_start, 64),
             "MIDI note move failed");
  const auto moved = session.value()->snapshot().tracks.front().midi_clips.front().notes.front();
  REQUIRE_EQ(moved.id, original.id, "MIDI note move changed identity, duration or velocity");
  REQUIRE_EQ(moved.start, moved_start, "MIDI note move changed identity, duration or velocity");
  REQUIRE_EQ(moved.key, 64, "MIDI note move changed identity, duration or velocity");
  REQUIRE_EQ(moved.end.value - moved.start.value, original.end.value - original.start.value,
             "MIDI note move changed identity, duration or velocity");
  REQUIRE_EQ(moved.velocity, original.velocity,
             "MIDI note move changed identity, duration or velocity");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "MIDI note move changed identity, duration or velocity");
  REQUIRE_TRUE(session.value()->notes().move_note(clip, original.id, moved_start, 64),
               "MIDI note move no-op, undo, redo or save failed");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "MIDI note move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().undo(),
               "MIDI note move no-op, undo, redo or save failed");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().midi_clips.front().notes.front(), original,
             "MIDI note move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(),
               "MIDI note move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "MIDI note move no-op, undo, redo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "moved MIDI note did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front().notes.front(), moved,
             "moved MIDI note did not reopen");
}

// Note-on and note-off samples of the first instrument's compiled events.
std::vector<std::int64_t> midi_event_samples(const project::Snapshot& state) {
  auto events = engine::compile_midi_events(state);
  REQUIRE_OK(events, "MIDI events did not compile");
  std::vector<std::int64_t> samples;
  if (!events.value().instruments().empty()) {
    for (const auto& event : events.value().instruments().front().events())
      samples.push_back(event.sample.value);
  }
  return samples;
}

void midi_resize_sets_length_and_keeps_notes_past_the_end() {
  Directory directory;
  const auto root = directory.path() / "midi-length-resize";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI resize setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI resize setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr auto bar = 4 * quarter;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "MIDI resize clip setup failed");
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {3 * bar}),
               "MIDI resize clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  const auto current = [&]() -> const project::MidiClip& {
    return session.value()->snapshot().tracks.front().midi_clips.front();
  };
  REQUIRE_OK(session.value()->notes().add_note(clip, {0}, 60), "a new MIDI clip must not loop");
  REQUIRE_TRUE(!current().loop, "a new MIDI clip must not loop");
  require(!session.value()->notes().add_note(clip, {bar}, 62),
          "a note after a one-bar clip's end was accepted");
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->midi_clips().resize_midi_clip(clip, {0}),
               "empty or colliding MIDI resize changed project state");
  REQUIRE_TRUE(!session.value()->midi_clips().resize_midi_clip(clip, {4 * bar}),
               "empty or colliding MIDI resize changed project state");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "empty or colliding MIDI resize changed project state");
  // Ends need not fall on a bar: the arrangement's grid decides.
  const auto beat_resize = session.value()->midi_clips().resize_midi_clip(clip, {bar + bar / 4});
  require(beat_resize.has_value() && current().pattern_length.value == bar + bar / 4,
          beat_resize ? "a beat-length MIDI resize set the wrong length"
                      : beat_resize.error().message.c_str());
  REQUIRE_TRUE(session.value()->history().undo(), "a beat-length MIDI resize did not undo");
  REQUIRE_EQ(current().pattern_length.value, bar, "a beat-length MIDI resize did not undo");
  const auto after_undo = session.value()->snapshot().revision;

  // Lengthening sets the clip's length and gives the piano roll the room.
  require(session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}).has_value() &&
              session.value()->snapshot().revision == after_undo + 1,
          "MIDI lengthen failed or took more than one revision");
  REQUIRE_TRUE(!current().loop,
               "lengthening a non-looping MIDI clip repeated it instead of lengthening it");
  REQUIRE_EQ(current().pattern_length.value, 3 * bar,
             "lengthening a non-looping MIDI clip repeated it instead of lengthening it");
  REQUIRE_EQ(std::get<project::MusicalInterval>(current().placement).end.value, 3 * bar,
             "lengthening a non-looping MIDI clip repeated it instead of lengthening it");
  REQUIRE_EQ(midi_event_samples(session.value()->snapshot()).size(), 2,
             "lengthening a non-looping MIDI clip repeated it instead of lengthening it");
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {2 * bar}, 62),
               "notes could not be written into the lengthened clip");
  REQUIRE_TRUE(
      session.value()->notes().add_note_with_duration(clip, {bar + 3 * quarter}, {2 * quarter}, 64),
      "notes could not be written into the lengthened clip");
  const auto long_clip = current();
  const auto all_notes = midi_event_samples(session.value()->snapshot());
  REQUIRE_EQ(all_notes.size(), 6, "the lengthened clip did not play all its notes");

  // Shortening keeps notes after the new end, silent; a note across it is cut.
  require(session.value()->midi_clips().resize_midi_clip(clip, {2 * bar}).has_value() &&
              current().pattern_length.value == 2 * bar && current().notes == long_clip.notes,
          "shortening a MIDI clip dropped notes after its end");
  auto events = engine::compile_midi_events(session.value()->snapshot());
  const auto cut = session.value()->snapshot().tempo.tick_to_sample(
      {2 * bar}, session.value()->snapshot().sample_rate);
  REQUIRE_TRUE(events, "a note after the shortened clip's end still played");
  REQUIRE_TRUE(cut, "a note after the shortened clip's end still played");
  REQUIRE_EQ(events.value().instruments().front().events().size(), 4,
             "a note after the shortened clip's end still played");
  const auto& played = events.value().instruments().front().events();
  REQUIRE_EQ(played.back().sample.value, cut.value().value,
             "a note crossing the shortened clip's end was not cut there");
  REQUIRE_EQ(played.back().kind, engine::MidiEventKind::note_off,
             "a note crossing the shortened clip's end was not cut there");
  REQUIRE_TRUE(played.back().boundary_crop,
               "a note crossing the shortened clip's end was not cut there");
  REQUIRE_EQ(played.back().key, 64, "a note crossing the shortened clip's end was not cut there");
  require(!session.value()->notes().add_note(clip, {2 * bar}, 65),
          "a note after the shortened clip's end was accepted");

  // Dragging the edge back out restores what was kept.
  require(session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}).has_value() &&
              current().notes == long_clip.notes &&
              current().pattern_length == long_clip.pattern_length &&
              midi_event_samples(session.value()->snapshot()) == all_notes,
          "lengthening the clip again did not restore its kept notes");
  const auto restored = current();
  REQUIRE_TRUE(session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}),
               "an unchanged MIDI resize published a revision");
  REQUIRE_EQ(session.value()->snapshot().revision, after_undo + 5,
             "an unchanged MIDI resize published a revision");
  REQUIRE_TRUE(session.value()->history().undo(), "MIDI resize undo, redo or save failed");
  REQUIRE_EQ(current().pattern_length.value, 2 * bar, "MIDI resize undo, redo or save failed");
  REQUIRE_EQ(std::get<project::MusicalInterval>(current().placement).end.value, 2 * bar,
             "MIDI resize undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(), "MIDI resize undo, redo or save failed");
  REQUIRE_EQ(current(), restored, "MIDI resize undo, redo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "MIDI resize undo, redo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "resized MIDI clip did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front(), restored,
             "resized MIDI clip did not reopen");
  REQUIRE_TRUE(reopened.value()->midi_clips().resize_midi_clip(clip, {bar}),
               "MIDI clip could not be shortened and saved with notes past its end");
  REQUIRE_TRUE(reopened.value()->saving().save(),
               "MIDI clip could not be shortened and saved with notes past its end");
  const auto shortened = reopened.value()->snapshot().tracks.front().midi_clips.front();
  reopened.value().reset();
  auto again = application::ProjectSession::open(root);
  REQUIRE_TRUE(again, "notes kept past a MIDI clip's end did not reopen silent");
  REQUIRE_EQ(again.value()->snapshot().tracks.front().midi_clips.front(), shortened,
             "notes kept past a MIDI clip's end did not reopen silent");
  REQUIRE_EQ(shortened.notes, long_clip.notes,
             "notes kept past a MIDI clip's end did not reopen silent");
  REQUIRE_EQ(midi_event_samples(again.value()->snapshot()).size(), 2,
             "notes kept past a MIDI clip's end did not reopen silent");
}

void midi_loop_switch_repeats_or_stops_in_one_undo_step() {
  Directory directory;
  const auto root = directory.path() / "midi-loop-switch";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "MIDI loop setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "MIDI loop setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  constexpr auto bar = 4 * timeline::ticks_per_quarter;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}),
             "MIDI loop clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  const auto current = [&]() -> const project::MidiClip& {
    return session.value()->snapshot().tracks.front().midi_clips.front();
  };
  REQUIRE_OK(session.value()->notes().add_note(clip, {0}, 60), "MIDI loop note setup failed");
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->midi_clips().set_midi_clip_loop(
                   project::ClipId::from_bits({99, 99}).value(), true),
               "a missing clip or unchanged loop switch changed the project");
  REQUIRE_TRUE(session.value()->midi_clips().set_midi_clip_loop(clip, false),
               "a missing clip or unchanged loop switch changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "a missing clip or unchanged loop switch changed the project");

  // Loop on: the right edge repeats the pattern; the pattern stays one bar.
  require(session.value()->midi_clips().set_midi_clip_loop(clip, true) &&
              session.value()->snapshot().revision == revision + 1 && current().loop &&
              current().pattern_length.value == bar,
          "switching loop on did not keep the clip's length as its pattern");
  REQUIRE_TRUE(session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}),
               "resizing a looping MIDI clip did not repeat its pattern three times");
  REQUIRE_TRUE(current().loop,
               "resizing a looping MIDI clip did not repeat its pattern three times");
  REQUIRE_EQ(current().pattern_length.value, bar,
             "resizing a looping MIDI clip did not repeat its pattern three times");
  REQUIRE_EQ(std::get<project::MusicalInterval>(current().placement).end.value, 3 * bar,
             "resizing a looping MIDI clip did not repeat its pattern three times");
  REQUIRE_EQ(midi_event_samples(session.value()->snapshot()).size(), 6,
             "resizing a looping MIDI clip did not repeat its pattern three times");
  const auto looped = current();

  // Loop off: the repeats stop, the first pass's notes stay, and the
  // pattern becomes the whole clip.
  require(session.value()->midi_clips().set_midi_clip_loop(clip, false) && !current().loop &&
              current().pattern_length.value == 3 * bar && current().notes == looped.notes &&
              current().placement == looped.placement &&
              midi_event_samples(session.value()->snapshot()).size() == 2,
          "switching loop off did not stop the repeats in place");
  const auto unlooped = current();
  REQUIRE_TRUE(session.value()->history().undo(),
               "the loop switch did not undo and redo as one step");
  REQUIRE_EQ(current(), looped, "the loop switch did not undo and redo as one step");
  REQUIRE_TRUE(session.value()->history().redo(),
               "the loop switch did not undo and redo as one step");
  REQUIRE_EQ(current(), unlooped, "the loop switch did not undo and redo as one step");

  // Loop on again: the whole visible clip is the pattern.
  require(session.value()->notes().add_note(clip, {2 * bar}, 62) &&
              session.value()->midi_clips().set_midi_clip_loop(clip, true) && current().loop &&
              current().pattern_length.value == 3 * bar &&
              session.value()->midi_clips().resize_midi_clip(clip, {6 * bar}) &&
              midi_event_samples(session.value()->snapshot()).size() == 8,
          "a looping three-bar clip did not repeat all of its notes");
  const auto saved = current();
  REQUIRE_OK(session.value()->saving().save(), "looping MIDI clip did not save");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "looping MIDI clip did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front(), saved,
             "looping MIDI clip did not reopen");
}

void elapsed_midi_resize_uses_tempo_map_without_moving_anchor() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "elapsed-midi-loop");
  constexpr auto bar = 4 * timeline::ticks_per_quarter;
  REQUIRE_TRUE(session, "elapsed MIDI resize tempo setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(),
               "elapsed MIDI resize tempo setup failed");
  REQUIRE_TRUE(session.value()->song().set_tempo_event({4 * bar}, 90.0, timeline::TempoShape::step),
               "elapsed MIDI resize tempo setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
      "elapsed MIDI resize tempo setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_OK(session.value()->midi_clips().add_midi_clip(track, {0}),
             "elapsed MIDI resize clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {0}, 60),
               "elapsed MIDI resize note or timebase setup failed");
  REQUIRE_TRUE(session.value()->midi_clips().toggle_midi_clip_timebase(clip),
               "elapsed MIDI resize note or timebase setup failed");
  const auto current = [&]() -> const project::MidiClip& {
    return session.value()->snapshot().tracks.front().midi_clips.front();
  };
  const auto original = current();
  const auto original_interval = std::get<project::ElapsedInterval>(original.placement);
  const auto& state = session.value()->snapshot();
  const auto target = state.tempo.tick_to_sample({3 * bar}, state.sample_rate);
  const auto anchor = state.tempo.sample_to_tick(original_interval.start, state.sample_rate);
  const auto revision = state.revision;

  // Not looping: the length in ticks runs from the anchor tick to the end.
  require(target && anchor && session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}) &&
              session.value()->snapshot().revision == revision + 1,
          "elapsed MIDI resize failed under a tempo ramp");
  const auto interval = std::get<project::ElapsedInterval>(current().placement);
  REQUIRE_EQ(interval.start, original_interval.start,
             "elapsed MIDI resize moved its anchor or did not set its length");
  REQUIRE_EQ(interval.end, target.value(),
             "elapsed MIDI resize moved its anchor or did not set its length");
  REQUIRE_TRUE(!current().loop, "elapsed MIDI resize moved its anchor or did not set its length");
  REQUIRE_EQ(current().pattern_length.value, 3 * bar - anchor.value().value,
             "elapsed MIDI resize moved its anchor or did not set its length");
  REQUIRE_EQ(midi_event_samples(session.value()->snapshot()).size(), 2,
             "elapsed MIDI resize moved its anchor or did not set its length");
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {2 * bar}, 62),
               "a note could not be written into the lengthened elapsed clip");
  REQUIRE_EQ(midi_event_samples(session.value()->snapshot()).size(), 4,
             "a note could not be written into the lengthened elapsed clip");

  // Looping: the same clip repeats its pattern under the ramp.
  require(session.value()->history().undo() && session.value()->history().undo() &&
              current() == original &&
              session.value()->midi_clips().set_midi_clip_loop(clip, true) &&
              current().pattern_length == original.pattern_length &&
              session.value()->midi_clips().resize_midi_clip(clip, {3 * bar}) &&
              std::get<project::ElapsedInterval>(current().placement).end == target.value() &&
              current().loop && midi_event_samples(session.value()->snapshot()).size() == 6,
          "elapsed MIDI loop did not repeat its pattern under the tempo ramp");
  REQUIRE_TRUE(session.value()->history().undo(), "elapsed MIDI resize did not undo atomically");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(current().placement), original_interval,
             "elapsed MIDI resize did not undo atomically");
  REQUIRE_TRUE(current().loop, "elapsed MIDI resize did not undo atomically");
}

void midi_timebase_switch_preserves_tempo_ramp_events() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "ramped-midi");
  constexpr auto quarter = timeline::ticks_per_quarter;
  REQUIRE_TRUE(session, "ramped MIDI timebase setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(),
               "ramped MIDI timebase setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({8 * quarter}, 90.0, timeline::TempoShape::step),
      "ramped MIDI timebase setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
      "ramped MIDI timebase setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "ramped MIDI clip setup failed");
  REQUIRE_TRUE(
      session.value()->notes().add_note(
          session.value()->snapshot().tracks.front().midi_clips.front().id, {quarter / 4}, 64),
      "ramped MIDI clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front();
  auto before = engine::compile_midi_events(session.value()->snapshot());
  REQUIRE_TRUE(before, "ramped MIDI events unavailable");
  REQUIRE_EQ(before.value().instruments().front().events().size(), 2,
             "ramped MIDI events unavailable");
  const auto first_event = before.value().instruments().front().events().front().sample;
  const auto last_event = before.value().instruments().front().events().back().sample;
  const auto end = session.value()->snapshot().tempo.tick_to_sample(
      std::get<project::MusicalInterval>(clip.placement).end,
      session.value()->snapshot().sample_rate);
  REQUIRE_TRUE(end, "ramped MIDI conversion failed");
  REQUIRE_TRUE(session.value()->midi_clips().toggle_midi_clip_timebase(clip.id),
               "ramped MIDI conversion failed");
  const auto& converted = session.value()->snapshot().tracks.front().midi_clips.front();
  auto after = engine::compile_midi_events(session.value()->snapshot());
  REQUIRE_EQ(std::get<project::ElapsedInterval>(converted.placement).end, end.value(),
             "ramped MIDI conversion moved clip or note events");
  REQUIRE_TRUE(after, "ramped MIDI conversion moved clip or note events");
  REQUIRE_EQ(after.value().instruments().front().events().front().sample, first_event,
             "ramped MIDI conversion moved clip or note events");
  REQUIRE_EQ(after.value().instruments().front().events().back().sample, last_event,
             "ramped MIDI conversion moved clip or note events");
}

void audio_split_preserves_source_fades_and_one_step_undo() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio split fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path()));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio split fixture import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const project::ClipFades fades{0.25, 0.25, project::FadeCurve::smoothstep,
                                 project::FadeCurve::linear};
  REQUIRE_OK(session.value()->audio_clips().set_audio_clip_fades(original.id, fades),
             "audio split fixture fade failed");
  const auto old_fades = project::prepare_clip_fades(fades, 4);
  REQUIRE_OK(old_fades, "audio split fixture fade preparation failed");
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->audio_clips().split_audio_clip_at_sample(original.id, {1}),
               "invalid split changed the project or silently truncated a fade");
  REQUIRE_TRUE(!session.value()->audio_clips().split_audio_clip_at_sample(original.id, {0}),
               "invalid split changed the project or silently truncated a fade");
  REQUIRE_TRUE(!session.value()->audio_clips().split_audio_clip_at_sample(original.id, {4}),
               "invalid split changed the project or silently truncated a fade");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid split changed the project or silently truncated a fade");
  auto right_id = session.value()->audio_clips().split_audio_clip_at_sample(original.id, {2});
  REQUIRE_TRUE(right_id, "split did not publish exactly one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "split did not publish exactly one revision");
  const auto& clips = session.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.size(), 2, "split lost source or timeline continuity");
  REQUIRE_EQ(clips.front().id, original.id, "split lost source or timeline continuity");
  REQUIRE_EQ(clips.back().id, right_id.value(), "split lost source or timeline continuity");
  REQUIRE_EQ(clips.front().source, original.source, "split lost source or timeline continuity");
  REQUIRE_EQ(clips.back().source, original.source, "split lost source or timeline continuity");
  REQUIRE_EQ(clips.front().source_range, (project::SourceRange{{0}, {2}}),
             "split lost source or timeline continuity");
  REQUIRE_EQ(clips.back().source_range, (project::SourceRange{{2}, {4}}),
             "split lost source or timeline continuity");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(clips.front().placement),
             (project::ElapsedInterval{{0}, {2}}), "split lost source or timeline continuity");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(clips.back().placement),
             (project::ElapsedInterval{{2}, {4}}), "split lost source or timeline continuity");
  auto left_fades = project::prepare_clip_fades(clips.front().fades, 2);
  auto right_fades = project::prepare_clip_fades(clips.back().fades, 2);
  REQUIRE_TRUE(left_fades, "split produced invalid fades");
  REQUIRE_TRUE(right_fades, "split produced invalid fades");
  for (std::uint64_t frame = 0; frame < 4; ++frame) {
    const auto actual = frame < 2 ? project::clip_fade_gain(left_fades.value(), frame)
                                  : project::clip_fade_gain(right_fades.value(), frame - 2);
    REQUIRE_LT(std::abs(actual - project::clip_fade_gain(old_fades.value(), frame)),
               tolerance::double_rounding, "split changed the original audible fade envelope");
  }
  const auto split_clips = clips;
  REQUIRE_TRUE(session.value()->history().undo(), "one undo did not rejoin the split clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.size(), 1,
             "one undo did not rejoin the split clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().source_range,
             original.source_range, "one undo did not rejoin the split clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().fades, fades,
             "one undo did not rejoin the split clip");
  REQUIRE_TRUE(session.value()->history().redo(), "redo did not restore the split clip");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips, split_clips,
             "redo did not restore the split clip");
  REQUIRE_OK(session.value()->saving().save(), "split save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "split source ranges, placements or fades did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips, split_clips,
             "split source ranges, placements or fades did not reopen");
}

void audio_split_preserves_clip_level_automation() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "automation split fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path()));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "automation split fixture import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, {0.0}, 0.25),
               "clip level points were not accepted");
  REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, {0.25}, 0.5),
               "clip level points were not accepted");
  REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, {0.75}, 1.5),
               "clip level points were not accepted");
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  auto right_id = session.value()->audio_clips().split_audio_clip_at_sample(clip, {2});
  REQUIRE_TRUE(right_id, "automation split did not create one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "automation split did not create one revision");
  const auto& clips = session.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.size(), 2, "automation split lost a clip lane");
  REQUIRE_EQ(clips.front().automation.size(), 1, "automation split lost a clip lane");
  REQUIRE_EQ(clips.back().automation.size(), 1, "automation split lost a clip lane");
  REQUIRE_EQ(clips.back().id, right_id.value(), "automation split lost a clip lane");
  const auto& left = clips.front().automation.front();
  const auto& right = clips.back().automation.front();
  // The cut, halfway from 0.5 to 1.5, takes the level the lane plays there:
  // a gain lane blends in decibels (schema 1.39), so their geometric mean.
  const double cut = std::sqrt(0.5 * 1.5);
  REQUIRE_EQ(left.id, original.automation.front().id,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_NE(right.id, left.id, "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(left.points.size(), 3,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(right.points.size(), 2,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(left.points.back().coordinate, 2,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_LT(std::fabs(left.points.back().value - cut), tolerance::double_rounding,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(right.points.front().coordinate, 0,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_LT(std::fabs(right.points.front().value - cut), tolerance::double_rounding,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(right.points.back().coordinate, 1,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_EQ(right.points.back().value, 1.5,
             "automation split changed the linear curve or reused point IDs");
  REQUIRE_NE(right.points.front().id, left.points.back().id,
             "automation split changed the linear curve or reused point IDs");
  const auto split_clips = clips;
  REQUIRE_TRUE(session.value()->history().undo(),
               "automation split was not one reversible operation");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), original,
             "automation split was not one reversible operation");
  REQUIRE_TRUE(session.value()->history().redo(),
               "automation split was not one reversible operation");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips, split_clips,
             "automation split was not one reversible operation");
  REQUIRE_OK(session.value()->saving().save(), "automation split save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "automation split lanes did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips, split_clips,
             "automation split lanes did not reopen");
}

double independent_clip_curve_value(const project::AutomationLane& lane, std::int64_t coordinate) {
  const auto next =
      std::upper_bound(lane.points.begin(), lane.points.end(), coordinate,
                       [](std::int64_t location, const project::AutomationPoint& point) {
                         return location < point.coordinate;
                       });
  if (next == lane.points.begin())
    return next->value;
  const auto& first = *(next - 1);
  if (next == lane.points.end() || coordinate == first.coordinate)
    return first.value;
  const double u = static_cast<double>(coordinate - first.coordinate) /
                   static_cast<double>(next->coordinate - first.coordinate);
  double progress = 0.0;
  switch (first.to_next) {
  case project::AutomationShape::hold:
    break;
  case project::AutomationShape::linear:
    progress = u;
    break;
  case project::AutomationShape::quadratic:
    progress = u * u;
    break;
  case project::AutomationShape::exponential:
    progress = std::expm1(4.0 * u) / std::expm1(4.0);
    break;
  case project::AutomationShape::ease_in_out:
    progress = u * u * (3.0 - 2.0 * u);
    break;
  case project::AutomationShape::bezier: {
    const auto cubic = [](double t, double a, double b) {
      const double r = 1.0 - t;
      return 3.0 * r * r * t * a + 3.0 * r * t * t * b + t * t * t;
    };
    double low = 0.0;
    double high = 1.0;
    for (int iteration = 0; iteration < 60; ++iteration) {
      const double middle = (low + high) * 0.5;
      if (cubic(middle, first.handles.x1, first.handles.x2) < u)
        low = middle;
      else
        high = middle;
    }
    progress = cubic((low + high) * 0.5, first.handles.y1, first.handles.y2);
    break;
  }
  }
  // Values blend in the lane's scale (schema 1.39): a gain in decibels,
  // silence at -70 dB.
  if (lane.domain.scale == project::AutomationScale::decibel) {
    const auto db = [](double gain) {
      return gain > 0.0 ? std::max(-70.0, 20.0 * std::log10(gain)) : -70.0;
    };
    const double level = std::lerp(db(first.value), db(next->value), progress);
    return level <= -70.0 ? 0.0 : std::pow(10.0, level / 20.0);
  }
  return std::lerp(first.value, next->value, progress);
}

void audio_split_subdivides_exact_curves_and_rejects_exponential() {
  for (const auto shape :
       {project::AutomationShape::quadratic, project::AutomationShape::ease_in_out,
        project::AutomationShape::bezier, project::AutomationShape::exponential}) {
    Directory directory;
    const auto root = directory.path() / "curved-split-song";
    auto session = application::ProjectSession::create(root);
    REQUIRE_OK(session, "curved split fixture create failed");
    {
      FileDescriptor descriptor(make_wav(directory.path(), 48'000, {48'000}));
      REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
                 "curved split import failed");
    }
    const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
    REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                     clip, {0.0}, 0.25),
                 "curved split points could not be saved");
    REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                     clip, {0.75}, 1.75),
                 "curved split points could not be saved");
    REQUIRE_TRUE(session.value()->saving().save(), "curved split points could not be saved");
    session.value().reset();
    {
      auto writer = persistence::ProjectStore::open_writer(root, false);
      REQUIRE_OK(writer, "curved split writer failed");
      auto loaded = writer.value().load_current();
      REQUIRE_OK(loaded, "curved split load failed");
      auto snapshot = std::move(loaded).value().snapshot;
      auto& lane = snapshot.tracks.front().clips.front().automation.front();
      lane.points.front().to_next = shape;
      if (shape == project::AutomationShape::bezier)
        lane.points.front().handles = {0.12, 0.04, 0.75, 0.85};
      ++lane.settings_generation;
      ++snapshot.revision;
      REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
                 "curved split shape could not be saved");
    }
    auto reopened = application::ProjectSession::open(root);
    REQUIRE_OK(reopened, "curved split reopen failed");
    const auto original = reopened.value()->snapshot().tracks.front().clips.front();
    const auto revision = reopened.value()->snapshot().revision;
    const auto result = reopened.value()->audio_clips().split_audio_clip_at_sample(clip, {24'000});
    if (shape == project::AutomationShape::exponential) {
      REQUIRE_TRUE(!result, "exponential split should reject without changing the project");
      REQUIRE_EQ(result.error().code, foundation::ErrorCode::unsupported,
                 "exponential split should reject without changing the project");
      REQUIRE_EQ(reopened.value()->snapshot().revision, revision,
                 "exponential split should reject without changing the project");
      REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front(), original,
                 "exponential split should reject without changing the project");
      continue;
    }
    REQUIRE_TRUE(result, "exact curved split failed");
    REQUIRE_EQ(reopened.value()->snapshot().revision, revision + 1, "exact curved split failed");
    const auto& clips = reopened.value()->snapshot().tracks.front().clips;
    REQUIRE_EQ(clips.size(), 2, "curved split did not store exact Bézier subcurves");
    REQUIRE_EQ(clips.front().automation.size(), 1,
               "curved split did not store exact Bézier subcurves");
    REQUIRE_EQ(clips.back().automation.size(), 1,
               "curved split did not store exact Bézier subcurves");
    REQUIRE_EQ(clips.front().automation.front().points.front().to_next,
               project::AutomationShape::bezier,
               "curved split did not store exact Bézier subcurves");
    REQUIRE_EQ(clips.back().automation.front().points.front().to_next,
               project::AutomationShape::bezier,
               "curved split did not store exact Bézier subcurves");
    const auto& old_lane = original.automation.front();
    for (const std::int64_t coordinate : {0, 1, 3'000, 6'000, 12'000, 18'000, 23'999, 24'000,
                                          24'001, 27'000, 30'000, 35'999, 36'000, 42'000, 47'999}) {
      const auto& split_lane =
          coordinate < 24'000 ? clips.front().automation.front() : clips.back().automation.front();
      const auto local = coordinate < 24'000 ? coordinate : coordinate - 24'000;
      REQUIRE_LT(std::abs(independent_clip_curve_value(old_lane, coordinate) -
                          independent_clip_curve_value(split_lane, local)),
                 tolerance::float_round_trip, "curved split changed the evaluated sample value");
    }
    const auto split_clips = clips;
    REQUIRE_TRUE(reopened.value()->history().undo(), "curved split undo, redo or save failed");
    REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front(), original,
               "curved split undo, redo or save failed");
    REQUIRE_TRUE(reopened.value()->history().redo(), "curved split undo, redo or save failed");
    REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips, split_clips,
               "curved split undo, redo or save failed");
    REQUIRE_TRUE(reopened.value()->saving().save(), "curved split undo, redo or save failed");
    reopened.value().reset();
    auto saved = application::ProjectSession::open(root);
    REQUIRE_TRUE(saved, "curved split did not reopen exactly");
    REQUIRE_EQ(saved.value()->snapshot().tracks.front().clips, split_clips,
               "curved split did not reopen exactly");
  }
}

void elapsed_audio_split_preserves_musical_curve_under_tempo_ramp() {
  Directory directory;
  const auto root = directory.path() / "ramped-curved-split";
  auto session = application::ProjectSession::create(root);
  constexpr auto quarter = timeline::ticks_per_quarter;
  REQUIRE_TRUE(session, "ramped curved split tempo setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({8 * quarter}, 90.0, timeline::TempoShape::step),
      "ramped curved split tempo setup failed");
  REQUIRE_TRUE(
      session.value()->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
      "ramped curved split tempo setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 48'000, {48'000}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "ramped curved split import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, {0.0}, 0.25),
               "ramped curved split points failed");
  REQUIRE_TRUE(session.value()->automation().set_audio_clip_channel_level_point_at_fraction(
                   clip, {0.75}, 1.75),
               "ramped curved split points failed");
  REQUIRE_TRUE(session.value()->saving().save(), "ramped curved split points failed");
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "ramped curved split writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "ramped curved split load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    auto& lane = snapshot.tracks.front().clips.front().automation.front();
    lane.timebase = project::AutomationTimebase::musical;
    lane.points.front().coordinate = 0;
    lane.points.front().to_next = project::AutomationShape::ease_in_out;
    lane.points.back().coordinate = quarter;
    ++lane.settings_generation;
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "ramped curved split lane save failed");
  }
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "ramped curved split reopen failed");
  const auto state = reopened.value()->snapshot();
  const auto first_tick = state.tempo.sample_to_tick({0}, state.sample_rate);
  const auto cut_sample = state.tempo.tick_to_sample({quarter / 2}, state.sample_rate);
  REQUIRE_TRUE(first_tick, "ramped curved split cut conversion failed");
  REQUIRE_TRUE(cut_sample, "ramped curved split cut conversion failed");
  REQUIRE_GT(cut_sample.value().value, 0, "ramped curved split cut conversion failed");
  const auto cut_tick = state.tempo.sample_to_tick(cut_sample.value(), state.sample_rate);
  REQUIRE_TRUE(cut_tick, "ramped curved split cut tick invalid");
  REQUIRE_GT(cut_tick.value().value, 0, "ramped curved split cut tick invalid");
  REQUIRE_LT(cut_tick.value().value, quarter, "ramped curved split cut tick invalid");
  const auto original = state.tracks.front().clips.front();
  const auto split =
      reopened.value()->audio_clips().split_audio_clip_at_sample(clip, cut_sample.value());
  REQUIRE_OK(split, "ramped musical lane on elapsed clip did not split");
  const auto& clips = reopened.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.size(), 2, "ramped split changed clip placement or lane timebase");
  REQUIRE_TRUE(std::holds_alternative<project::ElapsedInterval>(clips.front().placement),
               "ramped split changed clip placement or lane timebase");
  REQUIRE_EQ(clips.front().automation.front().timebase, project::AutomationTimebase::musical,
             "ramped split changed clip placement or lane timebase");
  REQUIRE_EQ(clips.back().automation.front().timebase, project::AutomationTimebase::musical,
             "ramped split changed clip placement or lane timebase");
  for (const std::int64_t sample : std::array<std::int64_t, 8>{
           0, cut_sample.value().value / 2, cut_sample.value().value - 1, cut_sample.value().value,
           cut_sample.value().value + 1, 18'000, 24'000, 36'000}) {
    const auto tick = state.tempo.sample_to_tick({sample}, state.sample_rate);
    REQUIRE_OK(tick, "ramped split sample-to-tick conversion failed");
    const auto old_coordinate = tick.value().value - first_tick.value().value;
    const auto& lane = sample < cut_sample.value().value ? clips.front().automation.front()
                                                         : clips.back().automation.front();
    const auto local = sample < cut_sample.value().value
                           ? old_coordinate
                           : tick.value().value - cut_tick.value().value;
    REQUIRE_LT(std::abs(independent_clip_curve_value(original.automation.front(), old_coordinate) -
                        independent_clip_curve_value(lane, local)),
               tolerance::float_round_trip,
               "ramped split changed musical curve value at sample position");
  }
}

void musical_audio_split_uses_exact_tempo_map_boundaries() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "musical split fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 48'000, {48'000}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "musical split fixture import failed");
  }
  const auto id = session.value()->snapshot().tracks.front().clips.front().id;
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "musical split fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "musical split fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.tracks.front().clips.front().placement =
        project::MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}};
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "musical split fixture publication failed");
  }
  auto musical = application::ProjectSession::open(root);
  REQUIRE_OK(musical, "musical split fixture reopen failed");
  const auto before = musical.value()->snapshot().revision;
  REQUIRE_TRUE(!musical.value()->audio_clips().split_audio_clip_at_sample(id, {0}),
               "boundary musical split changed the project");
  REQUIRE_TRUE(!musical.value()->audio_clips().split_audio_clip_at_sample(id, {48'000}),
               "boundary musical split changed the project");
  REQUIRE_EQ(musical.value()->snapshot().revision, before,
             "boundary musical split changed the project");
  auto right = musical.value()->audio_clips().split_audio_clip_at_sample(id, {24'000});
  REQUIRE_TRUE(right, "exact musical split failed");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.size(), 2,
             "exact musical split failed");
  const auto& clips = musical.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.front().source_range, (project::SourceRange{{0}, {24'000}}),
             "musical split changed source or tick alignment");
  REQUIRE_EQ(clips.back().source_range, (project::SourceRange{{24'000}, {48'000}}),
             "musical split changed source or tick alignment");
  REQUIRE_EQ(std::get<project::MusicalInterval>(clips.front().placement),
             (project::MusicalInterval{{0}, {timeline::ticks_per_quarter}}),
             "musical split changed source or tick alignment");
  REQUIRE_EQ(
      std::get<project::MusicalInterval>(clips.back().placement),
      (project::MusicalInterval{{timeline::ticks_per_quarter}, {timeline::ticks_per_quarter * 2}}),
      "musical split changed source or tick alignment");
}

void audio_slip_moves_only_the_source_window() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio slip fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 48'000, {8}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio slip fixture import failed");
  }
  const auto id = session.value()->snapshot().tracks.front().clips.front().id;
  using Edge = application::AudioClipEdge;
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(id, Edge::left, {2}),
               "audio slip fixture trim failed");
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(id, Edge::right, {6}),
               "audio slip fixture trim failed");
  const project::ClipFades fades{0.25, 0.25, project::FadeCurve::linear,
                                 project::FadeCurve::smoothstep};
  REQUIRE_OK(session.value()->audio_clips().set_audio_clip_fades(id, fades),
             "audio slip fixture fade failed");
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->audio_clips().slip_audio_clip(id, 0),
               "invalid or zero slip changed the project");
  REQUIRE_TRUE(!session.value()->audio_clips().slip_audio_clip(id, -3),
               "invalid or zero slip changed the project");
  REQUIRE_TRUE(!session.value()->audio_clips().slip_audio_clip(id, 3),
               "invalid or zero slip changed the project");
  REQUIRE_TRUE(
      !session.value()->audio_clips().slip_audio_clip(id, std::numeric_limits<std::int64_t>::min()),
      "invalid or zero slip changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid or zero slip changed the project");
  REQUIRE_TRUE(session.value()->audio_clips().slip_audio_clip(id, 2),
               "valid slip did not publish one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "valid slip did not publish one revision");
  const auto slipped = session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(slipped.source_range, (project::SourceRange{{4}, {8}}),
             "slip changed placement, settings or media identity");
  REQUIRE_EQ(slipped.placement, original.placement,
             "slip changed placement, settings or media identity");
  REQUIRE_EQ(slipped.fades, fades, "slip changed placement, settings or media identity");
  REQUIRE_EQ(slipped.source, original.source, "slip changed placement, settings or media identity");
  REQUIRE_EQ(slipped.automation, original.automation,
             "slip changed placement, settings or media identity");
  REQUIRE_TRUE(session.value()->history().undo(), "slip did not undo and redo as one operation");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), original,
             "slip did not undo and redo as one operation");
  REQUIRE_TRUE(session.value()->history().redo(), "slip did not undo and redo as one operation");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), slipped,
             "slip did not undo and redo as one operation");
  REQUIRE_OK(session.value()->saving().save(), "slip save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "slipped source window did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front(), slipped,
             "slipped source window did not reopen");
}

void clip_processing_intent_is_durable_without_unsafe_edit_reinterpretation() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "clip processing fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 48'000, {8}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "clip processing fixture import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  using Edge = application::AudioClipEdge;
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(clip, Edge::left, {1}),
               "clip processing fixture trim failed");
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(clip, Edge::right, {7}),
               "clip processing fixture trim failed");
  project::ClipProcessing processing;
  processing.pitch_semitones = 7.0;
  processing.reviewed_warp_markers.push_back({{4}, timeline::SamplePosition{2}});
  processing.warp.mode = project::WarpMode::tones;
  processing.warp.mode_overridden = true;
  processing.warp.tones.grain_ms = 25.0;
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(session.value()->audio_clips().set_audio_clip_processing(clip, processing),
               "clip processing command did not publish one revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "clip processing command did not publish one revision");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().processing, processing,
             "clip processing command did not publish one revision");
  // Split, slip and trim of processed clips have their own contract
  // (tests/application/clip_warp_edit_tests.cpp); an unchanged edit is a no-op.
  require(!session.value()->audio_clips().set_audio_clip_processing(clip, processing) &&
              session.value()->audio_clips().trim_audio_clip(clip, Edge::left, {1}) &&
              session.value()->snapshot().revision == before + 1,
          "an unchanged processed-clip edit changed its saved interpretation");
  REQUIRE_TRUE(session.value()->history().undo(),
               "clip processing did not undo and redo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().processing,
             project::ClipProcessing{}, "clip processing did not undo and redo atomically");
  REQUIRE_TRUE(session.value()->history().redo(),
               "clip processing did not undo and redo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().processing, processing,
             "clip processing did not undo and redo atomically");
  REQUIRE_OK(session.value()->saving().save(), "clip processing save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "clip processing was lost on reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().processing, processing,
             "clip processing was lost on reopen");
}

void rate_converted_split_and_slip_preserve_renderable_duration() {
  if (!media::source_rate_supported(44'100, 48'000))
    return;
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "rate-converted split setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 44'100, {88'200}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "rate-converted split import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->audio_clips().split_audio_clip_at_sample(original.id, {6}),
               "unrepresentable split changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "unrepresentable split changed the project");
  auto right = session.value()->audio_clips().split_audio_clip_at_sample(original.id, {24'000});
  REQUIRE_TRUE(right, "rate-converted split did not publish one edit");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "rate-converted split did not publish one edit");
  const auto clips = session.value()->snapshot().tracks.front().clips;
  REQUIRE_EQ(clips.size(), 2, "rate-converted split changed playback speed or left a gap");
  REQUIRE_EQ(clips.front().source_range, (project::SourceRange{{0}, {22'050}}),
             "rate-converted split changed playback speed or left a gap");
  REQUIRE_EQ(clips.back().source_range, (project::SourceRange{{22'050}, {88'200}}),
             "rate-converted split changed playback speed or left a gap");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(clips.front().placement),
             (project::ElapsedInterval{{0}, {24'000}}),
             "rate-converted split changed playback speed or left a gap");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(clips.back().placement),
             (project::ElapsedInterval{{24'000}, {96'000}}),
             "rate-converted split changed playback speed or left a gap");
  REQUIRE_TRUE(session.value()->history().undo(),
               "rate-converted split did not undo and redo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips, std::vector{original},
             "rate-converted split did not undo and redo atomically");
  REQUIRE_TRUE(session.value()->history().redo(),
               "rate-converted split did not undo and redo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips, clips,
             "rate-converted split did not undo and redo atomically");
  REQUIRE_OK(session.value()->saving().save(), "rate-converted split save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "rate-converted split did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips, clips,
             "rate-converted split did not reopen");

  Directory slip_directory;
  auto slip_session = application::ProjectSession::create(slip_directory.path() / "song");
  REQUIRE_OK(slip_session, "rate-converted slip setup failed");
  {
    FileDescriptor descriptor(make_wav(slip_directory.path(), 44'100, {6}));
    REQUIRE_OK(slip_session.value()->imports().import_wav_audio(descriptor.get()),
               "rate-converted slip import failed");
  }
  const auto id = slip_session.value()->snapshot().tracks.front().clips.front().id;
  REQUIRE_OK(slip_session.value()->audio_clips().trim_audio_clip(
                 id, application::AudioClipEdge::right, {1}),
             "one-frame rate-converted slip setup failed");
  const auto unslipped = slip_session.value()->snapshot().tracks.front().clips.front();
  const auto slip_revision = slip_session.value()->snapshot().revision;
  REQUIRE_TRUE(!slip_session.value()->audio_clips().slip_audio_clip(id, 5),
               "slip committed a source window that renders a different duration");
  REQUIRE_EQ(slip_session.value()->snapshot().revision, slip_revision,
             "slip committed a source window that renders a different duration");
  REQUIRE_EQ(slip_session.value()->snapshot().tracks.front().clips.front(), unslipped,
             "slip committed a source window that renders a different duration");
  REQUIRE_TRUE(slip_session.value()->audio_clips().slip_audio_clip(id, 1),
               "valid source-rate slip changed the audible placement or failed one-step undo");
  REQUIRE_EQ(slip_session.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{1}, {2}}),
             "valid source-rate slip changed the audible placement or failed one-step undo");
  REQUIRE_EQ(slip_session.value()->snapshot().tracks.front().clips.front().placement,
             unslipped.placement,
             "valid source-rate slip changed the audible placement or failed one-step undo");
  REQUIRE_TRUE(slip_session.value()->history().undo(),
               "valid source-rate slip changed the audible placement or failed one-step undo");
  REQUIRE_EQ(slip_session.value()->snapshot().tracks.front().clips.front(), unslipped,
             "valid source-rate slip changed the audible placement or failed one-step undo");
}

void audio_timebase_toggle_preserves_current_sample_placement() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio timebase fixture setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 48'000, {48'000}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio timebase fixture import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  require(std::holds_alternative<project::ElapsedInterval>(original.placement),
          "audio timebase fixture did not start elapsed");
  REQUIRE_TRUE(session.value()->audio_clips().toggle_audio_clip_timebase(original.id),
               "audio timebase toggle did not publish one edit");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "audio timebase toggle did not publish one edit");
  const auto musical = session.value()->snapshot().tracks.front().clips.front();
  const auto* musical_place = std::get_if<project::MusicalInterval>(&musical.placement);
  REQUIRE_TRUE(musical_place, "timebase toggle changed source settings");
  REQUIRE_EQ(musical.source, original.source, "timebase toggle changed source settings");
  REQUIRE_EQ(musical.source_range, original.source_range,
             "timebase toggle changed source settings");
  REQUIRE_EQ(musical.fades, original.fades, "timebase toggle changed source settings");
  const auto& state = session.value()->snapshot();
  const auto first = state.tempo.tick_to_sample(musical_place->start, state.sample_rate);
  const auto last = state.tempo.tick_to_sample(musical_place->end, state.sample_rate);
  REQUIRE_TRUE(first, "timebase toggle moved the current sample boundaries");
  REQUIRE_TRUE(last, "timebase toggle moved the current sample boundaries");
  REQUIRE_EQ(first.value().value, 0, "timebase toggle moved the current sample boundaries");
  REQUIRE_EQ(last.value().value, 48'000, "timebase toggle moved the current sample boundaries");
  REQUIRE_TRUE(session.value()->history().undo(),
               "timebase toggle did not undo and redo as one edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), original,
             "timebase toggle did not undo and redo as one edit");
  REQUIRE_TRUE(session.value()->history().redo(),
               "timebase toggle did not undo and redo as one edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), musical,
             "timebase toggle did not undo and redo as one edit");
  REQUIRE_OK(session.value()->saving().save(), "musical timebase save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "musical timebase did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front(), musical,
             "musical timebase did not reopen");
  REQUIRE_TRUE(reopened.value()->audio_clips().toggle_audio_clip_timebase(original.id),
               "toggle back to elapsed moved the audio clip");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 reopened.value()->snapshot().tracks.front().clips.front().placement),
             std::get<project::ElapsedInterval>(original.placement),
             "toggle back to elapsed moved the audio clip");
}

void audio_trim_preserves_source_speed_and_reveals_both_edges() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio trim session setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio trim import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  using Edge = application::AudioClipEdge;
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {-1}),
               "invalid trim changed the project or exceeded source bounds");
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {4}),
               "invalid trim changed the project or exceeded source bounds");
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(original.id, Edge::right, {5}),
               "invalid trim changed the project or exceeded source bounds");
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(
                   project::ClipId::from_bits({0, 999}).value(), Edge::left, {1}),
               "invalid trim changed the project or exceeded source bounds");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "invalid trim changed the project or exceeded source bounds");

  REQUIRE_OK(session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {1}),
             "inward trim failed");
  REQUIRE_OK(session.value()->audio_clips().trim_audio_clip(original.id, Edge::right, {3}),
             "inward trim failed");
  const auto& trimmed = session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(trimmed.source, original.source,
             "trim changed speed, source identity, or the opposite edge");
  REQUIRE_EQ(trimmed.source_range, (project::SourceRange{{1}, {3}}),
             "trim changed speed, source identity, or the opposite edge");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(trimmed.placement),
             (project::ElapsedInterval{{1}, {3}}),
             "trim changed speed, source identity, or the opposite edge");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 2,
             "trim changed speed, source identity, or the opposite edge");
  REQUIRE_OK(session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {1}),
             "same-edge trim created an undo step");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 2,
             "same-edge trim created an undo step");
  REQUIRE_OK(session.value()->saving().save(), "trimmed audio save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "trimmed source range did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{1}, {3}}), "trimmed source range did not reopen");
  REQUIRE_OK(reopened.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {0}),
             "outward trim did not reveal original source");
  REQUIRE_OK(reopened.value()->audio_clips().trim_audio_clip(original.id, Edge::right, {4}),
             "outward trim did not reveal original source");
  const auto& revealed = reopened.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(revealed.source_range, original.source_range,
             "outward trim did not restore both original boundaries");
  REQUIRE_EQ(revealed.placement, original.placement,
             "outward trim did not restore both original boundaries");
  REQUIRE_OK(reopened.value()->history().undo(),
             "one undo failed to restore both sides of the right-edge trim");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {3}}),
             "one undo failed to restore both sides of the right-edge trim");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 reopened.value()->snapshot().tracks.front().clips.front().placement),
             (project::ElapsedInterval{{0}, {3}}),
             "one undo failed to restore both sides of the right-edge trim");
  REQUIRE_OK(reopened.value()->saving().save(), "revealed trim save failed");
  reopened.value().reset();
  auto saved = application::ProjectSession::open(root);
  REQUIRE_TRUE(saved, "revealed and undone trim did not reopen");
  REQUIRE_EQ(saved.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {3}}), "revealed and undone trim did not reopen");
}

void audio_trim_converted_source_uses_project_sample_boundaries() {
  if (!media::source_rate_supported(44'100, 48'000))
    return;
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "converted trim session setup failed");
  {
    FileDescriptor descriptor(make_wav(directory.path(), 44'100, {88'200}));
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "converted trim import failed");
  }
  const auto original = session.value()->snapshot().tracks.front().clips.front();
  const auto before = session.value()->snapshot().revision;
  using Edge = application::AudioClipEdge;
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {6}),
               "unrepresentable source-rate trim edge changed the project");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front(), original,
             "unrepresentable source-rate trim edge changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "unrepresentable source-rate trim edge changed the project");
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {24'000}),
               "converted source inward trim failed");
  REQUIRE_TRUE(session.value()->audio_clips().trim_audio_clip(original.id, Edge::right, {72'000}),
               "converted source inward trim failed");
  const auto trimmed = session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(trimmed.source_range, (project::SourceRange{{22'050}, {66'150}}),
             "converted trim shifted audible time or source speed");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(trimmed.placement),
             (project::ElapsedInterval{{24'000}, {72'000}}),
             "converted trim shifted audible time or source speed");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 2,
             "converted trim shifted audible time or source speed");
  REQUIRE_OK(session.value()->saving().save(), "converted trim save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "converted trim did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front(), trimmed,
             "converted trim did not reopen");
  REQUIRE_TRUE(reopened.value()->audio_clips().trim_audio_clip(original.id, Edge::left, {0}),
               "converted trim did not reveal original source audio");
  REQUIRE_TRUE(reopened.value()->audio_clips().trim_audio_clip(original.id, Edge::right, {96'000}),
               "converted trim did not reveal original source audio");
  const auto& revealed = reopened.value()->snapshot().tracks.front().clips.front();
  REQUIRE_EQ(revealed.source_range, original.source_range,
             "revealed converted audio moved or changed source content");
  REQUIRE_EQ(revealed.placement, original.placement,
             "revealed converted audio moved or changed source content");
  REQUIRE_EQ(revealed.fades, original.fades,
             "revealed converted audio moved or changed source content");
  REQUIRE_TRUE(reopened.value()->history().undo(), "converted trim undo did not restore one edit");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {66'150}}), "converted trim undo did not restore one edit");

  Directory higher_rate_directory;
  auto higher_rate_session =
      application::ProjectSession::create(higher_rate_directory.path() / "song");
  REQUIRE_OK(higher_rate_session, "higher-rate trim setup failed");
  {
    FileDescriptor descriptor(make_wav(higher_rate_directory.path(), 96'000, {192'000}));
    REQUIRE_OK(higher_rate_session.value()->imports().import_wav_audio(descriptor.get()),
               "higher-rate trim import failed");
  }
  const auto higher_rate_clip =
      higher_rate_session.value()->snapshot().tracks.front().clips.front();
  REQUIRE_TRUE(higher_rate_session.value()->audio_clips().trim_audio_clip(higher_rate_clip.id,
                                                                          Edge::left, {24'000}),
               "higher-rate trim did not choose the nearest source-time frames");
  REQUIRE_TRUE(higher_rate_session.value()->audio_clips().trim_audio_clip(higher_rate_clip.id,
                                                                          Edge::right, {72'000}),
               "higher-rate trim did not choose the nearest source-time frames");
  REQUIRE_EQ(higher_rate_session.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{48'000}, {144'000}}),
             "higher-rate trim did not choose the nearest source-time frames");
}

void audio_trim_rejects_neighbor_overlap_and_stretched_placement() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path(), 48'000, {48'000});
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "audio trim collision setup failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio trim collision import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  using Edge = application::AudioClipEdge;
  REQUIRE_OK(session.value()->audio_clips().trim_audio_clip(clip, Edge::right, {24'000}),
             "adjacent same-track clip setup failed");
  REQUIRE_TRUE(
      session.value()->audio_clips().duplicate_audio_clip(clip, {timeline::ticks_per_quarter}),
      "adjacent same-track clip setup failed");
  const auto previous = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->audio_clips().trim_audio_clip(clip, Edge::right, {24'001}),
               "overlapping trim published a partial source or placement edit");
  REQUIRE_EQ(session.value()->snapshot().revision, previous,
             "overlapping trim published a partial source or placement edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {24'000}}),
             "overlapping trim published a partial source or placement edit");
  REQUIRE_OK(session.value()->saving().save(), "musical trim fixture save failed");
  session.value().reset();

  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "musical trim fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "musical trim fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.tracks.front().clips.pop_back();
    snapshot.tracks.front().clips.front().placement =
        project::MusicalInterval{{0}, {timeline::ticks_per_quarter}};
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "musical trim fixture publication failed");
  }
  auto musical = application::ProjectSession::open(root);
  REQUIRE_OK(musical, "musical trim fixture reopen failed");
  const auto musical_before = musical.value()->snapshot().revision;
  REQUIRE_OK(musical.value()->audio_clips().trim_audio_clip(clip, Edge::left, {6'000}),
             "musical trim lost its timebase or source alignment");
  REQUIRE_EQ(musical.value()->snapshot().revision, musical_before + 1,
             "musical trim lost its timebase or source alignment");
  REQUIRE_EQ(
      std::get<project::MusicalInterval>(
          musical.value()->snapshot().tracks.front().clips.front().placement),
      (project::MusicalInterval{{timeline::ticks_per_quarter / 4}, {timeline::ticks_per_quarter}}),
      "musical trim lost its timebase or source alignment");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{6'000}, {24'000}}),
             "musical trim lost its timebase or source alignment");
  REQUIRE_OK(musical.value()->audio_clips().trim_audio_clip(clip, Edge::left, {0}),
             "musical outward trim did not reveal source");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {24'000}}), "musical outward trim did not reveal source");
  REQUIRE_OK(musical.value()->audio_clips().trim_audio_clip(clip, Edge::right, {18'000}),
             "musical right-edge trim or reveal lost source alignment");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 musical.value()->snapshot().tracks.front().clips.front().placement),
             (project::MusicalInterval{{0}, {timeline::ticks_per_quarter * 3 / 4}}),
             "musical right-edge trim or reveal lost source alignment");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {18'000}}),
             "musical right-edge trim or reveal lost source alignment");
  REQUIRE_OK(musical.value()->audio_clips().trim_audio_clip(clip, Edge::right, {24'000}),
             "musical right-edge trim or reveal lost source alignment");
  REQUIRE_OK(musical.value()->audio_clips().move_audio_clip(clip, {timeline::ticks_per_quarter}),
             "moving a revealed musical clip changed its source range or timebase");
  REQUIRE_EQ(
      std::get<project::MusicalInterval>(
          musical.value()->snapshot().tracks.front().clips.front().placement),
      (project::MusicalInterval{{timeline::ticks_per_quarter}, {timeline::ticks_per_quarter * 2}}),
      "moving a revealed musical clip changed its source range or timebase");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {24'000}}),
             "moving a revealed musical clip changed its source range or timebase");
  const auto moved_revision = musical.value()->snapshot().revision;
  REQUIRE_TRUE(!musical.value()->audio_clips().move_audio_clip(
                   clip, {std::numeric_limits<std::int64_t>::max()}),
               "musical clip movement overflow published a project edit");
  REQUIRE_EQ(musical.value()->snapshot().revision, moved_revision,
             "musical clip movement overflow published a project edit");
  auto following = musical.value()->audio_clips().duplicate_audio_clip_after(clip);
  REQUIRE_TRUE(following, "musical duplicate did not preserve exact tick length or one-step undo");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 musical.value()->snapshot().tracks.front().clips.back().placement),
             (project::MusicalInterval{{timeline::ticks_per_quarter * 2},
                                       {timeline::ticks_per_quarter * 3}}),
             "musical duplicate did not preserve exact tick length or one-step undo");
  REQUIRE_TRUE(musical.value()->history().undo(),
               "musical duplicate did not preserve exact tick length or one-step undo");
  REQUIRE_EQ(musical.value()->snapshot().tracks.front().clips.size(), 1,
             "musical duplicate did not preserve exact tick length or one-step undo");
  REQUIRE_OK(musical.value()->saving().save(), "musical trim save failed");
  musical.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "stretched trim fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "stretched trim fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.tracks.front().clips.front().placement =
        project::MusicalInterval{{0}, {timeline::ticks_per_quarter / 2}};
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "stretched trim fixture publication failed");
  }
  auto stretched = application::ProjectSession::open(root);
  REQUIRE_OK(stretched, "stretched trim fixture reopen failed");
  const auto stretched_before = stretched.value()->snapshot().revision;
  // A stretched clip is processed: trimming keeps its stretch through the
  // warp map (24,000 source frames over 12,000 outputs).
  require(
      stretched.value()->audio_clips().trim_audio_clip(clip, Edge::right, {6'000}).has_value() &&
          stretched.value()->snapshot().revision == stretched_before + 1 &&
          std::get<project::MusicalInterval>(
              stretched.value()->snapshot().tracks.front().clips.front().placement) ==
              project::MusicalInterval{{0}, {timeline::ticks_per_quarter / 4}} &&
          stretched.value()->snapshot().tracks.front().clips.front().source_range ==
              project::SourceRange{{0}, {12'000}},
      "a stretched clip trim did not keep its stretch ratio");
}

void audio_trim_does_not_reveal_unverified_missing_source() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "missing-source trim fixture create failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "missing-source trim fixture import failed");
  }
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "missing-source trim fixture writer failed");
    auto loaded = writer.value().load_current();
    REQUIRE_OK(loaded, "missing-source trim fixture load failed");
    auto snapshot = std::move(loaded).value().snapshot;
    snapshot.sources.clear(); // The clip remains as an unresolved placeholder.
    ++snapshot.revision;
    REQUIRE_OK(writer.value().save(snapshot, persistence::SaveKind::explicit_save),
               "missing-source placeholder publication failed");
  }
  auto unresolved = application::ProjectSession::open(root);
  REQUIRE_OK(unresolved, "missing-source placeholder did not reopen");
  using Edge = application::AudioClipEdge;
  REQUIRE_OK(unresolved.value()->audio_clips().trim_audio_clip(clip, Edge::right, {3}),
             "missing-source placeholder could not be narrowed");
  const auto before = unresolved.value()->snapshot().revision;
  auto rejected = unresolved.value()->audio_clips().trim_audio_clip(clip, Edge::right, {4});
  REQUIRE_TRUE(!rejected, "missing source allowed unverified reveal or changed the accepted trim");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unavailable,
             "missing source allowed unverified reveal or changed the accepted trim");
  REQUIRE_EQ(unresolved.value()->snapshot().revision, before,
             "missing source allowed unverified reveal or changed the accepted trim");
  REQUIRE_EQ(unresolved.value()->snapshot().tracks.front().clips.front().source_range,
             (project::SourceRange{{0}, {3}}),
             "missing source allowed unverified reveal or changed the accepted trim");
}

void accepted_wav_import_survives_save_failure() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto source = make_wav(directory.path());
  auto initial = application::ProjectSession::create(root);
  REQUIRE_OK(initial, "audio import fault fixture setup failed");
  initial.value().reset();
  bool fail_save = true;
  auto faulted =
      application::ProjectSession::open(root, [&fail_save](persistence::SaveStage stage) {
        return fail_save && stage == persistence::SaveStage::manifest_written;
      });
  REQUIRE_OK(faulted, "audio import fault fixture reopen failed");
  FileDescriptor descriptor(source);
  auto imported = faulted.value()->imports().import_wav_audio(descriptor.get());
  REQUIRE_TRUE(!imported, "failed save discarded or misreported the accepted audio import");
  REQUIRE_NE(imported.error().message.find("save failed"), std::string::npos,
             "failed save discarded or misreported the accepted audio import");
  REQUIRE_EQ(faulted.value()->snapshot().sources.size(), 1,
             "failed save discarded or misreported the accepted audio import");
  REQUIRE_EQ(faulted.value()->snapshot().tracks.size(), 1,
             "failed save discarded or misreported the accepted audio import");
  REQUIRE_EQ(faulted.value()->snapshot().tracks[0].clips.size(), 1,
             "failed save discarded or misreported the accepted audio import");
  fail_save = false;
  REQUIRE_OK(faulted.value()->saving().save(), "audio import retry lost its source receipt");
  faulted.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "retried WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1, "retried WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1, "retried WAV import did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].clips.size(), 1,
             "retried WAV import did not reopen");
}

void midi_move_is_one_atomic_undo_step() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "move fixture setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "move fixture setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  constexpr auto bar = 4 * timeline::ticks_per_quarter;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "move fixture clips failed");
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {3 * bar}),
               "move fixture clips failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  const auto before = session.value()->snapshot().revision;
  require(!session.value()->midi_clips().move_midi_clip(clip, {-bar}),
          "placement before the timeline accepted");
  require(!session.value()->midi_clips().move_midi_clip(clip, {3 * bar}),
          "same-track overlap accepted");
  REQUIRE_EQ(session.value()->snapshot().revision, before,
             "rejected movement changed project revision");
  REQUIRE_OK(session.value()->midi_clips().move_midi_clip(clip, {bar}), "bar movement failed");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "bar movement did not commit one revision");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 session.value()->snapshot().tracks.front().midi_clips.front().placement)
                 .start.value,
             bar, "bar movement did not commit one revision");
  REQUIRE_OK(session.value()->midi_clips().move_midi_clip(clip, {bar}),
             "same-position move failed");
  REQUIRE_EQ(session.value()->snapshot().revision, before + 1,
             "same-position move created an undo step");
  REQUIRE_OK(session.value()->saving().save(), "moved project save failed");
  auto reader = persistence::ProjectStore::open_reader(root);
  auto reopened = reader
                      ? reader.value().load_current()
                      : foundation::Result<persistence::LoadedGeneration>::failure(reader.error());
  REQUIRE_TRUE(reopened, "moved placement did not reopen");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 reopened.value().snapshot.tracks.front().midi_clips.front().placement)
                 .start.value,
             bar, "moved placement did not reopen");
  REQUIRE_OK(session.value()->history().undo(), "move undo failed");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 session.value()->snapshot().tracks.front().midi_clips.front().placement)
                 .start.value,
             0, "one undo did not restore original placement");
  REQUIRE_OK(session.value()->saving().save(), "undone project save failed");
  session.value().reset();
  auto undone_reopen = application::ProjectSession::open(root);
  REQUIRE_TRUE(undone_reopen, "undone placement did not reopen");
  REQUIRE_EQ(std::get<project::MusicalInterval>(
                 undone_reopen.value()->snapshot().tracks.front().midi_clips.front().placement)
                 .start.value,
             0, "undone placement did not reopen");
}

void elapsed_midi_move_keeps_sample_duration_and_one_undo_step() {
  Directory directory;
  const auto root = directory.path() / "elapsed-midi-move";
  auto session = application::ProjectSession::create(root);
  REQUIRE_TRUE(session, "elapsed MIDI move setup failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "elapsed MIDI move setup failed");
  const auto track = session.value()->snapshot().tracks.front().id;
  constexpr auto bar = 4 * timeline::ticks_per_quarter;
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "elapsed MIDI clip setup failed");
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {3 * bar}),
               "elapsed MIDI clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {timeline::ticks_per_quarter / 4}, 64),
               "elapsed MIDI note or timebase setup failed");
  REQUIRE_TRUE(session.value()->midi_clips().toggle_midi_clip_timebase(clip),
               "elapsed MIDI note or timebase setup failed");
  const auto original = std::get<project::ElapsedInterval>(
      session.value()->snapshot().tracks.front().midi_clips.front().placement);
  const auto revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(!session.value()->midi_clips().move_midi_clip(clip, {-bar}),
               "elapsed MIDI move accepted a negative or overlapping target");
  REQUIRE_TRUE(!session.value()->midi_clips().move_midi_clip(clip, {3 * bar}),
               "elapsed MIDI move accepted a negative or overlapping target");
  REQUIRE_EQ(session.value()->snapshot().revision, revision,
             "elapsed MIDI move accepted a negative or overlapping target");
  REQUIRE_OK(session.value()->midi_clips().move_midi_clip(clip, {bar}), "elapsed MIDI move failed");
  const auto placed = std::get<project::ElapsedInterval>(
      session.value()->snapshot().tracks.front().midi_clips.front().placement);
  const auto target = session.value()->snapshot().tempo.tick_to_sample(
      {bar}, session.value()->snapshot().sample_rate);
  REQUIRE_TRUE(target, "elapsed MIDI move changed duration or revision count");
  REQUIRE_EQ(placed.start, target.value(), "elapsed MIDI move changed duration or revision count");
  REQUIRE_EQ(placed.end.value - placed.start.value, original.end.value - original.start.value,
             "elapsed MIDI move changed duration or revision count");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "elapsed MIDI move changed duration or revision count");
  REQUIRE_TRUE(session.value()->midi_clips().move_midi_clip(clip, {bar}),
               "elapsed MIDI move no-op, undo, redo or save failed");
  REQUIRE_EQ(session.value()->snapshot().revision, revision + 1,
             "elapsed MIDI move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().undo(),
               "elapsed MIDI move no-op, undo, redo or save failed");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 session.value()->snapshot().tracks.front().midi_clips.front().placement),
             original, "elapsed MIDI move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->history().redo(),
               "elapsed MIDI move no-op, undo, redo or save failed");
  REQUIRE_TRUE(session.value()->saving().save(),
               "elapsed MIDI move no-op, undo, redo or save failed");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "elapsed MIDI move did not reopen");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(
                 reopened.value()->snapshot().tracks.front().midi_clips.front().placement),
             placed, "elapsed MIDI move did not reopen");
}

void stopped_starter_master_export_survives_session_close() {
  Directory directory;
  const auto root = directory.path() / "export-song";
  const auto output = directory.path() / "master.wav";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "export session setup failed");
  // A starter sound is a Crate track playing the bundled sample as one chop
  // on the first pad (ADR-0063).
  namespace crate = native_dsp::synth::crate;
  const auto added =
      session.value()->instruments().add_native_instrument_track(crate::engine_id, std::nullopt);
  REQUIRE_OK(added, "Crate track setup failed");
  const auto track = added.value();
  FileDescriptor starter(OSCILLATE_STARTER_PLUCK);
  REQUIRE_TRUE(session.value()->imports().import_native_synth_media(
                   track, crate::sample_slot, starter.get(),
                   std::vector<project::NativeRegion>{
                       {0, std::string(crate::sample_slot), 0, "other", "Pluck"}}),
               "starter Crate clip setup failed");
  REQUIRE_TRUE(session.value()->midi_clips().add_midi_clip(track, {0}),
               "starter Crate clip setup failed");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_TRUE(session.value()->notes().add_note(clip, {0}, crate::first_pad_key),
               "starter Crate note save failed");
  REQUIRE_TRUE(session.value()->saving().save(), "starter Crate note save failed");
  auto prepared = session.value()->exports().prepare_wav_master_export({output});
  REQUIRE_TRUE(prepared, "stopped project did not prepare its current revision");
  REQUIRE_EQ(prepared.value().revision(), session.value()->snapshot().revision,
             "stopped project did not prepare its current revision");
  session.value().reset(); // Task owns the snapshot, reader, stopped lease and gate.
  auto published = std::move(prepared.value()).run();
  REQUIRE_TRUE(published, "stopped starter project did not export");
  REQUIRE_EQ(published.value(), exporting::PublishOutcome::durable,
             "stopped starter project did not export");
  auto decoded = media::WavReader::open(output);
  REQUIRE_TRUE(decoded, "exported master does not reopen as stereo 48 kHz WAV");
  REQUIRE_EQ(decoded.value().sample_rate(), 48'000,
             "exported master does not reopen as stereo 48 kHz WAV");
  REQUIRE_EQ(decoded.value().channels(), 2, "exported master does not reopen as stereo 48 kHz WAV");
  REQUIRE_GT(decoded.value().frame_count(), 0,
             "exported master does not reopen as stereo 48 kHz WAV");
  std::array<media::StereoFrame, 4096> frames{};
  auto read = decoded.value().read_at(0, frames);
  REQUIRE_TRUE(read, "exported master has no decoded frames");
  REQUIRE_GT(read.value(), 0, "exported master has no decoded frames");
  bool audible = false;
  for (std::size_t index = 0; index < read.value(); ++index) {
    audible |= frames[index].left != 0.0F || frames[index].right != 0.0F;
  }
  require(audible, "starter MIDI master was silent");
  const auto prior = read_bytes(output);

  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "export project did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1, "export project did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].midi_clips.size(), 1,
             "export project did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].midi_clips[0].notes.size(), 1,
             "export project did not reopen");
  REQUIRE_OK(reopened.value()->tracks().add_instrument_track(),
             "unsupported fixture track add failed");
  const auto unbound = reopened.value()->snapshot().tracks.back().id;
  REQUIRE_OK(reopened.value()->midi_clips().add_midi_clip(unbound, {0}),
             "unsupported fixture clip add failed");
  const auto unbound_clip = reopened.value()->snapshot().tracks.back().midi_clips.front().id;
  REQUIRE_OK(reopened.value()->notes().add_note(unbound_clip, {0}, 60),
             "unsupported fixture note add failed");
  auto unsupported = reopened.value()->exports().prepare_wav_master_export({output});
  REQUIRE_OK(unsupported, "unsupported project was blocked before render preflight");
  auto failed = std::move(unsupported.value()).run();
  REQUIRE_TRUE(!failed, "unsupported project replaced the previous durable master");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unsupported,
             "unsupported project replaced the previous durable master");
  REQUIRE_EQ(read_bytes(output), prior, "unsupported project replaced the previous durable master");
}

void encoded_master_export_keeps_stopped_lease_and_survives_session_close() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto output = directory.path() / "master.flac";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "encoded export session setup failed");
  {
    FileDescriptor input(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
               "encoded export source setup failed");
  }
  auto task = session.value()->exports().prepare_encoded_master_export({output});
  REQUIRE_OK(task, "encoded export did not reserve the stopped transport");
  REQUIRE_EQ(task.value().revision(), session.value()->snapshot().revision,
             "encoded export did not reserve the stopped transport");
  REQUIRE_TRUE(!session.value()->reserve_playback_start(),
               "encoded export did not reserve the stopped transport");
  session.value().reset();
  auto result = std::move(task.value()).run();
#if OSCILLATE_HAVE_CODEC
  const auto bytes = read_bytes(output);
  REQUIRE_TRUE(result, "prepared FLAC export did not survive session close");
  REQUIRE_EQ(result.value(), exporting::PublishOutcome::durable,
             "prepared FLAC export did not survive session close");
  REQUIRE_GT(bytes.size(), 4, "prepared FLAC export did not survive session close");
  REQUIRE_TRUE(std::equal(bytes.begin(), bytes.begin() + 4, "fLaC"),
               "prepared FLAC export did not survive session close");
#else
  require(!result && result.error().code == foundation::ErrorCode::unsupported &&
              !fs::exists(output),
          "codec-off encoded export did not fail clearly without publication");
#endif
}

void stem_export_keeps_stopped_lease_and_survives_session_close() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto target = directory.path() / "stems";
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "stem export session setup failed");
  {
    FileDescriptor input(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
               "stem export source setup failed");
  }
  const auto track_id = session.value()->snapshot().tracks.front().id;
  auto task = session.value()->exports().prepare_wav_stem_export({target});
  REQUIRE_OK(task, "stem export did not reserve stopped transport");
  REQUIRE_EQ(task.value().revision(), session.value()->snapshot().revision,
             "stem export did not reserve stopped transport");
  REQUIRE_TRUE(!session.value()->reserve_playback_start(),
               "stem export did not reserve stopped transport");
  session.value().reset();
  auto result = std::move(task.value()).run();
  const auto stem = target / ("track-" + track_id.to_string() + ".wav");
  REQUIRE_TRUE(result, "prepared stem export did not survive session close");
  REQUIRE_EQ(result.value(), exporting::PublishOutcome::durable,
             "prepared stem export did not survive session close");
  REQUIRE_TRUE(fs::exists(stem), "prepared stem export did not survive session close");
  REQUIRE_GT(read_bytes(stem).size(), 44, "prepared stem export did not survive session close");
}

void playback_and_stopped_export_exclude_each_other() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "transport fixture setup failed");
  const auto output = directory.path() / "master.wav";
  {
    auto export_task = session.value()->exports().prepare_wav_master_export({output});
    REQUIRE_OK(export_task, "stopped export reservation failed");
    require(!session.value()->reserve_playback_start(),
            "playback started while an export held the stopped lease");
  }
  auto playback = session.value()->reserve_playback_start();
  REQUIRE_OK(playback, "playback reservation failed after export release");
  require(!session.value()->exports().prepare_wav_master_export({output}),
          "stopped export prepared while playback was reserved");
  playback.value().finish_after_join(); // No device was started in this control-side test.
  REQUIRE_OK(session.value()->exports().prepare_wav_master_export({output}),
             "stopped export stayed blocked after callback quiescence");
}

void prepared_playback_rejects_a_changed_project_and_live_export_lease() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "prepared playback fixture setup failed");
  application::InstrumentPlayback playback(*session.value());
  REQUIRE_TRUE(!playback.start_prepared({}), "empty playback preparation started a device");
  REQUIRE_TRUE(!playback.playing(), "empty playback preparation started a device");
  auto task = playback.prepare_start();
  REQUIRE_OK(task, "playback preparation request failed");
  auto prepared = std::move(task.value()).run();
  REQUIRE_OK(prepared, "playback preparation failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "stale preparation edit failed");
  REQUIRE_TRUE(!playback.start_prepared(std::move(prepared.value())),
               "stale playback preparation started a device");
  REQUIRE_TRUE(!playback.playing(), "stale playback preparation started a device");
  auto current = playback.prepare_start();
  REQUIRE_OK(current, "current playback preparation request failed");
  auto ready = std::move(current.value()).run();
  REQUIRE_OK(ready, "current playback preparation failed");
  auto export_task =
      session.value()->exports().prepare_wav_master_export({directory.path() / "master.wav"});
  REQUIRE_OK(export_task, "stopped export lease setup failed");
  REQUIRE_TRUE(!playback.start_prepared(std::move(ready.value())),
               "prepared playback bypassed the stopped export lease");
  REQUIRE_TRUE(!playback.playing(), "prepared playback bypassed the stopped export lease");
}

void audio_playback_preparation_pins_managed_source_and_rejects_stale_revision() {
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "audio playback preparation fixture failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "audio clip import failed");
  }
  fs::remove(source);
  application::InstrumentPlayback playback(*session.value());
  auto task = playback.prepare_start();
  REQUIRE_OK(task, "managed audio playback task was rejected");
  auto prepared = std::move(task.value()).run();
  REQUIRE_OK(prepared, "managed audio playback preparation failed");
  auto& audio_option = prepared.value().audio_clip;
  if (!audio_option) {
    throw std::runtime_error("managed audio playback omitted the clip");
  }
  auto& audio = *audio_option;
  REQUIRE_EQ(audio.clip, session.value()->snapshot().tracks[0].clips[0].id,
             "audio playback did not prepare the exact managed clip range");
  REQUIRE_EQ(audio.first_source_frame, 0,
             "audio playback did not prepare the exact managed clip range");
  REQUIRE_EQ(audio.end_source_frame, 4,
             "audio playback did not prepare the exact managed clip range");
  std::array<media::StereoFrame, 4> decoded{};
  auto read = audio.source.read_at(0, decoded);
  REQUIRE_TRUE(read, "prepared clip source did not decode after external file removal");
  REQUIRE_EQ(read.value(), 4, "prepared clip source did not decode after external file removal");
  REQUIRE_EQ(decoded[0].left, 0.25F,
             "prepared clip source did not decode after external file removal");
  REQUIRE_EQ(decoded[0].right, 0.25F,
             "prepared clip source did not decode after external file removal");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(),
             "stale audio preparation edit failed");
  REQUIRE_TRUE(!playback.start_prepared(std::move(prepared).value()),
               "stale audio preparation started playback");
  REQUIRE_TRUE(!playback.playing(), "stale audio preparation started playback");
}

void wait_for_audio_output(application::InstrumentPlayback& playback);

void instrument_only_start_passes_requested_buffer() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_TRUE(session, "instrument-only buffer fixture failed");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(),
               "instrument-only buffer fixture failed");
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device), 64);
  auto prepared = std::move(playback.prepare_start().value()).run();
  REQUIRE_TRUE(prepared, "instrument-only start did not pass the requested buffer");
  REQUIRE_TRUE(playback.start_prepared(std::move(prepared).value()),
               "instrument-only start did not pass the requested buffer");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::streaming,
             "instrument-only start did not pass the requested buffer");
  REQUIRE_EQ(device->requested_buffer_frames, 64,
             "instrument-only start did not pass the requested buffer");
  playback.stop();
}

void playback_controller_renders_and_retires_with_fake_output() {
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "fake output project create failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "fake output clip import failed");
  }
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device), 64);
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::stopped,
             "idle playback reported an active device");
  REQUIRE_EQ(playback.completed_sample(), 0, "idle playback reported an active device");
  REQUIRE_EQ(playback.source_underruns(), 0, "idle playback reported an active device");
  REQUIRE_TRUE(!playback.pop_source_underrun(), "idle playback reported an active device");
  auto task = playback.prepare_start();
  REQUIRE_OK(task, "fake output preparation request failed");
  auto prepared = std::move(task.value()).run();
  REQUIRE_OK(prepared, "fake output source preparation failed");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "audio controller failed to reserve priming before output start");
  REQUIRE_TRUE(playback.playing(),
               "audio controller failed to reserve priming before output start");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::stopped,
             "audio controller failed to reserve priming before output start");
  REQUIRE_TRUE(!playback.prepare_start(),
               "audio controller failed to reserve priming before output start");
  bool started = false;
  oscillate::test::wait_until(
      [&] {
        auto ready = playback.poll_start();
        REQUIRE_OK(ready, "audio clip priming failed");
        started = ready.value();
        return started;
      },
      "audio clip primes");
  REQUIRE_TRUE(started, "audio clip did not start through the fake output");
  REQUIRE_TRUE(playback.poll_start().value(), "audio clip did not start through the fake output");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::streaming,
             "audio clip did not start through the fake output");
  REQUIRE_EQ(playback.output_status().requested_client_rate, 48'000,
             "audio clip did not start through the fake output");
  REQUIRE_EQ(device->requested_buffer_frames, 64,
             "audio clip did not start through the fake output");
  REQUIRE_TRUE(!playback.set_requested_buffer_frames(128),
               "audio clip did not start through the fake output");
  device->pump();
  playback.observe();
  REQUIRE_EQ(device->peak, 0.25F,
             "managed audio clip did not render at the correct value and revision");
  REQUIRE_EQ(playback.completed_sample(), 128,
             "managed audio clip did not render at the correct value and revision");
  REQUIRE_EQ(playback.source_underruns(), 0,
             "managed audio clip did not render at the correct value and revision");
  REQUIRE_TRUE(!playback.pop_source_underrun(),
               "managed audio clip did not render at the correct value and revision");
  REQUIRE_TRUE(!playback.source_failure(),
               "managed audio clip did not render at the correct value and revision");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "managed audio clip did not render at the correct value and revision");
  playback.stop();
  oscillate::test::wait_until([&] { return playback.poll_retirement(); },
                              "the stopped output retires");
  REQUIRE_TRUE(!playback.playing(),
               "output stop did not retire the worker or release the export lease");
  REQUIRE_TRUE(!playback.retiring(),
               "output stop did not retire the worker or release the export lease");
  REQUIRE_EQ(device->status.state, audio_io::OutputState::stopped,
             "output stop did not retire the worker or release the export lease");
  REQUIRE_TRUE(
      session.value()->exports().prepare_wav_master_export({directory.path() / "master.wav"}),
      "output stop did not retire the worker or release the export lease");
  REQUIRE_TRUE(!playback.set_requested_buffer_frames(96),
               "stopped direct playback did not validate a new request");
  REQUIRE_OK(playback.set_requested_buffer_frames(application::max_playback_buffer_frames),
             "stopped direct playback did not validate a new request");
  auto again = std::move(playback.prepare_start().value()).run();
  REQUIRE_TRUE(again, "direct restart could not use a changed buffer request");
  REQUIRE_TRUE(playback.start_prepared(std::move(again).value()),
               "direct restart could not use a changed buffer request");
  wait_for_audio_output(playback);
  REQUIRE_EQ(device->requested_buffer_frames, application::max_playback_buffer_frames,
             "direct restart retained the old requested buffer size");
  playback.stop();
}

void playback_controller_rejects_absent_output_without_retaining_transport() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "absent output project create failed");
  application::InstrumentPlayback playback(
      *session.value(), [](audio_io::StereoRenderSource, audio_io::TransportCursor&) {
        return std::unique_ptr<application::PlaybackOutput>{};
      });
  REQUIRE_TRUE(!playback.start(), "absent output left playback or export reservation active");
  REQUIRE_TRUE(!playback.playing(), "absent output left playback or export reservation active");
  REQUIRE_TRUE(!playback.retiring(), "absent output left playback or export reservation active");
  REQUIRE_TRUE(!playback.poll_start(), "absent output left playback or export reservation active");
  REQUIRE_TRUE(
      session.value()->exports().prepare_wav_master_export({directory.path() / "master.wav"}),
      "absent output left playback or export reservation active");
}

// Priming runs on a worker; the deadline only guards against a hang.
void wait_for_audio_output(application::InstrumentPlayback& playback) {
  oscillate::test::wait_until(
      [&] {
        auto ready = playback.poll_start();
        REQUIRE_OK(ready, "live audio feed failed to prime");
        return ready.value();
      },
      "live audio feed primes");
}

void live_audio_trim_restarts_at_completed_boundary() {
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {48'000});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "live trim session create failed");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
             "live trim source import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device));
  auto prepared = std::move(playback.prepare_start().value()).run();
  REQUIRE_OK(prepared, "live trim initial playback failed");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "live trim initial playback failed");
  wait_for_audio_output(playback);
  device->pump();
  playback.observe();
  REQUIRE_EQ(playback.completed_sample(), 128,
             "live trim fixture did not reach an active completed block");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "live trim fixture did not reach an active completed block");

  REQUIRE_OK(
      session.value()->audio_clips().trim_audio_clip(clip, application::AudioClipEdge::left, {64}),
      "live trim project edit failed");
  auto unsafe = playback.submit_current();
  REQUIRE_TRUE(!unsafe, "clip edit silently reused a stale feed or stopped old audio");
  REQUIRE_EQ(unsafe.error().code, foundation::ErrorCode::conflict,
             "clip edit silently reused a stale feed or stopped old audio");
  REQUIRE_EQ(playback.status().model_revision, session.value()->snapshot().revision,
             "clip edit silently reused a stale feed or stopped old audio");
  REQUIRE_LT(playback.status().active_graph_revision, playback.status().model_revision,
             "clip edit silently reused a stale feed or stopped old audio");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::failed,
             "clip edit silently reused a stale feed or stopped old audio");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::streaming,
             "clip edit silently reused a stale feed or stopped old audio");
  auto stale_task = playback.prepare_live_edit();
  REQUIRE_OK(stale_task, "live edit preparation was refused");
  auto stale = std::move(stale_task).value().run();
  REQUIRE_OK(stale, "live edit worker preparation failed");
  REQUIRE_OK(
      session.value()->audio_clips().trim_audio_clip(clip, application::AudioClipEdge::left, {128}),
      "second live trim failed");
  REQUIRE_TRUE(!playback.restart_prepared_after_edit(std::move(stale).value()),
               "stale live edit stopped or displaced active playback");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::streaming,
             "stale live edit stopped or displaced active playback");
  REQUIRE_EQ(playback.completed_sample(), 128,
             "stale live edit stopped or displaced active playback");

  auto current_task = playback.prepare_live_edit();
  REQUIRE_OK(current_task, "current live edit preparation failed");
  auto current = std::move(current_task).value().run();
  REQUIRE_OK(current, "current live edit worker preparation failed");
  auto& current_audio = current.value().audio_clip;
  if (!current_audio) {
    throw std::runtime_error("current live edit omitted the audio clip");
  }
  REQUIRE_EQ(current_audio->first_source_frame, 128,
             "current live edit did not prepare trimmed source geometry");
  REQUIRE_OK(playback.restart_prepared_after_edit(std::move(current).value()),
             "live edit handoff did not stop at the completed sample with pending status");
  REQUIRE_EQ(playback.output_state(), audio_io::OutputState::stopped,
             "live edit handoff did not stop at the completed sample with pending status");
  REQUIRE_EQ(playback.completed_sample(), 128,
             "live edit handoff did not stop at the completed sample with pending status");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::pending,
             "live edit handoff did not stop at the completed sample with pending status");
  wait_for_audio_output(playback);
  device->peak = 0.0F;
  device->pump();
  playback.observe();
  REQUIRE_EQ(playback.completed_sample(), 256,
             "restarted graph lost its sample boundary or counted priming as an underrun");
  REQUIRE_EQ(playback.source_underruns(), 0,
             "restarted graph lost its sample boundary or counted priming as an underrun");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "restarted graph lost its sample boundary or counted priming as an underrun");
  REQUIRE_EQ(device->peak, 0.25F,
             "restarted graph lost its sample boundary or counted priming as an underrun");
  REQUIRE_EQ(device->transport->completed_snapshot().generation, 2,
             "restarted graph lost its sample boundary or counted priming as an underrun");
  REQUIRE_OK(session.value()->audio_clips().move_audio_clip(clip, {0}),
             "live audio move project edit failed");
  require(!playback.submit_current(), "live audio move reused the old clip feed");
  auto moved_task = playback.prepare_live_edit();
  REQUIRE_OK(moved_task, "live audio move preparation was refused");
  auto moved = std::move(moved_task).value().run();
  REQUIRE_OK(moved, "live audio move could not re-prime at the completed boundary");
  REQUIRE_TRUE(playback.restart_prepared_after_edit(std::move(moved).value()),
               "live audio move could not re-prime at the completed boundary");
  wait_for_audio_output(playback);
  device->peak = 0.0F;
  device->pump();
  playback.observe();
  REQUIRE_EQ(playback.completed_sample(), 384,
             "moved clip lost its source cursor or active revision on the second handoff");
  REQUIRE_EQ(playback.source_underruns(), 0,
             "moved clip lost its source cursor or active revision on the second handoff");
  REQUIRE_EQ(device->peak, 0.25F,
             "moved clip lost its source cursor or active revision on the second handoff");
  REQUIRE_EQ(device->transport->completed_snapshot().generation, 3,
             "moved clip lost its source cursor or active revision on the second handoff");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "moved clip lost its source cursor or active revision on the second handoff");
  require(!playback.prepare_live_edit(), "unchanged audio revision requested another handoff");
  REQUIRE_OK(
      session.value()->audio_clips().move_audio_clip(clip, {2 * timeline::ticks_per_quarter}),
      "future live move project edit failed");
  auto future_task = playback.prepare_live_edit();
  REQUIRE_OK(future_task, "future live move preparation was refused");
  auto future = std::move(future_task).value().run();
  REQUIRE_OK(future, "future clip could not prime ahead of its new project position");
  REQUIRE_TRUE(playback.restart_prepared_after_edit(std::move(future).value()),
               "future clip could not prime ahead of its new project position");
  wait_for_audio_output(playback);
  device->peak = 0.0F;
  device->pump();
  playback.observe();
  device->peak = 0.0F;
  device->pump();
  REQUIRE_EQ(playback.completed_sample(), 640,
             "future move rendered at its old position or lost transport generation");
  REQUIRE_EQ(playback.source_underruns(), 0,
             "future move rendered at its old position or lost transport generation");
  REQUIRE_EQ(device->peak, 0.0F,
             "future move rendered at its old position or lost transport generation");
  REQUIRE_EQ(device->transport->completed_snapshot().generation, 4,
             "future move rendered at its old position or lost transport generation");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "future move rendered at its old position or lost transport generation");
  playback.stop();
}

void live_trim_past_playhead_requires_no_source_worker() {
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {48'000});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "past clip session create failed");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
             "past clip source import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device));
  auto prepared = std::move(playback.prepare_start().value()).run();
  REQUIRE_OK(prepared, "past clip initial playback failed");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "past clip initial playback failed");
  wait_for_audio_output(playback);
  device->pump();
  playback.observe();
  REQUIRE_OK(session.value()->audio_clips().trim_audio_clip(clip, application::AudioClipEdge::right,
                                                            {128}),
             "past clip right trim failed");
  auto task = playback.prepare_live_edit();
  REQUIRE_OK(task, "past clip preparation was refused");
  auto next = std::move(task).value().run();
  REQUIRE_OK(next, "past clip could not publish without a source worker");
  REQUIRE_TRUE(playback.restart_prepared_after_edit(std::move(next).value()),
               "past clip could not publish without a source worker");
  REQUIRE_TRUE(playback.poll_start().value(), "past clip incorrectly waited for a source worker");
  REQUIRE_EQ(playback.source_underruns(), 0, "past clip incorrectly waited for a source worker");
  device->peak = 0.0F;
  device->pump();
  playback.observe();
  device->peak = 0.0F;
  device->pump();
  REQUIRE_EQ(playback.completed_sample(), 384, "past clip rendered outside its trimmed range");
  REQUIRE_EQ(device->peak, 0.0F, "past clip rendered outside its trimmed range");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "past clip rendered outside its trimmed range");
  playback.stop();
}

void live_audio_reprime_failure_preserves_edit_and_reports_divergence() {
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {48'000});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "failed live trim session create failed");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
             "failed live trim source import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device));
  auto prepared = std::move(playback.prepare_start().value()).run();
  REQUIRE_OK(prepared, "failed live trim initial playback failed");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "failed live trim initial playback failed");
  wait_for_audio_output(playback);
  device->pump();
  playback.observe();
  REQUIRE_OK(
      session.value()->audio_clips().trim_audio_clip(clip, application::AudioClipEdge::left, {128}),
      "failed live trim edit was rejected");
  auto task = playback.prepare_live_edit();
  REQUIRE_OK(task, "failed live edit preparation was refused");
  auto next = std::move(task).value().run();
  REQUIRE_OK(next, "failed live edit worker preparation failed");
  auto& next_audio = next.value().audio_clip;
  if (!next_audio) {
    throw std::runtime_error("failed live edit fixture omitted prepared clip");
  }
  next_audio->end_source_frame += 1;
  REQUIRE_TRUE(!playback.restart_prepared_after_edit(std::move(next).value()),
               "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_TRUE(!playback.playing(),
               "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::failed,
             "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_EQ(playback.status().model_revision, session.value()->snapshot().revision,
             "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_LT(playback.status().active_graph_revision, playback.status().model_revision,
             "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_OK(playback.status().failure,
             "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_EQ(session.value()->snapshot().tracks.front().clips.front().source_range.start.value, 128,
             "failed re-prime hid model/graph divergence or discarded accepted trim");
  REQUIRE_OK(session.value()->saving().save(), "failed re-prime prevented project save");
}

void live_reprime_device_failure_remains_visible_after_stop() {
  Directory directory;
  const auto source = make_wav(directory.path(), 48'000, {48'000});
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "device failure session create failed");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
             "device failure source import failed");
  const auto clip = session.value()->snapshot().tracks.front().clips.front().id;
  auto device = std::make_shared<FakePlaybackDevice>();
  std::size_t factory_calls = 0;
  application::InstrumentPlayback playback(
      *session.value(),
      [device, &factory_calls](
          audio_io::StereoRenderSource render,
          audio_io::TransportCursor& transport) -> std::unique_ptr<application::PlaybackOutput> {
        ++factory_calls;
        if (factory_calls == 2) {
          return {};
        }
        device->source = render;
        device->transport = &transport;
        return std::make_unique<FakePlaybackOutput>(device);
      });
  REQUIRE_TRUE(!playback.prepare_live_edit(), "live edit started without a streaming transport");
  REQUIRE_TRUE(!playback.restart_prepared_after_edit({}),
               "live edit started without a streaming transport");
  auto prepared = std::move(playback.prepare_start().value()).run();
  REQUIRE_OK(prepared, "device failure initial playback failed");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "device failure initial playback failed");
  wait_for_audio_output(playback);
  device->pump();
  playback.observe();
  REQUIRE_OK(
      session.value()->audio_clips().trim_audio_clip(clip, application::AudioClipEdge::left, {128}),
      "device failure trim edit failed");
  auto task = playback.prepare_live_edit();
  REQUIRE_OK(task, "device failure live preparation was refused");
  auto next = std::move(task).value().run();
  REQUIRE_OK(next, "device failure handoff did not publish revised graph");
  REQUIRE_TRUE(playback.restart_prepared_after_edit(std::move(next).value()),
               "device failure handoff did not publish revised graph");
  bool failed = false;
  oscillate::test::wait_until(
      [&] {
        auto poll = playback.poll_start();
        if (!poll) {
          failed = true;
          return true;
        }
        require(!poll.value(), "missing restart output reported as started");
        return false;
      },
      "the missing restart output fails");
  REQUIRE_TRUE(failed, "device restart failure discarded the visible accepted-edit status");
  REQUIRE_EQ(factory_calls, 2, "device restart failure discarded the visible accepted-edit status");
  REQUIRE_TRUE(!playback.playing(),
               "device restart failure discarded the visible accepted-edit status");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::failed,
             "device restart failure discarded the visible accepted-edit status");
  REQUIRE_EQ(playback.status().model_revision, session.value()->snapshot().revision,
             "device restart failure discarded the visible accepted-edit status");
  REQUIRE_OK(playback.status().failure,
             "device restart failure discarded the visible accepted-edit status");
  REQUIRE_LT(playback.status().active_graph_revision, playback.status().model_revision,
             "device restart failure discarded the visible accepted-edit status");
}

void playback_controller_prepares_and_renders_two_managed_clips() {
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "multi-clip session create failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "first audio import failed");
  }
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "second audio import failed");
  }
  fs::remove(source);
  auto device = std::make_shared<FakePlaybackDevice>();
  application::InstrumentPlayback playback(*session.value(), fake_output_factory(device));
  auto task = playback.prepare_start();
  REQUIRE_OK(task, "multi-clip preparation request failed");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "managed multi-clip preparation omitted a source");
  REQUIRE_TRUE(prepared.value().audio_clip, "managed multi-clip preparation omitted a source");
  REQUIRE_EQ(prepared.value().additional_audio_clips.size(), 1,
             "managed multi-clip preparation omitted a source");
  REQUIRE_OK(playback.start_prepared(std::move(prepared).value()),
             "multi-clip playback reservation failed");
  bool started = false;
  oscillate::test::wait_until(
      [&] {
        auto ready = playback.poll_start();
        REQUIRE_OK(ready, "multi-clip priming failed");
        started = ready.value();
        return started;
      },
      "multi-clip playback primes");
  require(started, "multi-clip playback missed priming deadline");
  device->pump();
  playback.observe();
  REQUIRE_EQ(device->peak, 0.5F,
             "two managed WAV clips did not mix at their common project position");
  REQUIRE_EQ(playback.source_underruns(), 0,
             "two managed WAV clips did not mix at their common project position");
  REQUIRE_TRUE(!playback.source_failure(),
               "two managed WAV clips did not mix at their common project position");
  REQUIRE_EQ(playback.status().application, application::PlaybackApplication::active,
             "two managed WAV clips did not mix at their common project position");
  playback.stop();
  oscillate::test::wait_until([&] { return playback.poll_retirement(); },
                              "the multi-clip output retires");
  REQUIRE_TRUE(!playback.retiring(), "multi-clip stop left workers or transport reserved");
  REQUIRE_TRUE(
      session.value()->exports().prepare_wav_master_export({directory.path() / "master.wav"}),
      "multi-clip stop left workers or transport reserved");
}

void failed_second_clip_worker_releases_first_worker_and_transport() {
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "worker failure session create failed");
  for (int index = 0; index < 2; ++index) {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "worker failure clip import failed");
  }
  application::InstrumentPlayback playback(*session.value());
  auto task = playback.prepare_start();
  REQUIRE_OK(task, "worker failure preparation request failed");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "worker failure fixture did not prepare two clips");
  REQUIRE_EQ(prepared.value().additional_audio_clips.size(), 1,
             "worker failure fixture did not prepare two clips");
  ++prepared.value().additional_audio_clips[0].end_source_frame;
  REQUIRE_TRUE(!playback.start_prepared(std::move(prepared).value()),
               "invalid second worker did not fail startup");
  REQUIRE_TRUE(!playback.playing(), "invalid second worker did not fail startup");
  oscillate::test::wait_until([&] { return playback.poll_retirement(); },
                              "the failed second worker retires");
  REQUIRE_TRUE(!playback.retiring(),
               "failed second worker retained first worker or playback lease");
  REQUIRE_TRUE(
      session.value()->exports().prepare_wav_master_export({directory.path() / "master.wav"}),
      "failed second worker retained first worker or playback lease");
}

void playback_preparation_rejects_unbounded_clip_count_before_media_io() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "clip-limit session create failed");
  {
    FileDescriptor descriptor(source);
    REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
               "clip-limit source import failed");
  }
  auto overfull = std::make_shared<project::Snapshot>(session.value()->snapshot());
  overfull->tracks[0].clips.resize(33, overfull->tracks[0].clips.front());
  fs::remove_all(root / "assets");
  auto result = std::move(application::InstrumentPlaybackPreparation{root, overfull}).run();
  REQUIRE_TRUE(!result, "unbounded playback opened media before rejecting the clip count");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::resource_exhausted,
             "unbounded playback opened media before rejecting the clip count");
}

void playback_preparation_rejects_saved_processing_it_cannot_render() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "routing guard session create failed");
  auto snapshot = std::make_shared<project::Snapshot>(session.value()->snapshot());
  const auto track_id = project::TrackId::from_bits({0, 77}).value();
  snapshot->tracks.emplace_back(track_id, std::vector<project::Clip>{},
                                project::TrackKind::group_bus);
  auto routed = std::move(application::InstrumentPlaybackPreparation{root, snapshot}).run();
  REQUIRE_TRUE(!routed, "live playback silently accepted saved bus routing");
  REQUIRE_EQ(routed.error().code, foundation::ErrorCode::unsupported,
             "live playback silently accepted saved bus routing");

  snapshot->tracks.clear();
  snapshot->tracks.emplace_back(track_id);
  snapshot->tracks[0].effects.push_back(
      {project::InsertSlotId::from_bits({0, 178}).value(), true,
       project::Vst3PluginInstance{project::PluginInstanceId::from_bits({0, 78}).value(),
                                   "plugin",
                                   "vendor",
                                   "effect",
                                   "1",
                                   "",
                                   "",
                                   {},
                                   {},
                                   false}});
  auto plugin = std::move(application::InstrumentPlaybackPreparation{root, snapshot}).run();
  REQUIRE_TRUE(!plugin, "live playback silently accepted a saved VST3 insert");
  REQUIRE_EQ(plugin.error().code, foundation::ErrorCode::unsupported,
             "live playback silently accepted a saved VST3 insert");

  // The direct path plays unautomated audio tracks only: ADR-0063 removed its
  // sampler, whose level lane was the one lane it applied. Automated projects
  // play routed.
  snapshot->tracks[0].effects.clear();
  snapshot->tracks[0].automation.push_back(
      {project::AutomationLaneId::from_bits({0, 80}).value(),
       project::TrackControlTarget{track_id, channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        1.0, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::elapsed,
       {{project::AutomationPointId::from_bits({0, 81}).value(), 0, 0.5}},
       1});
  require(project::requires_routed_rendering(*snapshot),
          "a channel-level lane did not choose routed playback");
  auto automated = std::move(application::InstrumentPlaybackPreparation{root, snapshot}).run();
  REQUIRE_TRUE(!automated, "the direct path accepted automation it cannot apply");
  REQUIRE_EQ(automated.error().code, foundation::ErrorCode::unsupported,
             "the direct path accepted automation it cannot apply");
}

std::size_t asset_entries(const fs::path& root) {
  return static_cast<std::size_t>(
      std::distance(fs::directory_iterator(root / "assets"), fs::directory_iterator{}));
}

void asynchronous_wav_import_rejects_stale_result_without_publishing() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "async import project setup failed");
  FileDescriptor descriptor(source);
  auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
  REQUIRE_OK(task, "async import begin failed");
  const auto baseline = session.value()->snapshot().revision;
  REQUIRE_EQ(asset_entries(root), 0, "begin published an asset or model edit");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "begin published an asset or model edit");
  REQUIRE_EQ(session.value()->snapshot().revision, baseline,
             "begin published an asset or model edit");
  auto future = std::async(std::launch::async, [task = std::move(task).value()]() mutable {
    return std::move(task).run();
  });
  REQUIRE_OK(session.value()->tracks().add_instrument_track(),
             "project control could not edit during worker import");
  {
    auto prepared = future.get();
    REQUIRE_OK(prepared, "worker staging failed");
    auto rejected = session.value()->imports().finish_wav_audio_import(std::move(prepared).value());
    REQUIRE_TRUE(!rejected, "stale WAV import was committed");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
               "stale WAV import was committed");
  }
  REQUIRE_EQ(asset_entries(root), 0, "stale import left a managed asset or model edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "stale import left a managed asset or model edit");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "stale import left a managed asset or model edit");
}

void asynchronous_wav_import_commits_one_undo_step_and_reopens() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "async successful import project setup failed");
  const auto baseline = session.value()->snapshot().revision;
  project::TrackId imported = project::TrackId::from_bits({0, 2}).value();
  {
    auto task = [&] {
      FileDescriptor descriptor(source);
      auto started = session.value()->imports().begin_wav_audio_import(descriptor.get());
      REQUIRE_OK(started, "async successful import begin failed");
      return std::move(started).value();
    }(); // The caller may close its original descriptor before worker execution.
    auto prepared = std::move(task).run();
    REQUIRE_OK(prepared, "async successful import staging failed");
    auto committed =
        session.value()->imports().finish_wav_audio_import(std::move(prepared).value());
    REQUIRE_OK(committed, "async successful import finish failed");
    imported = committed.value();
  }
  REQUIRE_EQ(session.value()->snapshot().revision, baseline + 1,
             "async import did not commit one track/source/clip revision");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "async import did not commit one track/source/clip revision");
  REQUIRE_EQ(session.value()->snapshot().tracks[0].id, imported,
             "async import did not commit one track/source/clip revision");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "async import did not commit one track/source/clip revision");
  REQUIRE_EQ(asset_entries(root), 1, "async import did not commit one track/source/clip revision");
  REQUIRE_OK(session.value()->history().undo(),
             "async import did not undo as one logical operation");
  REQUIRE_TRUE(session.value()->snapshot().tracks.empty(),
               "async import did not undo as one logical operation");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "async import did not undo as one logical operation");
  session.value().reset(); // release the sole writer before reopening the saved generation
  fs::remove(source);
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "async managed import did not reopen without external source");
  REQUIRE_EQ(reopened.value()->snapshot().tracks[0].id, imported,
             "async managed import did not reopen without external source");
}

void asynchronous_wav_import_can_insert_above_existing_track() {
  Directory directory;
  const auto source = make_wav(directory.path());
  auto session = application::ProjectSession::create(directory.path() / "ordered-song");
  REQUIRE_OK(session, "ordered import setup failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "ordered import setup failed");
  const auto existing = session.value()->snapshot().tracks.front().id;
  FileDescriptor descriptor(source);
  auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
  REQUIRE_OK(task, "ordered import did not begin");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "ordered import did not stage");
  auto imported =
      session.value()->imports().finish_wav_audio_import(std::move(prepared).value(), 0);
  REQUIRE_OK(imported, "audio import did not insert above the existing track");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "audio import did not insert above the existing track");
  REQUIRE_EQ(session.value()->snapshot().tracks[0].id, imported.value(),
             "audio import did not insert above the existing track");
  REQUIRE_EQ(session.value()->snapshot().tracks[1].id, existing,
             "audio import did not insert above the existing track");
  REQUIRE_OK(session.value()->history().undo(), "ordered audio import did not undo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "ordered audio import did not undo atomically");
  REQUIRE_EQ(session.value()->snapshot().tracks[0].id, existing,
             "ordered audio import did not undo atomically");
}

void asynchronous_wav_import_cancellation_and_rejection_leave_no_asset() {
  Directory directory;
  const auto source = make_wav(directory.path(), 44'117);
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "async cancellation project setup failed");
  FileDescriptor descriptor(source);
  std::stop_source cancellation;
  cancellation.request_stop();
  {
    auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
    REQUIRE_OK(task, "cancelled import begin failed");
    auto cancelled = std::move(task).value().run(cancellation.get_token());
    REQUIRE_TRUE(!cancelled, "pre-cancelled WAV import was not rejected");
    REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
               "pre-cancelled WAV import was not rejected");
  }
  {
    auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
    REQUIRE_OK(task, "rejected import begin failed");
    auto prepared = std::move(task).value().run();
    REQUIRE_OK(prepared, "rejected import staging failed");
    auto rejected = session.value()->imports().finish_wav_audio_import(std::move(prepared).value());
    REQUIRE_TRUE(!rejected, "wrong-rate WAV import was accepted");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
               "wrong-rate WAV import was accepted");
  }
  REQUIRE_EQ(asset_entries(root), 0, "cancelled or rejected WAV left managed bytes");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "cancelled or rejected WAV left managed bytes");
}

void asynchronous_wav_import_rejects_foreign_writer_and_swapped_directory() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto first_root = directory.path() / "first";
  const auto second_root = directory.path() / "second";
  auto first = application::ProjectSession::create(first_root);
  auto second = application::ProjectSession::create(second_root);
  REQUIRE_TRUE(first, "foreign import project setup failed");
  REQUIRE_TRUE(second, "foreign import project setup failed");
  FileDescriptor descriptor(source);
  {
    auto task = first.value()->imports().begin_wav_audio_import(descriptor.get());
    REQUIRE_OK(task, "foreign import begin failed");
    auto prepared = std::move(task).value().run();
    REQUIRE_OK(prepared, "foreign import staging failed");
    auto rejected = second.value()->imports().finish_wav_audio_import(std::move(prepared).value());
    REQUIRE_TRUE(!rejected, "another project writer accepted staged WAV");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
               "another project writer accepted staged WAV");
  }
  REQUIRE_EQ(asset_entries(first_root), 0, "foreign import left managed bytes");
  REQUIRE_EQ(asset_entries(second_root), 0, "foreign import left managed bytes");
  {
    auto task = first.value()->imports().begin_wav_audio_import(descriptor.get());
    REQUIRE_OK(task, "directory-swap import begin failed");
    auto prepared = std::move(task).value().run();
    REQUIRE_OK(prepared, "directory-swap staging failed");
    fs::rename(first_root / "assets", first_root / "assets-old");
    fs::create_directory(first_root / "assets");
    auto rejected = first.value()->imports().finish_wav_audio_import(std::move(prepared).value());
    REQUIRE_TRUE(!rejected, "swapped assets directory accepted staged WAV");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
               "swapped assets directory accepted staged WAV");
  }
  REQUIRE_EQ(asset_entries(first_root), 0, "directory swap changed project state");
  REQUIRE_TRUE(first.value()->snapshot().sources.empty(), "directory swap changed project state");
}

void asynchronous_wav_import_retains_receipt_after_failed_save() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto root = directory.path() / "song";
  auto initial = application::ProjectSession::create(root);
  REQUIRE_OK(initial, "async save-fault setup failed");
  initial.value().reset();
  bool fail_once = true;
  auto session =
      application::ProjectSession::open(root, [&fail_once](persistence::SaveStage stage) {
        if (stage == persistence::SaveStage::manifest_written && fail_once) {
          fail_once = false;
          return true;
        }
        return false;
      });
  REQUIRE_OK(session, "async save-fault reopen failed");
  FileDescriptor descriptor(source);
  auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
  REQUIRE_OK(task, "async save-fault begin failed");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "async save-fault staging failed");
  auto failed = session.value()->imports().finish_wav_audio_import(std::move(prepared).value());
  REQUIRE_TRUE(!failed, "failed save discarded accepted async import");
  REQUIRE_NE(failed.error().message.find("save failed"), std::string::npos,
             "failed save discarded accepted async import");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "failed save discarded accepted async import");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "failed save discarded accepted async import");
  REQUIRE_OK(session.value()->saving().save(), "async import receipt did not survive save failure");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "retry did not reopen accepted async import");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1,
             "retry did not reopen accepted async import");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
             "retry did not reopen accepted async import");
}

void asynchronous_duplicate_import_restores_missing_managed_asset() {
  Directory directory;
  const auto source = make_wav(directory.path());
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "duplicate restore setup failed");
  FileDescriptor descriptor(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(descriptor.get()),
             "initial managed WAV import failed");
  REQUIRE_EQ(asset_entries(root), 1, "initial source asset was not published");
  fs::remove(fs::directory_iterator(root / "assets")->path());
  auto task = session.value()->imports().begin_wav_audio_import(descriptor.get());
  REQUIRE_OK(task, "duplicate restore begin failed");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "duplicate restore staging failed");
  auto finished = session.value()->imports().finish_wav_audio_import(std::move(prepared).value());
  REQUIRE_OK(finished,
             "duplicate import did not restore managed bytes without duplicating source identity");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "duplicate import did not restore managed bytes without duplicating source identity");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "duplicate import did not restore managed bytes without duplicating source identity");
  REQUIRE_EQ(asset_entries(root), 1,
             "duplicate import did not restore managed bytes without duplicating source identity");
}

#if OSCILLATE_HAVE_CODEC
fs::path encoded_fixture(std::string_view name) {
  return fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / name;
}

void encoded_import_reopens_and_deduplicates() {
  for (const auto [name, encoding] : {std::pair{"sine.aiff", project::SourceEncoding::aiff},
                                      std::pair{"sine.flac", project::SourceEncoding::flac},
                                      std::pair{"sine.mp3", project::SourceEncoding::mp3}}) {
    Directory directory;
    const auto root = directory.path() / "song";
    auto session = application::ProjectSession::create(root);
    REQUIRE_OK(session, "encoded project setup failed");
    const auto baseline = session.value()->snapshot().revision;
    FileDescriptor input(encoded_fixture(name));
    auto started = session.value()->imports().begin_encoded_audio_import(input.get());
    REQUIRE_OK(started, "encoded import did not begin");
    REQUIRE_EQ(session.value()->snapshot().revision, baseline,
               "encoded begin changed project or assets");
    REQUIRE_EQ(asset_entries(root), 0, "encoded begin changed project or assets");
    auto worker = std::async(std::launch::async, [task = std::move(started).value()]() mutable {
      return std::move(task).run();
    });
    auto prepared = worker.get();
    REQUIRE_OK(prepared, "encoded worker lost detected source format");
    REQUIRE_EQ(prepared.value().source().encoding, encoding,
               "encoded worker lost detected source format");
    const auto first =
        session.value()->imports().finish_encoded_audio_import(std::move(prepared).value());
    REQUIRE_OK(first, "encoded import did not commit one managed source and revision");
    REQUIRE_EQ(session.value()->snapshot().revision, baseline + 1,
               "encoded import did not commit one managed source and revision");
    REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
               "encoded import did not commit one managed source and revision");
    REQUIRE_EQ(asset_entries(root), 1,
               "encoded import did not commit one managed source and revision");
    auto duplicate = session.value()->imports().begin_encoded_audio_import(input.get());
    REQUIRE_OK(duplicate, "encoded duplicate did not begin");
    auto duplicated = std::move(duplicate).value().run();
    REQUIRE_OK(duplicated, "encoded duplicate did not stage");
    REQUIRE_OK(
        session.value()->imports().finish_encoded_audio_import(std::move(duplicated).value()),
        "encoded duplicate changed source identity or added duplicate bytes");
    REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
               "encoded duplicate changed source identity or added duplicate bytes");
    REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
               "encoded duplicate changed source identity or added duplicate bytes");
    REQUIRE_EQ(asset_entries(root), 1,
               "encoded duplicate changed source identity or added duplicate bytes");
    REQUIRE_OK(session.value()->history().undo(), "encoded duplicate was not one undo step");
    REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
               "encoded duplicate was not one undo step");
    REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
               "encoded duplicate was not one undo step");
    session.value().reset();
    auto reopened = application::ProjectSession::open(root);
    REQUIRE_OK(reopened, "encoded project did not reopen with original encoded identity");
    REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 2,
               "encoded project did not reopen with original encoded identity");
    REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1,
               "encoded project did not reopen with original encoded identity");
    REQUIRE_EQ(reopened.value()->snapshot().sources[0].encoding, encoding,
               "encoded project did not reopen with original encoded identity");
  }
}

void encoded_import_rejects_cancel_stale_and_malformed() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "encoded rejection project setup failed");
  FileDescriptor input(encoded_fixture("sine.flac"));
  std::stop_source cancel;
  cancel.request_stop();
  {
    auto task = session.value()->imports().begin_encoded_audio_import(input.get());
    REQUIRE_OK(task, "cancelled encoded import did not begin");
    auto stopped = std::move(task).value().run(cancel.get_token());
    REQUIRE_TRUE(!stopped, "cancelled encoded import staged content");
    REQUIRE_EQ(stopped.error().code, foundation::ErrorCode::cancelled,
               "cancelled encoded import staged content");
  }
  {
    auto task = session.value()->imports().begin_encoded_audio_import(input.get());
    REQUIRE_OK(task, "stale encoded import did not begin");
    auto prepared = std::move(task).value().run();
    REQUIRE_OK(prepared, "stale encoded import did not stage");
    REQUIRE_OK(session.value()->tracks().add_instrument_track(), "intervening edit failed");
    auto stale =
        session.value()->imports().finish_encoded_audio_import(std::move(prepared).value());
    REQUIRE_TRUE(!stale, "stale encoded import committed");
    REQUIRE_EQ(stale.error().code, foundation::ErrorCode::conflict,
               "stale encoded import committed");
  }
  const auto malformed = directory.path() / "malformed.flac";
  {
    std::ofstream output(malformed, std::ios::binary);
    output << "fLaCinvalid";
  }
  {
    FileDescriptor broken(malformed);
    auto task = session.value()->imports().begin_encoded_audio_import(broken.get());
    REQUIRE_OK(task, "malformed encoded source staged");
    REQUIRE_TRUE(!std::move(task).value().run(), "malformed encoded source staged");
  }
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "cancelled, stale or malformed encoded import left managed bytes");
  REQUIRE_EQ(asset_entries(root), 0,
             "cancelled, stale or malformed encoded import left managed bytes");
}

void encoded_import_retains_accepted_edit_after_save_failure() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto created = application::ProjectSession::create(root);
  REQUIRE_OK(created, "encoded save-fault setup failed");
  created.value().reset();
  bool fail_once = true;
  auto session =
      application::ProjectSession::open(root, [&fail_once](persistence::SaveStage stage) {
        if (stage == persistence::SaveStage::manifest_written && fail_once) {
          fail_once = false;
          return true;
        }
        return false;
      });
  REQUIRE_OK(session, "encoded save-fault reopen failed");
  FileDescriptor input(encoded_fixture("sine.aiff"));
  auto task = session.value()->imports().begin_encoded_audio_import(input.get());
  REQUIRE_OK(task, "encoded save-fault import did not begin");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "encoded save-fault import did not stage");
  auto failed = session.value()->imports().finish_encoded_audio_import(std::move(prepared).value());
  REQUIRE_TRUE(!failed, "encoded save failure discarded accepted model edit");
  REQUIRE_EQ(session.value()->snapshot().sources.size(), 1,
             "encoded save failure discarded accepted model edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "encoded save failure discarded accepted model edit");
  REQUIRE_OK(session.value()->saving().save(), "encoded save retry lost source receipt");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_OK(reopened, "encoded save retry did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().sources.size(), 1, "encoded save retry did not reopen");
}
#else
void encoded_import_is_explicitly_unavailable_without_codec() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(session, "codec-off project setup failed");
  const auto source = make_wav(directory.path());
  FileDescriptor input(source);
  auto unavailable = session.value()->imports().begin_encoded_audio_import(input.get());
  REQUIRE_TRUE(!unavailable, "codec-off encoded import did not report unsupported");
  REQUIRE_EQ(unavailable.error().code, foundation::ErrorCode::unsupported,
             "codec-off encoded import did not report unsupported");
  REQUIRE_TRUE(session.value()->snapshot().sources.empty(),
               "codec-off encoded import did not report unsupported");
}
#endif

void named_revision_restore_is_saved_undoable_and_never_repoints_name() {
  Directory directory;
  const auto root = directory.path() / "named-project";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "named project create failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "named baseline save failed");
  REQUIRE_OK(session.value()->saving().save(), "named baseline save failed");
  const auto baseline = session.value()->snapshot().revision;
  auto named = session.value()->saving().create_named_revision("First mix");
  REQUIRE_TRUE(named, "named baseline was not committed");
  REQUIRE_EQ(named.value().status, persistence::SaveStatus::committed,
             "named baseline was not committed");
  auto listing = session.value()->saving().list_named_revisions();
  REQUIRE_TRUE(listing, "named baseline was not listed");
  REQUIRE_EQ(listing.value().revisions.size(), 1, "named baseline was not listed");
  REQUIRE_EQ(listing.value().revisions.front().name, "First mix", "named baseline was not listed");
  REQUIRE_TRUE(listing.value().issues.empty(), "named baseline was not listed");
  REQUIRE_TRUE(!session.value()->saving().create_named_revision("First mix"),
               "duplicate name replaced saved point");
  REQUIRE_EQ(session.value()->saving().list_named_revisions().value().revisions.size(), 1,
             "duplicate name replaced saved point");
  auto playback = session.value()->reserve_playback_start();
  REQUIRE_OK(playback, "named actions bypassed active transport");
  REQUIRE_TRUE(!session.value()->saving().restore_named_revision("First mix"),
               "named actions bypassed active transport");
  REQUIRE_TRUE(!session.value()->saving().create_named_revision("While playing"),
               "named actions bypassed active transport");
  playback.value().finish_after_join();

  REQUIRE_OK(session.value()->tracks().add_bus_track(project::TrackKind::group_bus),
             "named later edit failed");
  REQUIRE_TRUE(!session.value()->saving().restore_named_revision("First mix"),
               "restore discarded unsaved current edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "restore discarded unsaved current edit");
  REQUIRE_OK(session.value()->saving().save(), "named later save failed");
  const auto before_restore = session.value()->snapshot().revision;
  REQUIRE_OK(session.value()->saving().restore_named_revision("First mix"),
             "named restore did not publish one monotonic revision");
  REQUIRE_EQ(session.value()->snapshot().revision, before_restore + 1,
             "named restore did not publish one monotonic revision");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "named restore did not publish one monotonic revision");
  const auto restored_revision = session.value()->snapshot().revision;
  REQUIRE_OK(session.value()->history().undo(), "named restore was not one undoable edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 2,
             "named restore was not one undoable edit");
  REQUIRE_OK(session.value()->history().redo(), "named restore was not one undoable edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "named restore was not one undoable edit");
  REQUIRE_OK(session.value()->saving().save(), "named redo save failed");
  session.value().reset();
  session = application::ProjectSession::open(root);
  REQUIRE_TRUE(session, "restored CURRENT did not reopen or cross-session undo leaked");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "restored CURRENT did not reopen or cross-session undo leaked");
  REQUIRE_GT(session.value()->snapshot().revision, restored_revision,
             "restored CURRENT did not reopen or cross-session undo leaked");
  REQUIRE_TRUE(!session.value()->history().undo(),
               "restored CURRENT did not reopen or cross-session undo leaked");
  auto store = persistence::ProjectStore::open_reader(root);
  REQUIRE_TRUE(store, "restore moved immutable named pointer");
  REQUIRE_EQ(store.value().load_named_revision("First mix").value().snapshot.revision, baseline,
             "restore moved immutable named pointer");
}

void named_revision_rejects_bad_media_and_keeps_failed_save_undoable() {
  Directory directory;
  const auto root = directory.path() / "named-media";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "named media create failed");
  const auto source = make_wav(directory.path());
  FileDescriptor input(source);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()), "named media import failed");
  const auto hash = session.value()->snapshot().sources.front().sha256;
  REQUIRE_OK(session.value()->saving().create_named_revision("With audio"),
             "named media point failed");
  auto absent = session.value()->saving().restore_named_revision("Unknown point");
  REQUIRE_TRUE(!absent, "unknown named revision was restored");
  REQUIRE_EQ(absent.error().code, foundation::ErrorCode::not_found,
             "unknown named revision was restored");
  REQUIRE_OK(session.value()->tracks().add_bus_track(project::TrackKind::group_bus),
             "named media later save failed");
  REQUIRE_OK(session.value()->saving().save(), "named media later save failed");
  const auto current_revision = session.value()->snapshot().revision;
  const auto later_track_count = session.value()->snapshot().tracks.size();
  const auto asset = root / "assets" / (hash + ".wav");
  fs::remove(asset);
  auto unavailable = session.value()->saving().restore_named_revision("With audio");
  REQUIRE_TRUE(!unavailable, "missing named media changed model");
  REQUIRE_EQ(unavailable.error().code, foundation::ErrorCode::unavailable,
             "missing named media changed model");
  REQUIRE_EQ(session.value()->snapshot().revision, current_revision,
             "missing named media changed model");
  fs::copy_file(source, asset);
  fs::resize_file(asset, 0);
  auto damaged = session.value()->saving().restore_named_revision("With audio");
  REQUIRE_TRUE(!damaged, "damaged named media changed model");
  REQUIRE_NE(damaged.error().message.find("damaged"), std::string::npos,
             "damaged named media changed model");
  REQUIRE_EQ(session.value()->snapshot().revision, current_revision,
             "damaged named media changed model");
  fs::copy_file(source, asset, fs::copy_options::overwrite_existing);
  session.value().reset();
  bool fail_once = true;
  session = application::ProjectSession::open(root, [&fail_once](persistence::SaveStage stage) {
    if (stage == persistence::SaveStage::manifest_written && fail_once) {
      fail_once = false;
      return true;
    }
    return false;
  });
  REQUIRE_OK(session, "named faulted reopen failed");
  auto failed = session.value()->saving().restore_named_revision("With audio");
  REQUIRE_TRUE(!failed, "save failure lost or falsely committed restored edit");
  REQUIRE_NE(failed.error().message.find("save not confirmed"), std::string::npos,
             "save failure lost or falsely committed restored edit");
  REQUIRE_EQ(session.value()->snapshot().revision, current_revision + 1,
             "save failure lost or falsely committed restored edit");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), 1,
             "save failure lost or falsely committed restored edit");
  REQUIRE_OK(session.value()->history().undo(), "failed restore could not be undone and saved");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), later_track_count,
             "failed restore could not be undone and saved");
  REQUIRE_OK(session.value()->saving().save(), "failed restore could not be undone and saved");
  session.value().reset();
  session = application::ProjectSession::open(root);
  REQUIRE_TRUE(session, "failed restore damaged CURRENT or named pointer");
  REQUIRE_EQ(session.value()->snapshot().tracks.size(), later_track_count,
             "failed restore damaged CURRENT or named pointer");
  REQUIRE_EQ(session.value()->saving().list_named_revisions().value().revisions.size(), 1,
             "failed restore damaged CURRENT or named pointer");
  session.value().reset();
  bool naming_fault = true;
  session = application::ProjectSession::open(root, [&naming_fault](persistence::SaveStage stage) {
    if (stage == persistence::SaveStage::manifest_written && naming_fault) {
      naming_fault = false;
      return true;
    }
    return false;
  });
  REQUIRE_OK(session, "named create-fault reopen failed");
  auto unconfirmed = session.value()->saving().create_named_revision("Not durable");
  REQUIRE_TRUE(!unconfirmed, "failed save published a named pointer");
  REQUIRE_EQ(session.value()->saving().list_named_revisions().value().revisions.size(), 1,
             "failed save published a named pointer");
}

void named_revision_rejects_rate_change_without_changing_current() {
  Directory directory;
  const auto root = directory.path() / "named-rate";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "named rate create failed");
  auto original = session.value()->snapshot();
  session.value().reset();
  auto writer = persistence::ProjectStore::open_writer(root, false);
  REQUIRE_OK(writer, "named rate writer failed");
  auto alternate = original;
  alternate.revision = 2;
  alternate.sample_rate = oscillate::test::sample_rate_44k;
  REQUIRE_OK(writer.value().save(alternate, persistence::SaveKind::explicit_save),
             "named alternate-rate generation failed");
  REQUIRE_OK(writer.value().name_current_revision("Other rate"),
             "named alternate-rate generation failed");
  original.revision = 3;
  REQUIRE_OK(writer.value().save(original, persistence::SaveKind::explicit_save),
             "named current-rate restore setup failed");
  writer = persistence::ProjectStore::open_reader(root);
  session = application::ProjectSession::open(root);
  REQUIRE_OK(session, "named rate reopen failed");
  auto rejected = session.value()->saving().restore_named_revision("Other rate");
  REQUIRE_TRUE(!rejected, "named restore changed project rate without device renegotiation");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
             "named restore changed project rate without device renegotiation");
  REQUIRE_EQ(session.value()->snapshot().sample_rate, 48'000,
             "named restore changed project rate without device renegotiation");
  REQUIRE_EQ(session.value()->snapshot().revision, 3,
             "named restore changed project rate without device renegotiation");
}

void wait_managed_repair_ready(application::ProjectSession& session) {
  oscillate::test::wait_until([&] { return session.media_repair().managed_source_repair_ready(); },
                              "the managed repair worker finishes");
  require(session.media_repair().managed_source_repair_ready(),
          "managed repair worker did not finish");
}

foundation::Result<persistence::RestoreStatus>
wait_managed_repair(application::ProjectSession& session) {
  wait_managed_repair_ready(session);
  auto result = session.media_repair().take_managed_source_repair_result();
  if (!result)
    throw std::runtime_error("managed repair result vanished");
  return std::move(result.value());
}

void managed_media_repair_preserves_identity_and_reopens() {
  Directory directory;
  const auto root = directory.path() / "repair-project";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "managed repair create failed");
  const auto candidate = make_wav(directory.path());
  FileDescriptor input(candidate);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
             "managed repair import failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(),
             "repair accepted unsaved project edits");
  REQUIRE_TRUE(!session.value()->media_repair().begin_managed_source_repair(
                   session.value()->snapshot().sources.front().id, input.get()),
               "repair accepted unsaved project edits");
  REQUIRE_OK(session.value()->saving().save(), "repair fixture save failed");
  const auto baseline = session.value()->snapshot();
  const auto source = baseline.sources.front();
  require(!session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
          "repair accepted already available media");
  const auto asset = root / "assets" / (source.sha256 + ".wav");
  fs::remove(asset);
  REQUIRE_TRUE(!session.value()->media_repair().begin_managed_source_repair(
                   project::SourceId::from_bits({0, 999'999}).value(), input.get()),
               "repair accepted an unknown source or invalid candidate descriptor");
  REQUIRE_TRUE(!session.value()->media_repair().begin_managed_source_repair(source.id, -1),
               "repair accepted an unknown source or invalid candidate descriptor");
  {
    auto playing = session.value()->reserve_playback_start();
    REQUIRE_OK(playing, "repair started while playback owned transport");
    REQUIRE_TRUE(
        !session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
        "repair started while playback owned transport");
    playing.value().finish_after_join();
  }
  auto issues = session.value()->media_repair().managed_media_issues();
  REQUIRE_TRUE(issues, "saved missing source identity was not exposed");
  REQUIRE_EQ(issues.value().size(), 1, "saved missing source identity was not exposed");
  REQUIRE_EQ(issues.value()[0].id, source.id, "saved missing source identity was not exposed");
  REQUIRE_EQ(issues.value()[0].source, source, "saved missing source identity was not exposed");
  REQUIRE_EQ(issues.value()[0].status,
             persistence::LoadedGeneration::SourceDiagnostic::Status::missing,
             "saved missing source identity was not exposed");
  const auto wrong_path = directory.path() / "wrong.wav";
  fs::copy_file(candidate, wrong_path);
  {
    std::fstream wrong(wrong_path, std::ios::binary | std::ios::in | std::ios::out);
    wrong.seekp(44);
    wrong.put('\x7f');
  }
  FileDescriptor wrong(wrong_path);
  REQUIRE_OK(session.value()->media_repair().begin_managed_source_repair(source.id, wrong.get()),
             "repair did not hold project writer and stopped transport");
  REQUIRE_TRUE(session.value()->media_repair().managed_source_repair_busy(),
               "repair did not hold project writer and stopped transport");
  REQUIRE_TRUE(!session.value()->saving().save(),
               "repair did not hold project writer and stopped transport");
  REQUIRE_TRUE(!session.value()->reserve_playback_start(),
               "repair did not hold project writer and stopped transport");
  REQUIRE_TRUE(!session.value()->media_repair().managed_media_issues(),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->imports().begin_wav_audio_import(input.get()),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->imports().begin_encoded_audio_import(input.get()),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->imports().begin_native_synth_media_import(
                   baseline.tracks.back().id, native_dsp::synth::crate::sample_slot, input.get()),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->saving().list_named_revisions(),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->saving().restore_named_revision("missing"),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->exports().prepare_stopped_snapshot_export(),
               "repair allowed a competing project-store operation");
  REQUIRE_TRUE(!session.value()->exports().prepare_wav_master_export(
                   {root / "blocked.wav", exporting::WavFormat::float32}),
               "repair allowed a competing project-store operation");
  wait_managed_repair_ready(*session.value());
  require(!session.value()->reserve_playback_start(),
          "repair released stopped lease before completion was consumed");
  auto rejected = wait_managed_repair(*session.value());
  issues = session.value()->media_repair().managed_media_issues();
  REQUIRE_TRUE(!rejected, "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "wrong same-length bytes repaired or changed project state");
  REQUIRE_TRUE(!fs::exists(asset), "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(session.value()->snapshot().revision, baseline.revision,
             "wrong same-length bytes repaired or changed project state");
  REQUIRE_TRUE(issues, "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(issues.value().size(), 1, "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(issues.value()[0].id, source.id,
             "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(issues.value()[0].source, source,
             "wrong same-length bytes repaired or changed project state");
  REQUIRE_EQ(issues.value()[0].status,
             persistence::LoadedGeneration::SourceDiagnostic::Status::missing,
             "wrong same-length bytes repaired or changed project state");

  fs::copy_file(candidate, asset);
  fs::resize_file(asset, 0);
  issues = session.value()->media_repair().managed_media_issues();
  REQUIRE_TRUE(issues, "damaged existing asset was silently replaced");
  REQUIRE_EQ(issues.value().size(), 1, "damaged existing asset was silently replaced");
  REQUIRE_EQ(issues.value()[0].status,
             persistence::LoadedGeneration::SourceDiagnostic::Status::damaged,
             "damaged existing asset was silently replaced");
  REQUIRE_TRUE(!session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
               "damaged existing asset was silently replaced");
  fs::remove(asset);

  const auto mutable_path = directory.path() / "mutable.wav";
  fs::copy_file(candidate, mutable_path);
  FileDescriptor mutable_input(mutable_path);
  {
    std::fstream changed(mutable_path, std::ios::binary | std::ios::in | std::ios::out);
    changed.seekp(44);
    changed.put('\x7e');
  }
  REQUIRE_OK(
      session.value()->media_repair().begin_managed_source_repair(source.id, mutable_input.get()),
      "mutated candidate did not begin verification");
  auto mutated = wait_managed_repair(*session.value());
  issues = session.value()->media_repair().managed_media_issues();
  REQUIRE_TRUE(!mutated, "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_EQ(mutated.error().code, foundation::ErrorCode::conflict,
             "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_TRUE(!fs::exists(asset),
               "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_TRUE(issues, "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_EQ(issues.value().size(), 1,
             "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_EQ(issues.value()[0].source, source,
             "same-inode candidate mutation after descriptor pinning was accepted");
  REQUIRE_EQ(issues.value()[0].status,
             persistence::LoadedGeneration::SourceDiagnostic::Status::missing,
             "same-inode candidate mutation after descriptor pinning was accepted");

  FileDescriptor pinned(candidate);
  const auto moved = directory.path() / "exact-moved.wav";
  fs::rename(candidate, moved);
  fs::copy_file(wrong_path, candidate);
  REQUIRE_OK(session.value()->media_repair().begin_managed_source_repair(source.id, pinned.get()),
             "exact pinned candidate did not begin repair");
  auto restored = wait_managed_repair(*session.value());
  REQUIRE_TRUE(restored, "pinned exact-file repair changed project identity or edits");
  REQUIRE_EQ(restored.value(), persistence::RestoreStatus::committed,
             "pinned exact-file repair changed project identity or edits");
  REQUIRE_TRUE(session.value()->media_repair().managed_media_issues().value().empty(),
               "pinned exact-file repair changed project identity or edits");
  REQUIRE_EQ(session.value()->snapshot().revision, baseline.revision,
             "pinned exact-file repair changed project identity or edits");
  REQUIRE_EQ(session.value()->snapshot().sources, baseline.sources,
             "pinned exact-file repair changed project identity or edits");
  REQUIRE_EQ(session.value()->snapshot().tracks, baseline.tracks,
             "pinned exact-file repair changed project identity or edits");
  REQUIRE_OK(session.value()->saving().save(), "repaired project did not save");
  session.value().reset();
  session = application::ProjectSession::open(root);
  REQUIRE_TRUE(session, "repaired asset or source identity did not reopen");
  REQUIRE_TRUE(session.value()->media_repair().managed_media_issues().value().empty(),
               "repaired asset or source identity did not reopen");
  REQUIRE_EQ(session.value()->snapshot().sources, baseline.sources,
             "repaired asset or source identity did not reopen");
  REQUIRE_EQ(session.value()->snapshot().tracks, baseline.tracks,
             "repaired asset or source identity did not reopen");
  auto reader = persistence::ProjectStore::open_reader(root);
  REQUIRE_OK(reader, "repaired project reader failed");
  auto checked = reader.value().open_managed_source(source);
  REQUIRE_OK(checked, "repaired managed bytes failed verification");
  ::close(checked.value());
}

void managed_media_repair_cancel_joins_before_writer_close() {
  Directory directory;
  const auto root = directory.path() / "repair-close";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "repair close create failed");
  const auto candidate = make_wav(directory.path(), 48'000, {1'000'000});
  FileDescriptor input(candidate);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
             "repair close import failed");
  const auto source = session.value()->snapshot().sources.front();
  fs::remove(root / "assets" / (source.sha256 + ".wav"));
  REQUIRE_OK(session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
             "repair close worker did not start");
  session.value()->media_repair().cancel_managed_source_repair();
  session.value().reset();
  session = application::ProjectSession::open(root);
  REQUIRE_OK(session, "repair worker retained writer after session close");
  auto issues = session.value()->media_repair().managed_media_issues();
  REQUIRE_TRUE(issues, "cancelled repair damaged saved source identity");
  REQUIRE_TRUE((issues.value().empty() || issues.value()[0].id == source.id),
               "cancelled repair damaged saved source identity");
}

void managed_media_repair_rejects_replaced_current_after_worker_completion() {
  Directory directory;
  const auto root = directory.path() / "repair-stale-current";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "stale repair fixture create failed");
  const auto old_pointer = read_bytes(root / "state" / "CURRENT");
  const auto candidate = make_wav(directory.path());
  FileDescriptor input(candidate);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
             "stale repair fixture import failed");
  const auto baseline = session.value()->snapshot();
  const auto source = baseline.sources.front();
  fs::remove(root / "assets" / (source.sha256 + ".wav"));
  REQUIRE_OK(session.value()->media_repair().begin_managed_source_repair(source.id, input.get()),
             "stale repair worker did not start");
  wait_managed_repair_ready(*session.value());

  // Simulate an external actor replacing CURRENT after the worker has finished.
  // The old pointer names a valid generation; the in-memory model is untouched.
  const auto replacement = root / "state" / "CURRENT-test-replacement";
  {
    std::ofstream output(replacement, std::ios::binary);
    output.write(reinterpret_cast<const char*>(old_pointer.data()),
                 static_cast<std::streamsize>(old_pointer.size()));
    require(output.good(), "stale CURRENT pointer write failed");
  }
  fs::rename(replacement, root / "state" / "CURRENT");
  auto consumed = session.value()->media_repair().take_managed_source_repair_result();
  if (!consumed)
    throw std::runtime_error("stale repair completion vanished");
  REQUIRE_TRUE(!consumed.value(),
               "stale repair completion claimed a different saved CURRENT or changed the model");
  REQUIRE_EQ(consumed.value().error().code, foundation::ErrorCode::conflict,
             "stale repair completion claimed a different saved CURRENT or changed the model");
  REQUIRE_EQ(session.value()->snapshot().revision, baseline.revision,
             "stale repair completion claimed a different saved CURRENT or changed the model");
  REQUIRE_EQ(session.value()->snapshot().sources, baseline.sources,
             "stale repair completion claimed a different saved CURRENT or changed the model");
  REQUIRE_EQ(session.value()->snapshot().tracks, baseline.tracks,
             "stale repair completion claimed a different saved CURRENT or changed the model");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "stale repair fixture did not retain the replacement CURRENT generation");
  REQUIRE_TRUE(reopened.value()->snapshot().sources.empty(),
               "stale repair fixture did not retain the replacement CURRENT generation");
}

void external_relink_worker_preserves_identity_and_rejects_stale_completion() {
  Directory directory;
  const auto original = make_wav(directory.path());
  const auto root = directory.path() / "external-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "external relink project create failed");
  FileDescriptor input(original);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
             "external relink fixture import failed");
  auto baseline = session.value()->snapshot();
  baseline.revision++;
  baseline.sources.front().external_link = project::ExternalSourceLink{original.string()};
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_OK(writer, "external relink fixture save failed");
    REQUIRE_OK(writer.value().save(baseline, persistence::SaveKind::explicit_save),
               "external relink fixture save failed");
  }
  session = application::ProjectSession::open(root);
  REQUIRE_OK(session, "external relink fixture reopen failed");
  REQUIRE_OK(session.value()->saving().create_named_revision("Before relink"),
             "external relink named baseline failed");
  const auto replacement = directory.path() / "replacement.wav";
  fs::copy_file(original, replacement);
  auto task = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                      replacement.string());
  REQUIRE_OK(task, "external relink begin failed");
  auto prepared = std::async(std::launch::async, [task = std::move(task).value()]() mutable {
                    return std::move(task).run();
                  }).get();
  REQUIRE_OK(prepared, "external relink worker verification failed");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "intervening project edit failed");
  const auto edited_revision = session.value()->snapshot().revision;
  REQUIRE_TRUE(
      !session.value()->imports().finish_external_source_relink(std::move(prepared).value()),
      "stale relink completion changed the project");
  REQUIRE_EQ(session.value()->snapshot().revision, edited_revision,
             "stale relink completion changed the project");
  REQUIRE_EQ(external_path(session.value()->snapshot().sources.front()), original.string(),
             "stale relink completion changed the project");

  task = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                 replacement.string());
  REQUIRE_OK(task, "fresh external relink begin failed");
  prepared = std::move(task).value().run();
  auto playback = session.value()->reserve_playback_start();
  REQUIRE_OK(playback, "active playback accepted a source-location transition");
  REQUIRE_OK(prepared, "active playback accepted a source-location transition");
  REQUIRE_TRUE(
      !session.value()->imports().finish_external_source_relink(std::move(prepared).value()),
      "active playback accepted a source-location transition");
  REQUIRE_EQ(external_path(session.value()->snapshot().sources.front()), original.string(),
             "active playback accepted a source-location transition");
  playback.value().finish_after_join();
  task = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                 replacement.string());
  REQUIRE_OK(task, "post-playback external relink begin failed");
  prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "fresh verified relink failed");
  REQUIRE_TRUE(
      session.value()->imports().finish_external_source_relink(std::move(prepared).value()),
      "fresh verified relink failed");
  const auto relinked = session.value()->snapshot();
  REQUIRE_TRUE(!session.value()->saving().restore_named_revision("Before relink"),
               "named external path restore mutated model before worker verification");
  REQUIRE_EQ(session.value()->snapshot().sources, relinked.sources,
             "named external path restore mutated model before worker verification");
  REQUIRE_EQ(session.value()->snapshot().revision, relinked.revision,
             "named external path restore mutated model before worker verification");
  REQUIRE_EQ(relinked.sources.front().id, baseline.sources.front().id,
             "relink changed stable source identity or clip references");
  REQUIRE_EQ(relinked.sources.front().sha256, baseline.sources.front().sha256,
             "relink changed stable source identity or clip references");
  REQUIRE_EQ(relinked.tracks.front().clips.front().source, baseline.sources.front().id,
             "relink changed stable source identity or clip references");
  REQUIRE_EQ(external_path(relinked.sources.front()), replacement.string(),
             "relink changed stable source identity or clip references");
  REQUIRE_TRUE(session.value()->history().undo(), "relink did not form one undoable model edit");
  REQUIRE_EQ(external_path(session.value()->snapshot().sources.front()), original.string(),
             "relink did not form one undoable model edit");
  REQUIRE_TRUE(!session.value()->saving().save(), "relink did not form one undoable model edit");
  REQUIRE_TRUE(session.value()->history().redo(), "relink did not form one undoable model edit");
  REQUIRE_EQ(external_path(session.value()->snapshot().sources.front()), replacement.string(),
             "relink did not form one undoable model edit");
  REQUIRE_OK(session.value()->saving().save(),
             "redo to CURRENT's path did not save without rehash");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "saved external relink did not reopen with stable references");
  REQUIRE_EQ(reopened.value()->snapshot().sources, relinked.sources,
             "saved external relink did not reopen with stable references");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().clips.front().source,
             baseline.sources.front().id,
             "saved external relink did not reopen with stable references");
}

void external_relink_proof_rechecks_candidate_and_retains_failed_edit() {
  Directory directory;
  const auto original = make_wav(directory.path());
  const auto root = directory.path() / "relink-failure-song";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "external failure fixture create failed");
  FileDescriptor input(original);
  REQUIRE_OK(session.value()->imports().import_wav_audio(input.get()),
             "external failure fixture import failed");
  auto baseline = session.value()->snapshot();
  baseline.revision++;
  baseline.sources.front().external_link = project::ExternalSourceLink{original.string()};
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_TRUE(writer, "external failure fixture save failed");
    REQUIRE_TRUE(writer.value().save(baseline, persistence::SaveKind::explicit_save),
                 "external failure fixture save failed");
  }
  session = application::ProjectSession::open(root);
  REQUIRE_OK(session, "external failure fixture reopen failed");
  const auto candidate = directory.path() / "candidate.wav";
  fs::copy_file(original, candidate);
  {
    std::fstream changed(candidate, std::ios::in | std::ios::out | std::ios::binary);
    changed.seekp(44);
    changed.put('\x7f');
  }
  auto rejected = session.value()->imports().begin_external_source_relink(
      baseline.sources.front().id, candidate.string());
  REQUIRE_OK(rejected, "wrong-byte worker verification changed the model");
  REQUIRE_TRUE(!std::move(rejected).value().run(),
               "wrong-byte worker verification changed the model");
  REQUIRE_EQ(session.value()->snapshot().sources, baseline.sources,
             "wrong-byte worker verification changed the model");
  fs::copy_file(original, candidate, fs::copy_options::overwrite_existing);
  rejected = session.value()->imports().begin_external_source_relink(
      baseline.sources.front().id, (directory.path() / "absent.wav").string());
  REQUIRE_OK(rejected, "missing external candidate passed worker verification");
  REQUIRE_TRUE(!std::move(rejected).value().run(),
               "missing external candidate passed worker verification");
  rejected = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                     candidate.string());
  std::stop_source cancelled;
  cancelled.request_stop();
  REQUIRE_OK(rejected, "cancelled external verification changed the model");
  REQUIRE_TRUE(!std::move(rejected).value().run(cancelled.get_token()),
               "cancelled external verification changed the model");
  REQUIRE_EQ(session.value()->snapshot().sources, baseline.sources,
             "cancelled external verification changed the model");
  auto task = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                      candidate.string());
  REQUIRE_OK(task, "external failure relink begin failed");
  auto prepared = std::move(task).value().run();
  REQUIRE_OK(prepared, "external failure worker verification failed");
  {
    std::fstream changed(candidate, std::ios::in | std::ios::out | std::ios::binary);
    changed.seekp(44);
    changed.put('\x7f');
  }
  auto failed =
      session.value()->imports().finish_external_source_relink(std::move(prepared).value());
  REQUIRE_TRUE(!failed, "changed candidate published or discarded accepted relink edit");
  REQUIRE_EQ(external_path(session.value()->snapshot().sources.front()), candidate.string(),
             "changed candidate published or discarded accepted relink edit");
  REQUIRE_TRUE(!session.value()->saving().save(),
               "changed candidate published or discarded accepted relink edit");
  {
    auto reader = persistence::ProjectStore::open_reader(root);
    REQUIRE_OK(reader, "failed relink reader could not open");
    auto saved = reader.value().load_current();
    REQUIRE_TRUE(saved, "failed relink changed saved CURRENT");
    REQUIRE_EQ(saved.value().snapshot.sources, baseline.sources,
               "failed relink changed saved CURRENT");
  }
  fs::copy_file(original, candidate, fs::copy_options::overwrite_existing);
  task = session.value()->imports().begin_external_source_relink(baseline.sources.front().id,
                                                                 candidate.string());
  REQUIRE_OK(task, "same-path retry relink begin failed");
  prepared = std::move(task).value().run();
  REQUIRE_TRUE(prepared, "same-path worker retry did not save accepted relink edit");
  REQUIRE_TRUE(
      session.value()->imports().finish_external_source_relink(std::move(prepared).value()),
      "same-path worker retry did not save accepted relink edit");
  session.value().reset();
  auto reopened = application::ProjectSession::open(root);
  REQUIRE_TRUE(reopened, "retried relink did not reopen");
  REQUIRE_EQ(external_path(reopened.value()->snapshot().sources.front()), candidate.string(),
             "retried relink did not reopen");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "project session contract passed",
      {
          {"midi_note_sets_move_and_delete_in_one_step",
           midi_note_sets_move_and_delete_in_one_step},
          {"midi_note_copies_land_at_the_drop", midi_note_copies_land_at_the_drop},
          {"creation_reports_an_uncertain_folder_flush",
           creation_reports_an_uncertain_folder_flush},
          {"routing_commands_reopen_and_undo_atomically",
           routing_commands_reopen_and_undo_atomically},
          {"track_automation_points_are_stable_and_undoable",
           track_automation_points_are_stable_and_undoable},
          {"send_automation_points_are_stable_undoable_and_durable",
           send_automation_points_are_stable_undoable_and_durable},
          {"channel_automation_edits_are_undoable_and_durable",
           channel_automation_edits_are_undoable_and_durable},
          {"channel_automation_point_move_keeps_identity_and_timebase",
           channel_automation_point_move_keeps_identity_and_timebase},
          {"automation_segment_curve_command_is_atomic_and_durable",
           automation_segment_curve_command_is_atomic_and_durable},
          {"clip_automation_segment_curve_uses_stable_lane_identity",
           clip_automation_segment_curve_uses_stable_lane_identity},
          {"vst3_parameter_automation_edits_are_undoable_and_durable",
           vst3_parameter_automation_edits_are_undoable_and_durable},
          {"vst3_instrument_parameter_automation_keeps_source_identity",
           vst3_instrument_parameter_automation_keeps_source_identity},
          {"two_bare_vst3_instrument_tracks_reopen_and_refuse_a_third",
           two_bare_vst3_instrument_tracks_reopen_and_refuse_a_third},
          {"paired_vst3_instrument_effect_tracks_can_be_created_in_order",
           paired_vst3_instrument_effect_tracks_can_be_created_in_order},
          {"channel_gestures_coalesce_revisions_into_one_undo_step",
           channel_gestures_coalesce_revisions_into_one_undo_step},
          {"audio_clip_channel_envelope_is_local_undoable_and_durable",
           audio_clip_channel_envelope_is_local_undoable_and_durable},
          {"tempo_events_are_atomic_undoable_and_durable",
           tempo_events_are_atomic_undoable_and_durable},
          {"track_automation_bar_placement_preserves_elapsed_lane",
           track_automation_bar_placement_preserves_elapsed_lane},
          {"clip_automation_timebase_switch_keeps_local_anchor",
           clip_automation_timebase_switch_keeps_local_anchor},
          {"session_commands_and_reopen", session_commands_and_reopen},
          {"session_redo_restores_undone_edit_and_rejects_stale_history",
           session_redo_restores_undone_edit_and_rejects_stale_history},
          {"native_synth_media_import_is_atomic_and_undoable",
           native_synth_media_import_is_atomic_and_undoable},
          {"native_synth_media_import_runs_on_a_worker",
           native_synth_media_import_runs_on_a_worker},
          {"native_effect_media_import_is_atomic_and_undoable",
           native_effect_media_import_is_atomic_and_undoable},
          {"crate_sample_import_sets_state_in_one_step",
           crate_sample_import_sets_state_in_one_step},
          {"accepted_media_import_survives_save_failure",
           accepted_media_import_survives_save_failure},
          {"midi_clip_notes_replace_in_one_undo_step", midi_clip_notes_replace_in_one_undo_step},
          {"native_instrument_state_is_one_undo_step", native_instrument_state_is_one_undo_step},
          {"midi_clip_with_notes_is_one_undo_step", midi_clip_with_notes_is_one_undo_step},
          {"wav_import_is_one_undoable_revision_and_reopens",
           wav_import_is_one_undoable_revision_and_reopens},
          {"wav_import_rejects_rate_mismatch_without_model_edit",
           wav_import_rejects_rate_mismatch_without_model_edit},
          {"wav_import_converts_source_duration_and_reopens",
           wav_import_converts_source_duration_and_reopens},
          {"audio_move_preserves_elapsed_duration_and_reopens",
           audio_move_preserves_elapsed_duration_and_reopens},
          {"move_before_zero_adds_a_bar_before_bar_one",
           move_before_zero_adds_a_bar_before_bar_one},
          {"audio_clip_moves_to_another_track", audio_clip_moves_to_another_track},
          {"audio_duplicate_is_atomic_and_preserves_source",
           audio_duplicate_is_atomic_and_preserves_source},
          {"adjacent_audio_copy_and_delete_are_reversible",
           adjacent_audio_copy_and_delete_are_reversible},
          {"audio_copy_reidentifies_clip_automation", audio_copy_reidentifies_clip_automation},
          {"midi_copy_retains_pattern_and_uses_fresh_note_ids",
           midi_copy_retains_pattern_and_uses_fresh_note_ids},
          {"midi_split_divides_notes_pedals_and_undoes_in_one_step",
           midi_split_divides_notes_pedals_and_undoes_in_one_step},
          {"native_instrument_track_arrives_with_its_sound_in_one_undo_step",
           native_instrument_track_arrives_with_its_sound_in_one_undo_step},
          {"project_key_is_set_cleared_and_undone", project_key_is_set_cleared_and_undone},
          {"presentation_edits_keep_playback_current", presentation_edits_keep_playback_current},
          {"project_key_suggestion_follows_most_of_the_audio",
           project_key_suggestion_follows_most_of_the_audio},
          {"midi_copy_preserves_elapsed_anchor", midi_copy_preserves_elapsed_anchor},
          {"midi_timebase_switch_preserves_events_and_roundtrips_samples",
           midi_timebase_switch_preserves_events_and_roundtrips_samples},
          {"midi_note_removal_is_undoable_and_durable", midi_note_removal_is_undoable_and_durable},
          {"midi_note_move_keeps_identity_duration_and_one_undo_step",
           midi_note_move_keeps_identity_duration_and_one_undo_step},
          {"midi_resize_sets_length_and_keeps_notes_past_the_end",
           midi_resize_sets_length_and_keeps_notes_past_the_end},
          {"midi_loop_switch_repeats_or_stops_in_one_undo_step",
           midi_loop_switch_repeats_or_stops_in_one_undo_step},
          {"elapsed_midi_resize_uses_tempo_map_without_moving_anchor",
           elapsed_midi_resize_uses_tempo_map_without_moving_anchor},
          {"midi_timebase_switch_preserves_tempo_ramp_events",
           midi_timebase_switch_preserves_tempo_ramp_events},
          {"audio_split_preserves_source_fades_and_one_step_undo",
           audio_split_preserves_source_fades_and_one_step_undo},
          {"audio_split_preserves_clip_level_automation",
           audio_split_preserves_clip_level_automation},
          {"audio_split_subdivides_exact_curves_and_rejects_exponential",
           audio_split_subdivides_exact_curves_and_rejects_exponential},
          {"elapsed_audio_split_preserves_musical_curve_under_tempo_ramp",
           elapsed_audio_split_preserves_musical_curve_under_tempo_ramp},
          {"musical_audio_split_uses_exact_tempo_map_boundaries",
           musical_audio_split_uses_exact_tempo_map_boundaries},
          {"audio_slip_moves_only_the_source_window", audio_slip_moves_only_the_source_window},
          {"clip_processing_intent_is_durable_without_unsafe_edit_reinterpretation",
           clip_processing_intent_is_durable_without_unsafe_edit_reinterpretation},
          {"rate_converted_split_and_slip_preserve_renderable_duration",
           rate_converted_split_and_slip_preserve_renderable_duration},
          {"audio_timebase_toggle_preserves_current_sample_placement",
           audio_timebase_toggle_preserves_current_sample_placement},
          {"audio_trim_preserves_source_speed_and_reveals_both_edges",
           audio_trim_preserves_source_speed_and_reveals_both_edges},
          {"audio_trim_converted_source_uses_project_sample_boundaries",
           audio_trim_converted_source_uses_project_sample_boundaries},
          {"audio_trim_rejects_neighbor_overlap_and_stretched_placement",
           audio_trim_rejects_neighbor_overlap_and_stretched_placement},
          {"audio_trim_does_not_reveal_unverified_missing_source",
           audio_trim_does_not_reveal_unverified_missing_source},
          {"accepted_wav_import_survives_save_failure", accepted_wav_import_survives_save_failure},
          {"midi_move_is_one_atomic_undo_step", midi_move_is_one_atomic_undo_step},
          {"elapsed_midi_move_keeps_sample_duration_and_one_undo_step",
           elapsed_midi_move_keeps_sample_duration_and_one_undo_step},
          {"stopped_starter_master_export_survives_session_close",
           stopped_starter_master_export_survives_session_close},
          {"encoded_master_export_keeps_stopped_lease_and_survives_session_close",
           encoded_master_export_keeps_stopped_lease_and_survives_session_close},
          {"stem_export_keeps_stopped_lease_and_survives_session_close",
           stem_export_keeps_stopped_lease_and_survives_session_close},
          {"playback_and_stopped_export_exclude_each_other",
           playback_and_stopped_export_exclude_each_other},
          {"prepared_playback_rejects_a_changed_project_and_live_export_lease",
           prepared_playback_rejects_a_changed_project_and_live_export_lease},
          {"audio_playback_preparation_pins_managed_source_and_rejects_stale_revision",
           audio_playback_preparation_pins_managed_source_and_rejects_stale_revision},
          {"instrument_only_start_passes_requested_buffer",
           instrument_only_start_passes_requested_buffer},
          {"playback_controller_renders_and_retires_with_fake_output",
           playback_controller_renders_and_retires_with_fake_output},
          {"playback_controller_rejects_absent_output_without_retaining_transport",
           playback_controller_rejects_absent_output_without_retaining_transport},
          {"live_audio_trim_restarts_at_completed_boundary",
           live_audio_trim_restarts_at_completed_boundary},
          {"live_trim_past_playhead_requires_no_source_worker",
           live_trim_past_playhead_requires_no_source_worker},
          {"live_audio_reprime_failure_preserves_edit_and_reports_divergence",
           live_audio_reprime_failure_preserves_edit_and_reports_divergence},
          {"live_reprime_device_failure_remains_visible_after_stop",
           live_reprime_device_failure_remains_visible_after_stop},
          {"playback_controller_prepares_and_renders_two_managed_clips",
           playback_controller_prepares_and_renders_two_managed_clips},
          {"failed_second_clip_worker_releases_first_worker_and_transport",
           failed_second_clip_worker_releases_first_worker_and_transport},
          {"playback_preparation_rejects_unbounded_clip_count_before_media_io",
           playback_preparation_rejects_unbounded_clip_count_before_media_io},
          {"playback_preparation_rejects_saved_processing_it_cannot_render",
           playback_preparation_rejects_saved_processing_it_cannot_render},
          {"asynchronous_wav_import_rejects_stale_result_without_publishing",
           asynchronous_wav_import_rejects_stale_result_without_publishing},
          {"asynchronous_wav_import_commits_one_undo_step_and_reopens",
           asynchronous_wav_import_commits_one_undo_step_and_reopens},
          {"asynchronous_wav_import_can_insert_above_existing_track",
           asynchronous_wav_import_can_insert_above_existing_track},
          {"asynchronous_wav_import_cancellation_and_rejection_leave_no_asset",
           asynchronous_wav_import_cancellation_and_rejection_leave_no_asset},
          {"asynchronous_wav_import_rejects_foreign_writer_and_swapped_directory",
           asynchronous_wav_import_rejects_foreign_writer_and_swapped_directory},
          {"asynchronous_wav_import_retains_receipt_after_failed_save",
           asynchronous_wav_import_retains_receipt_after_failed_save},
          {"asynchronous_duplicate_import_restores_missing_managed_asset",
           asynchronous_duplicate_import_restores_missing_managed_asset},
          {"named_revision_restore_is_saved_undoable_and_never_repoints_name",
           named_revision_restore_is_saved_undoable_and_never_repoints_name},
          {"named_revision_rejects_bad_media_and_keeps_failed_save_undoable",
           named_revision_rejects_bad_media_and_keeps_failed_save_undoable},
          {"named_revision_rejects_rate_change_without_changing_current",
           named_revision_rejects_rate_change_without_changing_current},
          {"managed_media_repair_preserves_identity_and_reopens",
           managed_media_repair_preserves_identity_and_reopens},
          {"managed_media_repair_cancel_joins_before_writer_close",
           managed_media_repair_cancel_joins_before_writer_close},
          {"managed_media_repair_rejects_replaced_current_after_worker_completion",
           managed_media_repair_rejects_replaced_current_after_worker_completion},
          {"external_relink_worker_preserves_identity_and_rejects_stale_completion",
           external_relink_worker_preserves_identity_and_rejects_stale_completion},
          {"external_relink_proof_rechecks_candidate_and_retains_failed_edit",
           external_relink_proof_rechecks_candidate_and_retains_failed_edit},
#if OSCILLATE_HAVE_CODEC
          {"encoded_import_reopens_and_deduplicates", encoded_import_reopens_and_deduplicates},
          {"encoded_import_rejects_cancel_stale_and_malformed",
           encoded_import_rejects_cancel_stale_and_malformed},
          {"encoded_import_retains_accepted_edit_after_save_failure",
           encoded_import_retains_accepted_edit_after_save_failure},
#else
          {"encoded_import_is_explicitly_unavailable_without_codec",
           encoded_import_is_explicitly_unavailable_without_codec},
#endif
      });
}
