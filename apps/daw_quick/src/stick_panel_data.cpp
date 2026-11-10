#include "stick_panel_data.hpp"

#include "oscillate/foundation/value_text.hpp"

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace oscillate::quick::stick_panel {
namespace {

namespace stick = native_dsp::synth::stick;
using stick::Articulation;
using stick::Piece;

constexpr double bytes_per_mb = 1024.0 * 1024.0;

QString text(std::string_view view) {
  return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

QString state_name(stick::library::State state) {
  switch (state) {
  case stick::library::State::ready:
    return QStringLiteral("ready");
  case stick::library::State::invalid:
    return QStringLiteral("invalid");
  case stick::library::State::loading:
    return QStringLiteral("loading");
  case stick::library::State::missing:
    break;
  }
  return QStringLiteral("missing");
}

// In Piece order: the parameter ids' middle and the panel's names.
constexpr std::array<std::string_view, stick::piece_count> piece_ids{
    "kick", "snare",  "hihat",  "tom1", "tom2",   "tom3", "tom4",
    "tom5", "crash1", "crash2", "ride", "splash", "china"};
constexpr std::array<std::string_view, stick::piece_count> piece_names{
    "Kick",  "Snare",   "Hi-hat",  "Tom 1", "Tom 2",  "Tom 3", "Tom 4",
    "Tom 5", "Crash 1", "Crash 2", "Ride",  "Splash", "China"};
// In MicGroup order.
constexpr std::array<std::string_view, stick::mic_group_count> group_ids{
    "kick_in", "kick_out",  "kick_sub", "snare_top", "snare_bottom", "toms",    "hihat",
    "ride",    "overheads", "room",     "far_room",  "mid_side",     "china_oh"};
constexpr std::array<std::string_view, stick::mic_group_count> group_names{
    "Kick In", "Kick Out",  "Kick Sub", "Snare Top", "Snare Btm", "Toms",    "Hi-hat",
    "Ride",    "Overheads", "Room",     "Far Room",  "Mid-Side",  "China OH"};
// What each articulation adds to a piece's description (the plain stroke
// nothing; the second drum names its piece); in Articulation order.
constexpr std::array<std::string_view, stick::articulation_count> articulation_names{
    "", "second", "side-stick", "bell", "closed", "half open", "open", "pedal"};

// Key names as the piano roll gives them, C-1 at MIDI 0: "C2" for 36.
constexpr std::array<std::string_view, 12> pitch_names{"C",  "C♯", "D",  "D♯", "E",  "F",
                                                       "F♯", "G",  "G♯", "A",  "A♯", "B"};
constexpr int octave_of_key_zero = -1;
// The lowest key that may play a piece: the lowest tom (F2).
constexpr int lowest_tom_key = 41;
constexpr std::uint32_t every_articulation = (1U << stick::articulation_count) - 1U;

QString key_name(int key) {
  const int pitches = static_cast<int>(pitch_names.size());
  // A whole octave number; the formatter gives a negative one its minus sign.
  const int octave = key / pitches + octave_of_key_zero;
  return text(pitch_names[static_cast<std::size_t>(key % pitches)]) +
         QString::fromStdString(foundation::format_number(octave, 0));
}

// The key a piece's plain stroke plays (the hi-hat's at the pedal), the
// lowest of them; empty when none does.
QString main_key(Piece piece, std::size_t toms) {
  for (int key = std::min(stick::first_key, lowest_tom_key); key <= stick::last_key; ++key) {
    const auto mapped = stick::map_key(key, toms);
    if (mapped && mapped->piece == piece &&
        (mapped->articulation == Articulation::hit || mapped->at_pedal)) {
      return key_name(key);
    }
  }
  return {};
}

// The keys a piece plays on a kit with `toms` toms, lowest first: those
// whose articulation it recorded (`recorded`, a bit per Articulation; every
// one while the kit is not loaded).
QStringList keys_of(Piece piece, std::size_t toms, std::uint32_t recorded) {
  QStringList keys;
  for (int key = std::min(stick::first_key, lowest_tom_key); key <= stick::last_key; ++key) {
    const auto mapped = stick::map_key(key, toms);
    if (mapped && mapped->piece == piece &&
        (mapped->at_pedal ||
         (recorded & (1U << static_cast<unsigned>(mapped->articulation))) != 0U)) {
      keys.push_back(key_name(key));
    }
  }
  return keys;
}

} // namespace

QVariantMap library(stick::Kit kit) {
  const auto status = stick::library::status(kit);
  const auto& facts = stick::library::facts(kit);
  QVariantList kits;
  for (std::size_t k = 0; k < stick::kit_count; ++k) {
    const auto& other = stick::library::kits[k];
    std::error_code error;
    const bool installed = std::filesystem::exists(
        stick::library::kit_directory(static_cast<stick::Kit>(k)) / stick::library::manifest_file,
        error);
    kits.push_back(QVariantMap{{QStringLiteral("name"), text(other.name)},
                               {QStringLiteral("description"), text(other.description)},
                               {QStringLiteral("installed"), installed}});
  }
  return {
      {QStringLiteral("state"), state_name(status.state)},
      {QStringLiteral("message"), QString::fromStdString(status.message)},
      {QStringLiteral("directory"), QString::fromStdString(status.directory.string())},
      {QStringLiteral("name"), QString::fromStdString(status.name)},
      {QStringLiteral("attribution"), QString::fromStdString(status.attribution)},
      {QStringLiteral("install_command"), text(stick::library::install_command) +
                                              QStringLiteral(" --kit ") + text(facts.install_flag)},
      {QStringLiteral("required"),
       text(facts.name) + QStringLiteral(" · ") + text(facts.attribution)},
      {QStringLiteral("resident_mb"), static_cast<double>(status.resident_bytes) / bytes_per_mb},
      {QStringLiteral("pack_mb"), static_cast<double>(status.pack_bytes) / bytes_per_mb},
      {QStringLiteral("samples"), static_cast<int>(status.samples)},
      {QStringLiteral("mics"), static_cast<int>(status.mics)},
      {QStringLiteral("licence_url"), text(stick::library::licence_url)},
      {QStringLiteral("kits"), kits}};
}

QVariantMap layout(stick::Kit kit) {
  const auto& facts = stick::library::facts(kit);
  const auto contents = stick::library::contents(kit);
  const bool loaded = std::ranges::any_of(contents.hits, [](auto hits) { return hits > 0; });
  const std::size_t toms = loaded ? contents.toms : facts.toms;
  QVariantList groups;
  for (std::size_t g = 0; g < stick::mic_group_count; ++g) {
    groups.push_back(QVariantMap{{QStringLiteral("group"), static_cast<int>(g)},
                                 {QStringLiteral("id"), text(group_ids[g])},
                                 {QStringLiteral("name"), text(group_names[g])},
                                 {QStringLiteral("channels"), static_cast<int>(contents.mics[g])}});
  }
  QVariantList pieces;
  for (std::size_t p = 0; p < stick::piece_count; ++p) {
    const auto piece = static_cast<Piece>(p);
    QStringList articulations;
    for (std::size_t a = 0; a < stick::articulation_count; ++a) {
      if ((contents.articulations[p] & (1U << a)) == 0U || articulation_names[a].empty()) {
        continue;
      }
      articulations.push_back(static_cast<Articulation>(a) == Articulation::alt
                                  ? text(articulation_names[a]) + QLatin1Char(' ') +
                                        text(piece_names[p]).toLower()
                                  : text(articulation_names[a]));
    }
    if (contents.brushes[p]) {
      articulations.push_back(QStringLiteral("brushes"));
    }
    const bool present =
        loaded ? contents.hits[p] > 0 : (facts.pieces & stick::piece_bit(piece)) != 0;
    pieces.push_back(
        QVariantMap{{QStringLiteral("piece"), static_cast<int>(p)},
                    {QStringLiteral("id"), text(piece_ids[p])},
                    {QStringLiteral("name"), text(piece_names[p])},
                    {QStringLiteral("hits"), static_cast<int>(contents.hits[p])},
                    {QStringLiteral("articulations"), articulations.join(QStringLiteral(", "))},
                    {QStringLiteral("keys"),
                     keys_of(piece, toms, loaded ? contents.articulations[p] : every_articulation)
                         .join(QStringLiteral(", "))},
                    {QStringLiteral("key"), main_key(piece, toms)},
                    {QStringLiteral("brushes"), contents.brushes[p]},
                    {QStringLiteral("present"), present}});
  }
  return {{QStringLiteral("kit"), static_cast<int>(kit)},
          {QStringLiteral("loaded"), loaded},
          {QStringLiteral("toms"), static_cast<int>(toms)},
          {QStringLiteral("groups"), groups},
          {QStringLiteral("pieces"), pieces}};
}

void decode(std::span<const float> frame, QVariantMap& out) {
  namespace t = stick::telemetry;
  if (frame.size() < t::size) {
    return;
  }
  const auto at = [&](std::size_t index) { return static_cast<double>(frame[index]); };
  const auto whole = [&](std::size_t index) { return static_cast<int>(std::lround(at(index))); };
  out[QStringLiteral("library")] = state_name(static_cast<stick::library::State>(
      std::clamp(whole(t::library), 0, static_cast<int>(stick::library::State::loading))));
  out[QStringLiteral("kit")] = whole(t::kit);
  out[QStringLiteral("hits")] = whole(t::hits);
  out[QStringLiteral("voices")] = whole(t::voices);
  out[QStringLiteral("output_peak_db")] = at(t::output_peak_db);
  out[QStringLiteral("pedal")] = at(t::pedal);
  out[QStringLiteral("last_piece")] = whole(t::last_piece);
  out[QStringLiteral("underruns")] = whole(t::underruns);
  QVariantList levels;
  for (std::size_t p = 0; p < stick::piece_count; ++p) {
    levels.push_back(at(t::piece_level + p));
  }
  out[QStringLiteral("piece_levels")] = levels;
  QVariantList meters;
  for (std::size_t g = 0; g < stick::mic_group_count; ++g) {
    meters.push_back(at(t::mic_db + g));
  }
  out[QStringLiteral("mic_db")] = meters;
}

} // namespace oscillate::quick::stick_panel
