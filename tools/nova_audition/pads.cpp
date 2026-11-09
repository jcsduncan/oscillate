#include "pads.hpp"

#include "audition.hpp"
#include "parts.hpp"

#include "oscillate/application/midi_file_import.hpp"
#include "oscillate/application/project_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::tools::pads {
namespace {

namespace fs = std::filesystem;
using audition::done;
using audition::fail;
using audition::preset_index;
using audition::read_file;
using audition::synth;
using audition::take;

using parts::beats_per_bar;
using parts::firm;
using parts::key;
using parts::medium;
using parts::Note;
using parts::soft;
using P = parts::Pitch;

// The pad starts alone; the lead comes in after these bars.
constexpr int lead_in_bars = 2;
// The pad's clip: the lead-in, the lead's eight bars and two to close.
constexpr int part_bars = 12;
constexpr std::uint32_t render_rate = 48'000;
constexpr std::string_view lead_engine = "wren";
constexpr std::string_view lead_preset = "Lyrical Air";

// Single strikes and the santur's quick double strokes, sparse and off the
// beat, leaving the tails to ring under the lead.
std::vector<Note> santur_part() {
  constexpr double strike = 0.2;
  return {
      {0.5, strike, key(P::c, 4), firm},     {0.75, strike, key(P::g, 4), medium},
      {2.25, strike, key(P::eb, 4), medium}, {4.0, strike, key(P::g, 3), soft},
      {5.75, strike, key(P::c, 5), medium},  {9.0, strike, key(P::c, 3), firm},
      {9.0, strike, key(P::c, 4), medium},   {10.75, strike, key(P::g, 4), soft},
      {13.5, strike, key(P::ab, 4), medium}, {13.75, strike, key(P::ab, 4), soft},
      {17.25, strike, key(P::c, 5), firm},   {17.5, strike, key(P::g, 4), medium},
      {20.75, strike, key(P::eb, 4), soft},  {24.5, strike, key(P::d, 5), medium},
      {24.75, strike, key(P::g, 4), soft},   {28.0, strike, key(P::c, 3), firm},
      {28.0, strike, key(P::c, 4), medium},  {31.25, strike, key(P::g, 4), medium},
      {31.5, strike, key(P::c, 5), soft},    {35.0, strike, key(P::ab, 4), medium},
      {38.75, strike, key(P::c, 4), firm},   {39.0, strike, key(P::g, 3), soft},
      {42.5, strike, key(P::g, 4), medium},  {42.75, strike, key(P::c, 5), soft},
      {44.75, strike, key(P::c, 4), soft},
  };
}

struct Audition {
  std::string_view name; // "<NN direction>"
  parts::Pad pad;
};

const std::vector<Audition>& auditions() {
  static const std::vector<Audition> list{
      {"02 breath", parts::ney_breath},
      {"03 santur", {"nova", "Santur Bloom", "Shimmer Heaven", parts::wet, santur_part}},
      {"06b flute cloud", parts::two_flutes},
      {"07 low", parts::warm_sub_bed},
  };
  return list;
}

project::TrackId add_lead(application::ProjectSession& session,
                          const std::vector<std::uint8_t>& midi) {
  const auto track = take(session.instruments().add_native_instrument_track(
      lead_engine, preset_index(synth(lead_engine), lead_preset)));
  application::MidiImportOptions options;
  options.target = application::MidiImportOptions::Target::track;
  options.track = track;
  options.zero_based_bar = lead_in_bars;
  take(session.imports().import_midi_bytes(midi, options));
  return track;
}

} // namespace

void render_set(const fs::path& output, const fs::path& lead_midi) {
  fs::create_directories(output);
  const auto midi = read_file(lead_midi);
  const auto work = output / ".projects";
  // Never clear a directory the tool did not make.
  if (!fs::create_directory(work))
    fail("scratch directory already exists: " + work.string());
  for (const auto& item : auditions()) {
    const std::string label = std::string(item.name) + " - " + std::string(item.pad.preset);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(application::ProjectSession::create(root));
    const auto lead = add_lead(*session, midi);
    parts::add_pad(*session, item.pad, part_bars * beats_per_bar);
    audition::render(*session, root, output / (label + ".wav"), render_rate);
    done(session->tracks().set_track_muted(lead, true));
    audition::render(*session, root, output / (label + " (pad alone).wav"), render_rate);
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::pads
