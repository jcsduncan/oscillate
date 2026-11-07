#include "parser_exercise.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::string_view current_manifest_seed = "current.json";
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || (std::string(argv[1]) != "manifest" && std::string(argv[1]) != "wav" &&
                    std::string(argv[1]) != "midi")) {
    std::cerr << "usage: oscillate_fuzz_corpus_replay manifest|wav|midi CORPUS_DIRECTORY\n";
    return 2;
  }
  const std::string parser = argv[1];
  std::size_t count = 0;
  try {
    for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
      if (!entry.is_regular_file())
        continue;
      std::ifstream input(entry.path(), std::ios::binary);
      if (!input)
        throw std::runtime_error("cannot open corpus file");
      const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                            std::istreambuf_iterator<char>()};
      // The current-schema seed must stay valid, or the corpus stops
      // reaching the decoder's accepting paths; malformed seeds may fail.
      if (parser == "manifest" && entry.path().filename() == current_manifest_seed &&
          !oscillate::fuzz::manifest_decodes(bytes.data(), bytes.size()))
        throw std::runtime_error("the current manifest seed no longer decodes; regenerate it "
                                 "with oscillate_fuzz_manifest_seed");
      if (parser == "manifest")
        oscillate::fuzz::manifest(bytes.data(), bytes.size());
      else if (parser == "wav")
        oscillate::fuzz::wav(bytes.data(), bytes.size());
      else
        oscillate::fuzz::midi(bytes.data(), bytes.size());
      ++count;
    }
    if (count == 0)
      throw std::runtime_error("empty corpus");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "replayed " << count << ' ' << argv[1] << " seeds\n";
  return 0;
}
