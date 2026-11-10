// Tonal targets (ADR-0120): measures references' long-term spectra with the
// band meter Lens runs, and builds targets from them.
//
//   oscillate_tonal_targets measure FILE.f32...
//       each file's normalised third-octave curve, one line per file
//   oscillate_tonal_targets build --id ID --name NAME --out TARGET.json FILE.f32...
//       a target from the files: per band the median and the middle half
//   oscillate_tonal_targets factory --out tonal_targets_factory.cpp TARGET.json...
//       the factory targets' source, in the order given; each target's id is
//       its file's name without .json
//
// Inputs are raw 48 kHz stereo float32 (ffmpeg -f f32le -ac 2 -ar 48000);
// build_targets.py decodes a folder of references and runs these.
#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace tonal = oscillate::native_dsp::tonal;
using oscillate::engine::StereoFrame;

constexpr double rate = 48'000.0;
constexpr std::size_t chunk_frames = 65'536;
constexpr int written_decimals = 2;
constexpr int usage_status = 2;

[[nodiscard]] std::string number(double value) {
  std::array<char, 32> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), value,
                                     std::chars_format::fixed, written_decimals);
  return {text.data(), written.ptr};
}

// A file's normalised curve, or false when it cannot be read or is silent.
[[nodiscard]] bool measure(const std::string& path, tonal::Curve& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "cannot read " << path << '\n';
    return false;
  }
  tonal::BandMeter meter(rate);
  std::vector<StereoFrame> chunk(chunk_frames);
  while (in) {
    in.read(reinterpret_cast<char*>(chunk.data()),
            static_cast<std::streamsize>(chunk.size() * sizeof(StereoFrame)));
    const auto frames = static_cast<std::size_t>(in.gcount()) / sizeof(StereoFrame);
    for (std::size_t i = 0; i < frames; ++i) {
      const double mid =
          0.5 * (static_cast<double>(chunk[i].left) + static_cast<double>(chunk[i].right));
      meter.push(mid, 0.0);
    }
  }
  if (meter.windows() == 0) {
    std::cerr << path << " is shorter than one analysis window\n";
    return false;
  }
  const auto levels = tonal::grid_levels(meter.mean_a());
  for (const double level : levels) {
    if (level <= oscillate::native_dsp::dsp::silent_db) {
      std::cerr << path << " has a silent band\n";
      return false;
    }
  }
  out = tonal::normalised(levels);
  return true;
}

int run_measure(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    tonal::Curve curve{};
    if (!measure(file, curve)) {
      return 1;
    }
    std::cout << file;
    for (const double level : curve) {
      std::cout << ' ' << number(level);
    }
    std::cout << '\n';
  }
  return 0;
}

// What build writes: the target's id and name, and where.
struct Build {
  std::string id;
  std::string name;
  std::string out;
};

int run_build(const Build& build, const std::vector<std::string>& files) {
  std::vector<tonal::Curve> curves;
  for (const auto& file : files) {
    tonal::Curve curve{};
    if (!measure(file, curve)) {
      return 1;
    }
    curves.push_back(curve);
  }
  auto target = tonal::build_target(build.id, build.name, curves);
  if (!target) {
    std::cerr << target.error().message << '\n';
    return 1;
  }
  // What Lens and factory will read: refuse a target they would refuse.
  const auto text = tonal::serialise_target(target.value());
  if (auto checked = tonal::parse_target(text, build.id); !checked) {
    std::cerr << checked.error().message << '\n';
    return 1;
  }
  std::ofstream file(build.out, std::ios::binary | std::ios::trunc);
  file << text;
  if (!file) {
    std::cerr << "cannot write " << build.out << '\n';
    return 1;
  }
  std::cout << "wrote " << build.out << " from " << curves.size() << " references\n";
  return 0;
}

void write_curve(std::ostream& out, std::string_view field, const tonal::Curve& curve) {
  out << "         ." << field << " = {";
  for (std::size_t b = 0; b < curve.size(); ++b) {
    out << (b == 0 ? "" : ", ") << number(curve[b]);
  }
  out << "},\n";
}

// `text` as a C++ string literal; false when it holds a control character.
[[nodiscard]] bool quoted(std::string_view text, std::string& out) {
  constexpr unsigned char first_printable = 0x20;
  out = "\"";
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < first_printable) {
      return false;
    }
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return true;
}

int run_factory(const std::string& out, const std::vector<std::string>& files) {
  std::vector<tonal::Target> targets;
  for (const auto& path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    auto target = tonal::parse_target(text.str(), std::filesystem::path(path).stem().string());
    if (!target) {
      std::cerr << path << ": " << target.error().message << '\n';
      return 1;
    }
    targets.push_back(std::move(target).value());
  }
  std::ofstream file(out, std::ios::binary | std::ios::trunc);
  file << "// Generated by tools/tonal_targets (ADR-0120); do not edit. Each target's\n"
          "// levels are statistics of its references' loudness-normalised long-term\n"
          "// third-octave spectra; no audio is stored.\n"
          "#include \"oscillate/native_dsp/tonal_targets.hpp\"\n\n"
          "#include <array>\n#include <span>\n\n"
          "namespace oscillate::native_dsp::tonal {\n\n"
          "std::span<const Target> factory_targets() {\n"
          "  static const std::array<Target, "
       << targets.size() << "> targets{{\n";
  for (const auto& target : targets) {
    std::string id;
    std::string name;
    if (!quoted(target.id, id) || !quoted(target.name, name)) {
      std::cerr << target.id << ": a control character in the id or name\n";
      return 1;
    }
    file << "    Target{.id = " << id << ",\n"
         << "         .name = " << name << ",\n"
         << "         .references = " << target.references << ",\n";
    write_curve(file, "median", target.median);
    write_curve(file, "low", target.low);
    write_curve(file, "high", target.high);
    file << "    },\n";
  }
  file << "  }};\n  return targets;\n}\n\n"
          "} // namespace oscillate::native_dsp::tonal\n";
  if (!file) {
    std::cerr << "cannot write " << out << '\n';
    return 1;
  }
  std::cout << "wrote " << out << " with " << targets.size() << " targets\n";
  return 0;
}

int usage() {
  std::cerr << "usage: oscillate_tonal_targets measure FILE.f32...\n"
               "       oscillate_tonal_targets build --id ID --name NAME --out TARGET.json "
               "FILE.f32...\n"
               "       oscillate_tonal_targets factory --out FILE.cpp TARGET.json...\n";
  return usage_status;
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    return usage();
  }
  Build build;
  std::vector<std::string> files;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if (args[i] == "--id" && has_value) {
      build.id = args[++i];
    } else if (args[i] == "--name" && has_value) {
      build.name = args[++i];
    } else if (args[i] == "--out" && has_value) {
      build.out = args[++i];
    } else {
      files.push_back(args[i]);
    }
  }
  if (files.empty()) {
    return usage();
  }
  if (args[0] == "measure") {
    return run_measure(files);
  }
  if (args[0] == "build" && !build.name.empty() && !build.out.empty()) {
    return run_build(build, files);
  }
  if (args[0] == "factory" && !build.out.empty()) {
    return run_factory(build.out, files);
  }
  return usage();
}
