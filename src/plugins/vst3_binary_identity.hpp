#pragma once

#include "oscillate/plugins/vst3_scan.hpp"

#include <filesystem>
#include <optional>

namespace oscillate::plugins::detail {

[[nodiscard]] std::optional<Vst3BinaryIdentity>
fingerprint_vst3_binary(const std::filesystem::path& bundle);

} // namespace oscillate::plugins::detail
