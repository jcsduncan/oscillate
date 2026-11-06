#include "oscillate/plugins/vst3_bundle_loader.hpp"

#include "oscillate/foundation/hex.hpp"
#include "vst3_binary_identity.hpp"
#include "vst3_effect_host_internal.hpp"

#include "pluginterfaces/base/ipluginbase.h"

#include <cstring>
#include <dlfcn.h>
#include <optional>
#include <string_view>
#include <utility>

namespace oscillate::plugins {
namespace {

using Entry = bool (*)(void*);
using Exit = bool (*)();
using FactoryGetter = Steinberg::IPluginFactory* (*)();

struct ModuleGuard {
  void* module{nullptr};
  Exit exit{nullptr};
  bool entered{false};
  ~ModuleGuard() {
    if (entered && exit) {
      try {
        exit();
        // A broken plugin must not throw through module cleanup.
        // NOLINTNEXTLINE(bugprone-empty-catch)
      } catch (...) {
      }
    }
    if (module)
      dlclose(module);
  }
};

struct FactoryGuard {
  Steinberg::IPluginFactory* factory{nullptr};
  ~FactoryGuard() {
    if (factory) {
      try {
        factory->release();
        // Module teardown must continue if a plugin release throws.
        // NOLINTNEXTLINE(bugprone-empty-catch)
      } catch (...) {
      }
    }
  }
};

template <std::size_t Size>
std::optional<std::string> checked_text(const char (&field)[Size], bool allow_empty = false) {
  const auto length = strnlen(field, Size);
  if (length == Size || (!allow_empty && length == 0))
    return std::nullopt;
  for (std::size_t index = 0; index < length; ++index) {
    if (static_cast<unsigned char>(field[index]) < 0x20)
      return std::nullopt;
  }
  return std::string(field, length);
}

std::string hex_id(const Steinberg::TUID& id) {
  return foundation::to_hex(std::as_bytes(std::span(id)));
}

bool factory_has_class(Steinberg::IPluginFactory& factory, const Vst3Class& expected) {
  Steinberg::PFactoryInfo info{};
  if (factory.getFactoryInfo(&info) != Steinberg::kResultOk)
    return false;
  const auto factory_vendor = checked_text(info.vendor);
  if (!factory_vendor)
    return false;
  const auto count = factory.countClasses();
  if (count <= 0 || count > 256)
    return false;
  Steinberg::IPluginFactory2* factory2 = nullptr;
  if (factory.queryInterface(Steinberg::IPluginFactory2_iid, reinterpret_cast<void**>(&factory2)) !=
      Steinberg::kResultOk)
    factory2 = nullptr;
  bool matched = false;
  for (Steinberg::int32 index = 0; index < count; ++index) {
    Steinberg::PClassInfo basic{};
    Steinberg::PClassInfo2 extended{};
    const bool has_extended =
        factory2 && factory2->getClassInfo2(index, &extended) == Steinberg::kResultOk;
    if (!has_extended && factory.getClassInfo(index, &basic) != Steinberg::kResultOk)
      break;
    const auto id = has_extended ? hex_id(extended.cid) : hex_id(basic.cid);
    if (id != expected.id)
      continue;
    const auto category =
        has_extended ? checked_text(extended.category) : checked_text(basic.category);
    const auto name = has_extended ? checked_text(extended.name) : checked_text(basic.name);
    const auto vendor =
        has_extended ? checked_text(extended.vendor, true) : std::optional<std::string>{};
    const auto version =
        has_extended ? checked_text(extended.version, true) : std::optional<std::string>{};
    const auto subcategories =
        has_extended ? checked_text(extended.subCategories, true) : std::optional<std::string>{};
    if (!category || !name || (has_extended && (!vendor || !version || !subcategories)))
      break;
    const auto class_vendor = has_extended && !vendor->empty() ? *vendor : *factory_vendor;
    matched = *category == expected.category && *name == expected.name &&
              class_vendor == expected.vendor &&
              (has_extended ? *version : std::string{}) == expected.version &&
              (has_extended ? *subcategories : std::string{}) == expected.subcategories;
    break;
  }
  if (factory2)
    factory2->release();
  return matched;
}

} // namespace

LoadedVst3Effect::LoadedVst3Effect(void* module, bool (*module_exit)(),
                                   std::unique_ptr<Vst3EffectHost> host)
    : module_(module), module_exit_(module_exit), host_(std::move(host)) {}

LoadedVst3Effect::~LoadedVst3Effect() {
  host_.reset();
  if (module_exit_) {
    try {
      module_exit_();
      // Destruction must remain nonthrowing even for a broken plugin.
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
  }
  if (module_)
    dlclose(module_);
}

Vst3EffectHost& LoadedVst3Effect::host() noexcept { return *host_; }
const Vst3EffectHost& LoadedVst3Effect::host() const noexcept { return *host_; }

namespace {
std::unique_ptr<LoadedVst3Effect>
load_verified(const VerifiedVst3Effect& verified, double sample_rate,
              std::uint32_t max_block_frames, Vst3HostKind kind, std::string& error,
              Vst3ProcessingMode mode, std::uint32_t max_parameter_events, bool routed_sidechain) {
  error.clear();
  const auto now = detail::fingerprint_vst3_binary(verified.binary.canonical_bundle);
  if (!now || now->canonical_bundle != verified.binary.canonical_bundle ||
      now->canonical_binary != verified.binary.canonical_binary ||
      now->byte_size != verified.binary.byte_size || now->sha256 != verified.binary.sha256) {
    error = "VST3 bundle binary is missing or changed since scan";
    return nullptr;
  }
  ModuleGuard module;
  module.module = dlopen(now->canonical_binary.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module.module) {
    error = "Verified VST3 bundle could not be loaded";
    return nullptr;
  }
  const auto loaded_file = detail::fingerprint_vst3_binary(verified.binary.canonical_bundle);
  if (!loaded_file || loaded_file->canonical_binary != now->canonical_binary ||
      loaded_file->byte_size != now->byte_size || loaded_file->sha256 != now->sha256) {
    error = "VST3 bundle binary changed during load";
    return nullptr;
  }
  const auto entry = reinterpret_cast<Entry>(dlsym(module.module, "ModuleEntry"));
  module.exit = reinterpret_cast<Exit>(dlsym(module.module, "ModuleExit"));
  const auto get_factory =
      reinterpret_cast<FactoryGetter>(dlsym(module.module, "GetPluginFactory"));
  if (!entry || !module.exit || !get_factory) {
    error = "Verified VST3 bundle is missing required exports";
    return nullptr;
  }
  try {
    if (!entry(module.module)) {
      error = "Verified VST3 ModuleEntry failed";
      return nullptr;
    }
    module.entered = true;
    FactoryGuard factory{get_factory()};
    if (!factory.factory) {
      error = "Verified VST3 factory is missing";
      return nullptr;
    }
    if (!factory_has_class(*factory.factory, verified.identity)) {
      error = "VST3 class identity changed since scan";
      return nullptr;
    }
    Vst3EffectConfig config{.identity = verified.identity,
                            .sample_rate = sample_rate,
                            .max_block_frames = max_block_frames,
                            .max_parameter_events = max_parameter_events,
                            .kind = kind,
                            .processing_mode = mode,
                            .routed_sidechain = routed_sidechain};
    auto host = detail::create_effect_host(*factory.factory, std::move(config), error);
    if (!host)
      return nullptr;
    auto result = std::make_unique<LoadedVst3Effect>(module.module, module.exit, std::move(host));
    module.module = nullptr;
    module.entered = false;
    module.exit = nullptr;
    return result;
  } catch (...) {
    error = "Verified VST3 module threw during load";
    return nullptr;
  }
}
} // namespace

std::unique_ptr<LoadedVst3Effect>
load_verified_vst3_effect(const VerifiedVst3Effect& verified, double sample_rate,
                          std::uint32_t max_block_frames, std::string& error,
                          Vst3ProcessingMode mode, std::uint32_t max_parameter_events,
                          bool routed_sidechain) {
  return load_verified(verified, sample_rate, max_block_frames, Vst3HostKind::effect, error, mode,
                       max_parameter_events, routed_sidechain);
}

std::unique_ptr<LoadedVst3Effect>
load_verified_vst3_instrument(const VerifiedVst3Effect& verified, double sample_rate,
                              std::uint32_t max_block_frames, std::string& error,
                              Vst3ProcessingMode mode, std::uint32_t max_parameter_events) {
  return load_verified(verified, sample_rate, max_block_frames, Vst3HostKind::instrument, error,
                       mode, max_parameter_events, false);
}

} // namespace oscillate::plugins
