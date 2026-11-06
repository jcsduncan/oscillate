#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
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
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool) override {
    return Steinberg::kResultOk;
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
    return static_cast<Steinberg::uint32>(latency());
  }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    return setup.symbolicSampleSize == Steinberg::Vst::kSample32 && setup.sampleRate == 48000.0 &&
                   (setup.maxSamplesPerBlock == 64 || setup.maxSamplesPerBlock == 128)
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (data.numInputs != 1 || data.numOutputs != 1 || data.numSamples > 128)
      return Steinberg::kResultFalse;
    constexpr auto timing_flags = Steinberg::Vst::ProcessContext::kTempoValid |
                                  Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
    if (data.processContext == nullptr ||
        (data.processContext->state & timing_flags) != timing_flags ||
        !std::isfinite(data.processContext->projectTimeMusic) ||
        !std::isfinite(data.processContext->tempo) || data.processContext->tempo <= 0.0 ||
        data.processContext->projectTimeSamples < 0)
      return Steinberg::kResultFalse;
    if (gain_ == 999.0F)
      return Steinberg::kResultFalse;
    if (gain_ == -98.0F && data.processMode != Steinberg::Vst::kOffline)
      return Steinberg::kResultFalse;
    // Reserved fixture state: turn the supplied block timing into an audible
    // value so the graph test can verify the project's tempo reaches the host.
    float gain = std::abs(gain_);
    if (gain_ == -98.0F || gain_ == -96.0F || gain_ == -95.0F || gain_ == -93.0F)
      gain = 0.5F;
    else if (gain_ == -97.0F)
      gain = static_cast<float>(data.processContext->tempo / 100.0 +
                                data.processContext->projectTimeMusic / 10.0);
    for (int frame = 0; frame < data.numSamples; ++frame) {
      for (int channel = 0; channel < 2; ++channel) {
        const float input = data.inputs[0].channelBuffers32[channel][frame];
        const float delayed = history_[channel][cursor_];
        // Deliberately dishonest synthetic tail for qualification rejection:
        // one additional seven-frame echo despite getTailSamples() == 0.
        data.outputs[0].channelBuffers32[channel][frame] =
            delayed + (gain_ == -93.0F ? late_echo_[channel][cursor_] : 0.0F);
        late_echo_[channel][cursor_] = gain_ == -93.0F ? delayed * 0.25F : 0.0F;
        // Reserved order-sensitive fixture mode for native/VST3 chains.
        history_[channel][cursor_] = gain_ == -94.0F ? input * std::abs(input) : input * gain;
      }
      cursor_ = (cursor_ + 1) % latency();
    }
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
  [[nodiscard]] int latency() const noexcept {
    if (gain_ == -95.0F)
      return 8'193; // Above the experimental export limit.
    if (gain_ == -96.0F)
      return 96; // Crosses both export block sizes and the former 64-frame cap.
    return 7;
  }
  std::atomic<Steinberg::uint32> references_{1};
  float gain_{2.0F};
  float history_[2][8'193]{};
  float late_echo_[2][8'193]{};
  int cursor_ = 0;
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
