#pragma once

#include <cstddef>
#include <cstdint>

namespace oscillate::fuzz {

void manifest(const std::uint8_t* bytes, std::size_t size);
void wav(const std::uint8_t* bytes, std::size_t size);
void midi(const std::uint8_t* bytes, std::size_t size);

} // namespace oscillate::fuzz
