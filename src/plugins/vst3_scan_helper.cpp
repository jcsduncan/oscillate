#include "scan_protocol.hpp"

#include "oscillate/foundation/hex.hpp"

#include "pluginterfaces/base/ipluginbase.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using oscillate::plugins::ScanResult;
using oscillate::plugins::ScanStatus;

template <std::size_t Size>
std::optional<std::string> checked_text(const char (&value)[Size], bool allow_empty = false) {
  const auto length = strnlen(value, Size);
  if (length == Size || (!allow_empty && length == 0))
    return std::nullopt;
  for (std::size_t i = 0; i < length; ++i) {
    if (static_cast<unsigned char>(value[i]) < 0x20)
      return std::nullopt;
  }
  return std::string(value, length);
}

std::string hex_id(const Steinberg::TUID& id) {
  return oscillate::foundation::to_hex(std::as_bytes(std::span(id)));
}

bool all_zero(const Steinberg::TUID& id) {
  for (auto byte : id) {
    if (byte != 0)
      return false;
  }
  return true;
}

ScanResult fail(ScanStatus status, std::string reason) {
  return {.status = status, .metadata = {}, .diagnostic = std::move(reason), .binary = {}};
}

ScanResult inspect(const std::filesystem::path& bundle) {
  std::error_code error;
  if (bundle.extension() != ".vst3" || !std::filesystem::is_directory(bundle, error) || error)
    return fail(ScanStatus::invalid_bundle, "Expected a VST3 bundle directory");
  const auto so = bundle / "Contents" / "x86_64-linux" / (bundle.stem().string() + ".so");
  if (!std::filesystem::is_regular_file(so, error) || error)
    return fail(ScanStatus::invalid_bundle, "VST3 x86_64 Linux binary is missing");

  void* library = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library)
    return fail(ScanStatus::invalid_bundle, "Cannot load VST3 binary");
  using Entry = bool (*)(void*);
  using Exit = bool (*)();
  using GetFactory = Steinberg::IPluginFactory* (*)();
  const auto entry = reinterpret_cast<Entry>(dlsym(library, "ModuleEntry"));
  const auto exit = reinterpret_cast<Exit>(dlsym(library, "ModuleExit"));
  const auto get_factory = reinterpret_cast<GetFactory>(dlsym(library, "GetPluginFactory"));
  if (!entry || !exit || !get_factory) {
    dlclose(library);
    return fail(ScanStatus::invalid_bundle, "Required VST3 exports are missing");
  }
  if (!entry(library)) {
    dlclose(library);
    return fail(ScanStatus::invalid_bundle, "VST3 ModuleEntry failed");
  }
  auto* factory = get_factory();
  if (!factory) {
    exit();
    dlclose(library);
    return fail(ScanStatus::invalid_metadata, "VST3 factory is missing");
  }
  ScanResult result{.status = ScanStatus::ok, .metadata = {}, .diagnostic = {}, .binary = {}};
  Steinberg::PFactoryInfo factory_info{};
  if (factory->getFactoryInfo(&factory_info) != Steinberg::kResultOk) {
    result = fail(ScanStatus::invalid_metadata, "VST3 factory metadata failed");
  } else if (auto vendor = checked_text(factory_info.vendor)) {
    result.metadata.vendor = *vendor;
  } else {
    result = fail(ScanStatus::invalid_metadata, "Invalid VST3 factory vendor");
  }
  if (result.ok()) {
    const auto count = factory->countClasses();
    if (count <= 0 ||
        count > static_cast<Steinberg::int32>(oscillate::plugins::detail::max_classes)) {
      result = fail(ScanStatus::invalid_metadata, "Invalid VST3 class count");
    } else {
      Steinberg::IPluginFactory2* factory2 = nullptr;
      if (factory->queryInterface(Steinberg::IPluginFactory2_iid,
                                  reinterpret_cast<void**>(&factory2)) != Steinberg::kResultOk) {
        factory2 = nullptr;
      }
      for (Steinberg::int32 i = 0; i < count; ++i) {
        Steinberg::PClassInfo basic{};
        Steinberg::PClassInfo2 extended{};
        const bool has_extended =
            factory2 && factory2->getClassInfo2(i, &extended) == Steinberg::kResultOk;
        if (!has_extended && factory->getClassInfo(i, &basic) != Steinberg::kResultOk) {
          result = fail(ScanStatus::invalid_metadata, "VST3 class metadata failed");
          break;
        }
        auto id = has_extended ? hex_id(extended.cid) : hex_id(basic.cid);
        const auto category =
            has_extended ? checked_text(extended.category) : checked_text(basic.category);
        const auto name = has_extended ? checked_text(extended.name) : checked_text(basic.name);
        const auto vendor =
            has_extended ? checked_text(extended.vendor, true) : std::optional<std::string>{};
        const auto version =
            has_extended ? checked_text(extended.version, true) : std::optional<std::string>{};
        const auto subcategories = has_extended ? checked_text(extended.subCategories, true)
                                                : std::optional<std::string>{};
        if ((has_extended && all_zero(extended.cid)) || (!has_extended && all_zero(basic.cid)) ||
            !category || !name || (has_extended && (!vendor || !version || !subcategories))) {
          result = fail(ScanStatus::invalid_metadata, "Invalid VST3 class fields");
          break;
        }
        for (const auto& existing : result.metadata.classes) {
          if (existing.id == id) {
            result = fail(ScanStatus::invalid_metadata, "Duplicate VST3 class identifier");
            break;
          }
        }
        if (!result.ok())
          break;
        result.metadata.classes.push_back(
            {std::move(id), *category, *name,
             has_extended && !vendor->empty() ? *vendor : result.metadata.vendor,
             has_extended ? *version : std::string{},
             has_extended ? *subcategories : std::string{}});
      }
      if (factory2)
        factory2->release();
    }
  }
  factory->release();
  exit();
  dlclose(library);
  return result;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  const auto result = inspect(argv[1]);
  auto encoded = oscillate::plugins::detail::encode_scan_result(result);
  if (!encoded)
    encoded = oscillate::plugins::detail::encode_scan_result(
        fail(ScanStatus::invalid_metadata, "VST3 metadata exceeds scanner protocol limits"));
  if (!encoded)
    return 3;
  const auto& bytes = *encoded;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0)
      return 3;
    offset += static_cast<std::size_t>(count);
  }
  return 0;
}
