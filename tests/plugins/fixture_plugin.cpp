#include "pluginterfaces/base/ipluginbase.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

class FixtureFactory final : public Steinberg::IPluginFactory2 {
public:
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::IPluginFactory_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::IPluginFactory2_iid, sizeof(Steinberg::TUID)) == 0) {
      *object = this;
      addRef();
      return Steinberg::kResultOk;
    }
    *object = nullptr;
    return Steinberg::kNoInterface;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }
  Steinberg::tresult PLUGIN_API getFactoryInfo(Steinberg::PFactoryInfo* info) override {
    if (!info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PFactoryInfo("Fixture Vendor", "", "", 0);
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API countClasses() override {
#if defined(FIXTURE_Large)
    return 256;
#else
    return 1;
#endif
  }
  Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index,
                                             Steinberg::PClassInfo* info) override {
    if (index < 0 || index >= countClasses() || !info)
      return Steinberg::kInvalidArgument;
    Steinberg::TUID id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
#if defined(FIXTURE_Large)
    id[0] = static_cast<Steinberg::int8>(index + 1);
#endif
    *info = Steinberg::PClassInfo(id, Steinberg::PClassInfo::kManyInstances, "Audio Module Class",
                                  "Fixture Effect");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index,
                                              Steinberg::PClassInfo2* info) override {
    if (index < 0 || index >= countClasses() || !info)
      return Steinberg::kInvalidArgument;
    Steinberg::TUID id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
#if defined(FIXTURE_Large)
    id[0] = static_cast<Steinberg::int8>(index + 1);
    const std::string long_field(62, 'N');
    const std::string long_subcategories(126, 'S');
    *info = Steinberg::PClassInfo2(id, Steinberg::PClassInfo::kManyInstances, "Audio Module Class",
                                   long_field.c_str(), 0, long_subcategories.c_str(),
                                   long_field.c_str(), long_field.c_str(), "VST 3.8");
#else
    *info = Steinberg::PClassInfo2(id, Steinberg::PClassInfo::kManyInstances, "Audio Module Class",
                                   "Fixture Effect", 0, "Fx", "Fixture Vendor", "1.2.3", "VST 3.8");
#endif
#if defined(FIXTURE_Malformed)
    std::memset(info->name, 'X', sizeof(info->name));
#endif
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString, Steinberg::FIDString,
                                               void** object) override {
    if (object)
      *object = nullptr;
    return Steinberg::kNoInterface;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
};

#if !defined(FIXTURE_NullFactory)
FixtureFactory factory;
#endif
} // namespace

extern "C" __attribute__((visibility("default"))) bool ModuleEntry(void*) {
#if defined(FIXTURE_Crash)
  // The scanner logs what a plugin prints before it fails.
  constexpr std::string_view last_words = "fixture crash: about to fault\n";
  [[maybe_unused]] const auto written =
      ::write(STDERR_FILENO, last_words.data(), last_words.size());
  std::raise(SIGSEGV);
#elif defined(FIXTURE_Hang)
  while (true)
    pause();
#endif
  return true;
}
#if !defined(FIXTURE_NoExports)
extern "C" __attribute__((visibility("default"))) bool ModuleExit() { return true; }
#endif
extern "C" __attribute__((visibility("default"))) Steinberg::IPluginFactory* GetPluginFactory() {
#if defined(FIXTURE_NullFactory)
  return nullptr;
#else
  factory.addRef();
  return &factory;
#endif
}
