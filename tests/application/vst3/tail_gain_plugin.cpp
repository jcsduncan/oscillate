#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

void record(const char* event) {
  const auto* path = std::getenv("OSCILLATE_VST3_LOADER_TEST_EVENTS");
  if (path && path[0] != 0)
    std::ofstream(path, std::ios::app) << event << '\n';
}

bool fail_mode(const char* mode) {
  const auto* selected = std::getenv("OSCILLATE_VST3_LOADER_TEST_FAIL");
  return selected && std::strcmp(selected, mode) == 0;
}

class GainComponent final : public Steinberg::Vst::IComponent,
                            public Steinberg::Vst::IAudioProcessor {
public:
  GainComponent() { record("component-create"); }
  ~GainComponent() { record("component-destroy"); }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IComponent*>(this);
    else if (std::memcmp(iid, Steinberg::Vst::IAudioProcessor_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IAudioProcessor*>(this);
    else {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override {
    const auto remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown*) override {
    record("component-initialize");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override {
    record("component-terminate");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getControllerClassId(Steinberg::TUID) override {
    return Steinberg::kNoInterface;
  }
  Steinberg::tresult PLUGIN_API setIoMode(Steinberg::Vst::IoMode) override {
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API getBusCount(Steinberg::Vst::MediaType type,
                                          Steinberg::Vst::BusDirection) override {
    return type == Steinberg::Vst::kAudio ? 1 : 0;
  }
  Steinberg::tresult PLUGIN_API getBusInfo(Steinberg::Vst::MediaType,
                                           Steinberg::Vst::BusDirection direction, Steinberg::int32,
                                           Steinberg::Vst::BusInfo& info) override {
    info = {};
    info.mediaType = Steinberg::Vst::kAudio;
    info.direction = direction;
    info.channelCount = 2;
    info.busType = Steinberg::Vst::kMain;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getRoutingInfo(Steinberg::Vst::RoutingInfo&,
                                               Steinberg::Vst::RoutingInfo&) override {
    return Steinberg::kNoInterface;
  }
  Steinberg::tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType, Steinberg::Vst::BusDirection,
                                            Steinberg::int32, Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool active) override {
    const auto* mode = std::getenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE");
    return active && mode && std::strcmp(mode, "activate-fail") == 0 ? Steinberg::kResultFalse
                                                                     : Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    float gain = 0.0F;
    Steinberg::int32 read = 0;
    if (stream->read(&gain, sizeof(gain), &read) != Steinberg::kResultOk || read != sizeof(gain) ||
        !std::isfinite(gain))
      return Steinberg::kResultFalse;
    gain_ = gain;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    return stream->write(&gain_, sizeof(gain_)) == Steinberg::kResultOk ? Steinberg::kResultOk
                                                                        : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                   Steinberg::int32 input_count,
                                                   Steinberg::Vst::SpeakerArrangement* outputs,
                                                   Steinberg::int32 output_count) override {
    return input_count == 1 && output_count == 1 &&
                   *inputs == Steinberg::Vst::SpeakerArr::kStereo &&
                   *outputs == Steinberg::Vst::SpeakerArr::kStereo
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API
  getBusArrangement(Steinberg::Vst::BusDirection, Steinberg::int32,
                    Steinberg::Vst::SpeakerArrangement& arrangement) override {
    arrangement = Steinberg::Vst::SpeakerArr::kStereo;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
    return size == Steinberg::Vst::kSample32 ? Steinberg::kResultOk : Steinberg::kResultFalse;
  }
  Steinberg::uint32 PLUGIN_API getLatencySamples() override {
    const auto* mode = std::getenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE");
    return mode && std::strcmp(mode, "latency-over-budget") == 0 ? 8'193 : 0;
  }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    return setup.symbolicSampleSize == Steinberg::Vst::kSample32 && setup.sampleRate == 48000.0 &&
                   setup.maxSamplesPerBlock == 128
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (data.numInputs != 1 || data.numOutputs != 1 || data.numSamples > 128)
      return Steinberg::kResultFalse;
    if (gain_ == 999.0F)
      return Steinberg::kResultFalse;
    for (int frame = 0; frame < data.numSamples; ++frame) {
      for (int channel = 0; channel < 2; ++channel) {
        const float input = data.inputs[0].channelBuffers32[channel][frame];
        data.outputs[0].channelBuffers32[channel][frame] = history_[channel][cursor_];
        history_[channel][cursor_] = input * std::abs(gain_);
      }
      cursor_ = (cursor_ + 1U) % 8U;
    }
    return Steinberg::kResultOk;
  }
  // The eight-frame history can emit its final sample at the last tail frame.
  Steinberg::uint32 PLUGIN_API getTailSamples() override {
    const auto* mode = std::getenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE");
    if (mode && std::strcmp(mode, "infinite-tail") == 0)
      return 0xffffffffU;
    if (mode && std::strcmp(mode, "overlong-tail") == 0)
      return 48'000U * 11U;
    return 8;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
  float gain_{2.0F};
  float history_[2][8]{};
  unsigned cursor_{0};
};

class GainFactory final : public Steinberg::IPluginFactory2 {
public:
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (fail_mode("factory-v1") &&
        std::memcmp(iid, Steinberg::IPluginFactory2_iid, sizeof(Steinberg::TUID)) == 0)
      return Steinberg::kNoInterface;
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
    if (fail_mode("factory-info"))
      return Steinberg::kResultFalse;
    if (!info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PFactoryInfo("Fixture Vendor", "", "", 0);
    if (fail_mode("factory-vendor-empty"))
      info->vendor[0] = 0;
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API countClasses() override { return fail_mode("class-count") ? 257 : 1; }
  Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index,
                                             Steinberg::PClassInfo* info) override {
    if (fail_mode("basic-fail"))
      return Steinberg::kResultFalse;
    if (index != 0 || !info)
      return Steinberg::kInvalidArgument;
    static const Steinberg::TUID id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    *info = Steinberg::PClassInfo(id, Steinberg::PClassInfo::kManyInstances, "Audio Module Class",
                                  "Loaded Gain");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index,
                                              Steinberg::PClassInfo2* info) override {
    if (fail_mode("class-info") || fail_mode("basic-fail"))
      return Steinberg::kResultFalse;
    if (index != 0 || !info)
      return Steinberg::kInvalidArgument;
    static const Steinberg::TUID id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    *info = Steinberg::PClassInfo2(id, Steinberg::PClassInfo::kManyInstances, "Audio Module Class",
                                   "Loaded Gain", 0, "Fx", "Fixture Vendor", "1.0.0", "VST 3.8");
    if (fail_mode("class-name-empty"))
      info->name[0] = 0;
    if (fail_mode("class-version-control"))
      info->version[0] = '\n';
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid,
                                               void** object) override {
    if (fail_mode("no-instance"))
      return Steinberg::kNoInterface;
    static const Steinberg::TUID id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    if (!object || std::memcmp(cid, id, sizeof(id)) != 0 ||
        std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(id)) != 0)
      return Steinberg::kNoInterface;
    *object = static_cast<Steinberg::Vst::IComponent*>(new GainComponent());
    return Steinberg::kResultOk;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
};

GainFactory factory;
} // namespace

extern "C" __attribute__((visibility("default"))) bool ModuleEntry(void*) {
  record("module-entry");
  return !fail_mode("entry");
}
extern "C" __attribute__((visibility("default"))) bool ModuleExit() {
  record("module-exit");
  return true;
}
extern "C" __attribute__((visibility("default"))) Steinberg::IPluginFactory* GetPluginFactory() {
  if (fail_mode("factory"))
    return nullptr;
  factory.addRef();
  return &factory;
}
