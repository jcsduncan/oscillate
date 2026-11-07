// Runs Mix Check (ADR-0074) on a project from the command line and prints
// its meters, loudness and findings; nothing is applied or saved.
//   oscillate_mix_check <project> [club|stream|dj]
#include "oscillate/application/mix_check.hpp"
#include "oscillate/application/project_session.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  using namespace oscillate::application;
  if (argc < 2) {
    std::cerr << "usage: oscillate_mix_check <project> [club|stream|dj]\n";
    return 2;
  }
  const std::filesystem::path root = argv[1];
  const std::string target = argc > 2 ? argv[2] : "club";
  auto session = ProjectSession::open(root);
  if (!session) {
    std::cerr << "open: " << session.error().message << '\n';
    return 1;
  }
  auto pinned = session.value()->exports().pin_snapshot();
  if (!pinned) {
    std::cerr << "pin: " << pinned.error().message << '\n';
    return 1;
  }
  // Each stage once, with the seconds since the start; with OSCILLATE_MIX_CHECK_TRACE
  // set, every progress report too (to see render against analysis time).
  const auto start = std::chrono::steady_clock::now();
  const bool trace = std::getenv("OSCILLATE_MIX_CHECK_TRACE") != nullptr;
  std::string last;
  auto measured =
      mix_check::measure(pinned.value(), [&](double fraction, const std::string& stage) {
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (trace || stage != last)
          std::cerr << "  " << std::fixed << std::setprecision(1) << seconds << " s  " << stage
                    << (trace ? "  " + std::to_string(fraction) : "") << '\n';
        last = stage;
        return true;
      });
  if (!measured) {
    std::cerr << "measure: " << measured.error().message << '\n';
    return 1;
  }
  const auto report = mix_check::report(measured.value(), target);
  std::cout << std::fixed << std::setprecision(1);
  std::cout << report.project_name << " · " << report.track_count << " tracks · "
            << report.duration_seconds << " s\n\nMeters\n";
  for (const auto& meter : report.meters)
    std::cout << "  [" << meter.state << "] " << meter.label << ": " << meter.value << " ("
              << meter.note << ")\n";
  std::cout << "\nLoudness: " << report.loudness.integrated << " LUFS, true peak "
            << report.loudness.true_peak << " dBTP, range " << report.loudness.range
            << " LU, drum crest " << report.loudness.crest << " dB\n";
  if (!report.energy_note.empty())
    std::cout << "Energy: " << report.energy_note << '\n';
  std::cout << "\nFindings (" << report.findings.size() << ")\n";
  for (const auto& finding : report.findings) {
    std::cout << "\n  " << (finding.bad ? "POKES OUT" : "WORTH A LOOK") << " · " << finding.title
              << '\n';
    std::cout << "    " << finding.text << '\n';
    std::cout << "    Tracks:";
    for (const auto& track : finding.tracks)
      std::cout << ' ' << track.name;
    if (finding.band)
      std::cout << " · band " << finding.band->label;
    std::cout << '\n';
    if (finding.fix) {
      std::cout << "    Fix: " << finding.fix->device << " (" << finding.fix->where << ")";
      for (const auto& [label, value] : finding.fix->params)
        std::cout << " · " << label << " " << value;
      std::cout << "\n    Expected: " << finding.expected << '\n';
    }
  }
  std::cout << "\nChecked and fine\n";
  for (const auto& line : report.fine)
    std::cout << "  ✓ " << line << '\n';
  return 0;
}
