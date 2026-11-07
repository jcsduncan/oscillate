#include "native_synth_session_adapter.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

template <typename T> T take(foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-prism-adapter-XXXXXX").string();
    auto* made = ::mkdtemp(pattern.data());
    require(made != nullptr, "temporary directory failed");
    path_ = made;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

std::size_t cutoff() {
  return *synth::find_parameter(*synth::default_engine().descriptor, "prism.filter.cutoff");
}

void edits_drags_cancels_and_status() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(application::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  std::vector<std::uint64_t> offered;
  std::vector<double> heard;
  std::optional<quick::AudibleSynthState> audible;
  quick::SessionNativeSynthAdapter adapter(
      *session, track, synth::default_engine(),
      [&](std::uint64_t revision) { offered.push_back(revision); }, [&] { return audible; }, {}, {},
      [&](std::span<const double> live) {
        heard.push_back(live[cutoff()]);
        return true;
      });

  auto values = adapter.values();
  require(values == synth::default_values(*synth::default_engine().descriptor),
          "the adapter reads the synth's dense values");

  values[cutoff()] = 1'000.0;
  adapter.submit(values, quick::NativeSynthGesture::single);
  require(adapter.values()[cutoff()] == 1'000.0 && offered.size() == 1,
          "a single edit applies and is offered to playback");
  require(!session->saving().has_unsaved_changes(), "a single edit is saved");

  // A drag is heard live and shown, but saves nothing until its release.
  const auto before_drag = session->snapshot().revision;
  adapter.submit(values, quick::NativeSynthGesture::begin);
  for (double hz : {900.0, 700.0, 500.0}) {
    values[cutoff()] = hz;
    adapter.submit(values, quick::NativeSynthGesture::update);
  }
  require(heard == std::vector<double>{900.0, 700.0, 500.0} &&
              adapter.values()[cutoff()] == 500.0 && session->snapshot().revision == before_drag &&
              offered.size() == 1,
          "every drag step is heard and shown without a saved edit");
  values[cutoff()] = 400.0;
  adapter.submit(values, quick::NativeSynthGesture::commit);
  require(adapter.values()[cutoff()] == 400.0 && offered.size() == 2 &&
              session->snapshot().revision == before_drag + 1,
          "the release saves one edit");
  require(!session->saving().has_unsaved_changes(), "a committed drag is saved");
  require(session->history().undo().has_value() && adapter.values()[cutoff()] == 1'000.0,
          "a whole drag undoes as one step");
  require(session->history().redo().has_value(), "redo restores the drag");

  adapter.submit(values, quick::NativeSynthGesture::begin);
  values[cutoff()] = 3'000.0;
  adapter.submit(values, quick::NativeSynthGesture::update);
  heard.clear();
  adapter.submit(values, quick::NativeSynthGesture::cancel);
  require(adapter.values()[cutoff()] == 400.0 && heard == std::vector<double>{400.0},
          "cancel restores, and is heard returning to, the value from before begin");
  require(session->history().undo().has_value() && adapter.values()[cutoff()] == 1'000.0,
          "cancel leaves no extra undo step behind");

  auto status = adapter.status();
  require(status.editable && status.audible_revision == status.saved_revision,
          "stopped playback reports saved values as audible");
  audible = quick::AudibleSynthState{status.saved_revision - 1, true};
  status = adapter.status();
  require(status.audible_revision + 1 == status.saved_revision && status.deferred_changes,
          "playback status reports pending and deferred changes honestly");

  auto bad = values;
  bad[cutoff()] = 1e9;
  adapter.submit(bad, quick::NativeSynthGesture::single);
  require(!adapter.status().message.empty(), "a rejected edit explains itself");
}

void a_panel_for_another_engine_is_read_only() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto plain = take(session->instruments().add_synth_track());
  // A second engine view of the same track: a copied descriptor with another id.
  synth::EngineDescriptor other = *synth::default_engine().descriptor;
  other.id = "other";
  other.name = "Other";
  const synth::SynthEngine other_engine{&other, synth::default_engine().create_kernel,
                                        synth::default_engine().tail_frames};
  quick::SessionNativeSynthAdapter adapter(*session, plain, other_engine);
  auto status = adapter.status();
  require(!status.editable && status.message.find("prism") != std::string::npos,
          "a panel for another engine explains why it cannot edit");
  require(adapter.values() == synth::default_values(other), "it shows its own table's defaults");
  const auto revision = session->snapshot().revision;
  adapter.submit(adapter.values(), quick::NativeSynthGesture::single);
  require(session->snapshot().revision == revision, "it never edits the other engine's patch");
}

void deferred_panel_saves_keep_latest_revision_and_reopen() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(application::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  int requests = 0;
  quick::SessionNativeSynthAdapter adapter(*session, track, synth::default_engine(), {}, {}, {},
                                           [&] { ++requests; });
  auto values = adapter.values();
  values[cutoff()] = 900.0;
  adapter.submit(values, quick::NativeSynthGesture::single);
  require(requests == 1 && session->saving().has_unsaved_changes() &&
              adapter.status().persistence_pending,
          "a panel edit requests a worker save without flushing on the UI thread");
  {
    auto first = take(session->saving().begin_background_save());
    const auto first_revision = first.revision();
    values[cutoff()] = 700.0;
    adapter.submit(values, quick::NativeSynthGesture::single);
    require(requests == 2 && session->snapshot().revision > first_revision,
            "the next edit remains responsive while an older snapshot is captured");
    auto first_outcome = take(std::move(first).run());
    session->saving().finish_background_save(first_revision, first_outcome);
  }
  require(session->saving().has_unsaved_changes(),
          "an older save must not mark a newer edit durable");
  {
    auto latest = take(session->saving().begin_background_save());
    const auto latest_revision = latest.revision();
    auto latest_outcome = take(std::move(latest).run());
    session->saving().finish_background_save(latest_revision, latest_outcome);
  }
  require(!session->saving().has_unsaved_changes() && !adapter.status().persistence_pending,
          "the latest completed save clears the pending indicator");
  values[cutoff()] = 600.0;
  adapter.submit(values, quick::NativeSynthGesture::single);
  {
    auto stale = take(session->saving().begin_background_save());
    values[cutoff()] = 700.0;
    adapter.submit(values, quick::NativeSynthGesture::single);
    require(session->saving().save().has_value(), "newer explicit save failed");
    require(!std::move(stale).run(), "a delayed worker regressed a newer CURRENT");
  }
  session.reset();
  auto reopened = take(application::ProjectSession::open(root));
  require(take(reopened->instruments().native_instrument_values(track))[cutoff()] == 700.0,
          "the latest panel value survives reopen");
}

void cancelling_never_edits_the_project() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  quick::SessionNativeSynthAdapter adapter(*session, track, synth::default_engine());
  const double before = adapter.values()[cutoff()];

  auto dragged = adapter.values();
  dragged[cutoff()] = 1'000.0;
  adapter.submit(dragged, quick::NativeSynthGesture::begin);
  adapter.submit(dragged, quick::NativeSynthGesture::update);
  require(session->tracks().set_track_muted(track, true).has_value(),
          "unrelated mute edit applies");
  dragged[cutoff()] = 900.0;
  adapter.submit(dragged, quick::NativeSynthGesture::update);
  const auto revision = session->snapshot().revision;
  adapter.submit(dragged, quick::NativeSynthGesture::cancel);
  require(adapter.values()[cutoff()] == before && session->snapshot().revision == revision &&
              session->snapshot().tracks.front().muted,
          "cancel restores the synth value and keeps the unrelated edit, saving nothing");
  require(session->history().undo().has_value() && !session->snapshot().tracks.front().muted &&
              adapter.values()[cutoff()] == before,
          "the drag left no undo step of its own");

  // Another editor's change to the same synth during the drag survives a cancel.
  auto external = adapter.values();
  external[cutoff()] = 2'000.0;
  auto next = external;
  next[cutoff()] = 3'000.0;
  adapter.submit(next, quick::NativeSynthGesture::begin);
  adapter.submit(next, quick::NativeSynthGesture::update);
  require(session->instruments().set_native_instrument_values(track, external).has_value(),
          "another synth edit applies during the gesture");
  const auto edited = session->snapshot().revision;
  adapter.submit(next, quick::NativeSynthGesture::cancel);
  require(session->snapshot().revision == edited && adapter.values()[cutoff()] == 2'000.0,
          "cancel does not undo or overwrite the other synth edit");
}

// Crate's seam (ADR-0063): values and document as one undo step, a drag of
// them as one, a built-in sample reported as such, and a clip at a bar.
void crate_state_media_and_clips() {
  namespace crate = synth::crate;
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track =
      take(session->instruments().add_native_instrument_track(crate::engine_id, std::nullopt));
  quick::SessionNativeSynthAdapter adapter(*session, track, crate::engine());
  const auto root = synth::find_parameter(*crate::engine().descriptor, "crate.root").value();

  project::NativeSynthDocument document;
  document.media.push_back(
      {std::string(crate::sample_slot), project::NativeBuiltinMedia{"kick"}, 0});
  document.regions.push_back({0, std::string(crate::sample_slot), 0, "kick", "Kick"});
  document.regions.push_back({5, std::string(crate::sample_slot), 100, "other", "Chop"});
  auto values = adapter.values();
  values[root] = 50.0;
  const auto before = session->snapshot().revision;
  const auto document_before = adapter.document();
  adapter.submit_state(values, document, quick::NativeSynthGesture::single);
  require(adapter.status().message.empty() && session->snapshot().revision == before + 1 &&
              adapter.values()[root] == 50.0 && adapter.document() == document,
          "values and document land as one revision");
  const auto media = adapter.media(crate::sample_slot);
  require(media.builtin && media.identity == "kick" && !media.audio && !media.decoding,
          "a built-in sample is reported by name");

  auto moved = document;
  adapter.submit_state(values, moved, quick::NativeSynthGesture::begin);
  moved.regions[1].start_frame = 120;
  adapter.submit_state(values, moved, quick::NativeSynthGesture::update);
  moved.regions[1].start_frame = 140;
  values[root] = 52.0;
  adapter.submit_state(values, moved, quick::NativeSynthGesture::commit);
  require(adapter.document().regions[1].start_frame == 140 && adapter.values()[root] == 52.0,
          "a state drag commits its last step");
  require(session->history().undo().has_value() && adapter.document() == document &&
              adapter.values()[root] == 50.0,
          "the drag is one undo step");
  require(session->history().undo().has_value() && adapter.document() == document_before &&
              adapter.values()[root] != 50.0,
          "the state edit is one undo step");

  constexpr auto quarter = timeline::ticks_per_quarter;
  const auto placeholder = project::NoteId::from_bits({0, 1}).value();
  require(adapter.add_midi_clip(1, {4 * quarter},
                                {{placeholder, {0}, {quarter}, crate::first_pad_key, 100}}),
          "a clip is added");
  const auto& clips = session->snapshot().tracks.front().midi_clips;
  require(clips.size() == 1 && clips.front().notes.size() == 1 &&
              std::get<project::MusicalInterval>(clips.front().placement).start.value ==
                  4 * quarter,
          "the clip starts at zero-based bar 1 of 4/4");
  require(!adapter.add_midi_clip(-1, {quarter}, {}), "a negative bar is refused");
}

} // namespace

int main() {
  try {
    edits_drags_cancels_and_status();
    a_panel_for_another_engine_is_read_only();
    deferred_panel_saves_keep_latest_revision_and_reopen();
    cancelling_never_edits_the_project();
    crate_state_media_and_clips();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native synth session adapter contract passed\n";
  return 0;
}
