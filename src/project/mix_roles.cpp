#include "oscillate/project/mix_roles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace oscillate::project {
namespace {

struct Keyword {
  std::string_view word;
  MixRole role;
};

// Checked in role order: drums, then vocals, then bass.
constexpr std::array keywords{
    Keyword{"drum", MixRole::drums},      Keyword{"drums", MixRole::drums},
    Keyword{"kick", MixRole::drums},      Keyword{"kicks", MixRole::drums},
    Keyword{"bd", MixRole::drums},        Keyword{"snare", MixRole::drums},
    Keyword{"sd", MixRole::drums},        Keyword{"clap", MixRole::drums},
    Keyword{"claps", MixRole::drums},     Keyword{"hat", MixRole::drums},
    Keyword{"hats", MixRole::drums},      Keyword{"hh", MixRole::drums},
    Keyword{"hihat", MixRole::drums},     Keyword{"cymbal", MixRole::drums},
    Keyword{"crash", MixRole::drums},     Keyword{"ride", MixRole::drums},
    Keyword{"tom", MixRole::drums},       Keyword{"toms", MixRole::drums},
    Keyword{"perc", MixRole::drums},      Keyword{"percussion", MixRole::drums},
    Keyword{"shaker", MixRole::drums},    Keyword{"tamb", MixRole::drums},
    Keyword{"rim", MixRole::drums},       Keyword{"conga", MixRole::drums},
    Keyword{"bongo", MixRole::drums},     Keyword{"break", MixRole::drums},
    Keyword{"breaks", MixRole::drums},    Keyword{"vocal", MixRole::vocals},
    Keyword{"vocals", MixRole::vocals},   Keyword{"vox", MixRole::vocals},
    Keyword{"voc", MixRole::vocals},      Keyword{"voice", MixRole::vocals},
    Keyword{"choir", MixRole::vocals},    Keyword{"adlib", MixRole::vocals},
    Keyword{"acapella", MixRole::vocals}, Keyword{"singer", MixRole::vocals},
    Keyword{"rap", MixRole::vocals},      Keyword{"bass", MixRole::bass},
    Keyword{"sub", MixRole::bass},        Keyword{"subbass", MixRole::bass},
    Keyword{"reese", MixRole::bass},
};
constexpr std::array name_order{MixRole::drums, MixRole::vocals, MixRole::bass};

[[nodiscard]] std::vector<std::string> words(std::string_view name) {
  std::vector<std::string> out;
  std::string word;
  const auto flush = [&] {
    if (!word.empty())
      out.push_back(std::move(word));
    word.clear();
  };
  bool digits = false;
  for (const char raw : name) {
    const auto c = static_cast<unsigned char>(raw);
    if (std::isalpha(c) != 0) {
      if (digits)
        flush();
      digits = false;
      word.push_back(static_cast<char>(std::tolower(c)));
    } else if (std::isdigit(c) != 0) {
      if (!digits)
        flush();
      digits = true;
      word.push_back(static_cast<char>(c));
    } else {
      flush();
    }
  }
  flush();
  return out;
}

[[nodiscard]] std::optional<MixRole> device_role(const Track& track) {
  const auto* patch = std::get_if<NativeInstrumentPatch>(&track.instrument_source);
  if (patch == nullptr)
    return std::nullopt;
  if (std::ranges::find(drum_engines, patch->engine) != drum_engines.end())
    return MixRole::drums;
  if (std::ranges::find(bass_engines, patch->engine) != bass_engines.end())
    return MixRole::bass;
  return std::nullopt;
}

// The role most of its clips' names agree on (ties go in name order).
[[nodiscard]] std::optional<MixRole> clip_names_role(const Track& track) {
  std::array<std::size_t, mix_role_count> votes{};
  for (const auto& clip : track.clips) {
    if (const auto role = role_from_name(clip.name))
      ++votes[static_cast<std::size_t>(*role)];
  }
  std::optional<MixRole> best;
  std::size_t most = 0;
  for (const auto role : name_order) {
    if (votes[static_cast<std::size_t>(role)] > most) {
      most = votes[static_cast<std::size_t>(role)];
      best = role;
    }
  }
  return best;
}

[[nodiscard]] bool low_notes(const Track& track) {
  std::vector<std::uint8_t> keys;
  for (const auto& clip : track.midi_clips) {
    for (const auto& note : clip.notes)
      keys.push_back(note.key);
  }
  if (keys.size() < bass_min_notes)
    return false;
  std::ranges::sort(keys);
  const auto top_index =
      static_cast<std::size_t>(bass_top_share * static_cast<double>(keys.size() - 1));
  return keys[keys.size() / 2] < bass_median_key && keys[top_index] < bass_top_key;
}

struct Inferred {
  MixRole role;
  RoleReason reason;
};

[[nodiscard]] Inferred source_role(const Track& track) {
  if (const auto role = device_role(track))
    return {*role, RoleReason::device};
  if (const auto role = role_from_name(track.name))
    return {*role, RoleReason::name};
  if (const auto role = clip_names_role(track))
    return {*role, RoleReason::name};
  if (track.kind == TrackKind::audio && track.heard_role)
    return {*track.heard_role, RoleReason::heard};
  if (track.kind == TrackKind::instrument && low_notes(track))
    return {MixRole::bass, RoleReason::notes};
  return {MixRole::music, RoleReason::fallback};
}

} // namespace

std::optional<MixRole> role_from_name(std::string_view name) {
  const auto found = words(name);
  for (const auto role : name_order) {
    for (const auto& word : found) {
      if (std::ranges::any_of(keywords, [&](const Keyword& keyword) {
            return keyword.role == role && keyword.word == word;
          }))
        return role;
    }
  }
  return std::nullopt;
}

std::vector<TrackRole> mix_roles(const Snapshot& snapshot) {
  const auto& tracks = snapshot.tracks;
  std::map<TrackId, std::size_t> index;
  for (std::size_t i = 0; i < tracks.size(); ++i)
    index.emplace(tracks[i].id, i);
  // The group bus a track plays into, if any.
  const auto parent = [&](std::size_t i) -> std::optional<std::size_t> {
    if (!tracks[i].main_output)
      return std::nullopt;
    const auto found = index.find(*tracks[i].main_output);
    if (found == index.end() || tracks[found->second].kind != TrackKind::group_bus)
      return std::nullopt;
    return found->second;
  };
  std::vector<std::vector<std::size_t>> children(tracks.size());
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (const auto up = parent(i))
      children[*up].push_back(i);
  }

  std::vector<TrackRole> roles;
  roles.reserve(tracks.size());
  for (const auto& track : tracks)
    roles.push_back({track.id});
  std::vector<bool> done(tracks.size(), false);
  // A saved route never loops (validation), so the recursion ends within
  // the track count.
  std::function<void(std::size_t)> resolve = [&](std::size_t i) {
    if (done[i])
      return;
    const auto& track = tracks[i];
    auto& out = roles[i];
    done[i] = true;
    if (track.mix_role) {
      out.role = *track.mix_role;
      out.reason = RoleReason::chosen;
      return;
    }
    for (auto up = parent(i); up; up = parent(*up)) {
      if (tracks[*up].mix_role) {
        out.role = *tracks[*up].mix_role;
        out.reason = RoleReason::group;
        return;
      }
    }
    switch (track.kind) {
    case TrackKind::audio:
    case TrackKind::instrument: {
      const auto inferred = source_role(track);
      out.role = inferred.role;
      out.reason = inferred.reason;
      return;
    }
    case TrackKind::group_bus: {
      std::optional<MixRole> shared;
      bool mixed = false;
      for (const auto child : children[i]) {
        resolve(child);
        const auto role = roles[child].role;
        if (role == MixRole::none || (shared && *shared != role))
          mixed = true;
        shared = role;
      }
      if (shared && !mixed) {
        out.role = *shared;
        out.reason = RoleReason::summed;
      } else if (const auto named = role_from_name(track.name)) {
        out.role = *named;
        out.reason = RoleReason::name;
      } else {
        out.role = MixRole::none;
        out.reason = RoleReason::fallback;
      }
      return;
    }
    case TrackKind::return_bus:
      out.role = MixRole::none;
      out.reason = RoleReason::fallback;
      return;
    }
  };
  for (std::size_t i = 0; i < tracks.size(); ++i)
    resolve(i);
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (track_holds_clips(tracks[i].kind))
      roles[i].stage = roles[i].role;
  }
  return roles;
}

} // namespace oscillate::project
