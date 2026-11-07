#include "parser_exercise.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* bytes, std::size_t size) {
  oscillate::fuzz::manifest(bytes, size);
  return 0;
}
