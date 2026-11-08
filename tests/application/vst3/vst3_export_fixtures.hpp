#pragma once

#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <variant>

// Shared by the VST3 export suites, whose fixtures save their projects under
// a directory's "project" folder.
namespace test_vst3 {

// The store of the project a fixture saved under `directory`.
template <typename Directory>
[[nodiscard]] oscillate::persistence::ProjectStore store_at(const Directory& directory) {
  return oscillate::test::take(
      oscillate::persistence::ProjectStore::open_reader(directory.path / "project"));
}

// The first VST3 instrument track: a one-source project's stem.
[[nodiscard]] inline oscillate::project::TrackId
first_instrument(const oscillate::project::Snapshot& snapshot) {
  const auto found = std::ranges::find_if(snapshot.tracks, [](const auto& track) {
    return std::holds_alternative<oscillate::project::Vst3PluginInstance>(track.instrument_source);
  });
  if (found == snapshot.tracks.end())
    throw std::runtime_error("the fixture has no VST3 instrument track");
  return found->id;
}

} // namespace test_vst3
