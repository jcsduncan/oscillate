#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

const Steinberg::TUID component_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const Steinberg::TUID controller_id = {33, 34, 35, 36, 37, 38, 39, 40,
                                       41, 42, 43, 44, 45, 46, 47, 48};
#ifdef OSCILLATE_FIXTURE_SIDECHAIN
constexpr bool fixture_sidechain = true;
#else
constexpr bool fixture_sidechain = false;
#endif

void record(const char* event) {
  const auto* path = std::getenv("OSCILLATE_VST3_LOADER_TEST_EVENTS");
  if (path && path[0] != 0)
    std::ofstream(path, std::ios::app) << event << '\n';
}

bool fail_mode(const char* mode) {
  const auto* selected = std::getenv("OSCILLATE_VST3_LOADER_TEST_FAIL");
  return selected && std::strcmp(selected, mode) == 0;
}

class FixtureMessage final : public Steinberg::Vst::IMessage {
public:
  explicit FixtureMessage(const char* id) : id_(id) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IMessage_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::FIDString PLUGIN_API getMessageID() override { return id_; }
  void PLUGIN_API setMessageID(Steinberg::FIDString id) override { id_ = id; }
  Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes() override { return nullptr; }

private:
  Steinberg::FIDString id_;
};

class GainComponent final : public Steinberg::Vst::IComponent,
                            public Steinberg::Vst::IAudioProcessor,
                            public Steinberg::Vst::IConnectionPoint {
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
    else if (std::memcmp(iid, Steinberg::Vst::IConnectionPoint_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
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
  Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override {
    record("component-connect");
    if (fail_mode("component-connect") || !other)
      return Steinberg::kResultFalse;
    peer_ = other;
    peer_->addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) override {
    record("component-disconnect");
    if (peer_ && other != peer_)
      return Steinberg::kResultFalse;
    if (peer_)
      peer_->release();
    peer_ = nullptr;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override {
    if (!message || std::strcmp(message->getMessageID(), "controller-message") != 0)
      return Steinberg::kResultFalse;
    record("component-notify");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown*) override {
    record("component-initialize");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override {
    record("component-terminate");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getControllerClassId(Steinberg::TUID id) override {
    std::memcpy(id, controller_id, sizeof(controller_id));
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setIoMode(Steinberg::Vst::IoMode) override {
    return Steinberg::kResultOk;
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IComponent's signature.
  Steinberg::int32 PLUGIN_API getBusCount(Steinberg::Vst::MediaType type,
                                          Steinberg::Vst::BusDirection direction) override {
    if (type != Steinberg::Vst::kAudio)
      return 0;
    return fixture_sidechain && direction == Steinberg::Vst::kInput ? 2 : 1;
  }
  // NOLINTBEGIN(bugprone-easily-swappable-parameters): IComponent's signature.
  Steinberg::tresult PLUGIN_API getBusInfo(Steinberg::Vst::MediaType,
                                           Steinberg::Vst::BusDirection direction,
                                           Steinberg::int32 index,
                                           Steinberg::Vst::BusInfo& info) override {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    info = {};
    info.mediaType = Steinberg::Vst::kAudio;
    info.direction = direction;
    info.channelCount = 2;
    info.busType = index == 1 ? Steinberg::Vst::kAux : Steinberg::Vst::kMain;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getRoutingInfo(Steinberg::Vst::RoutingInfo&,
                                               Steinberg::Vst::RoutingInfo&) override {
    return Steinberg::kNoInterface;
  }
  Steinberg::tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType, Steinberg::Vst::BusDirection,
                                            Steinberg::int32 index,
                                            Steinberg::TBool active) override {
    if (fixture_sidechain && index == 1)
      auxiliary_active_ = active;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    record("component-state");
    float gain = 0.0F;
    Steinberg::int32 read = 0;
    if (stream->read(&gain, sizeof(gain), &read) != Steinberg::kResultOk || read != sizeof(gain) ||
        !std::isfinite(gain))
      return Steinberg::kResultFalse;
    gain_ = gain;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    if (peer_) {
      FixtureMessage message("component-message");
      if (peer_->notify(&message) != Steinberg::kResultOk)
        return Steinberg::kResultFalse;
    }
    return stream->write(&gain_, sizeof(gain_)) == Steinberg::kResultOk ? Steinberg::kResultOk
                                                                        : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                   Steinberg::int32 input_count,
                                                   Steinberg::Vst::SpeakerArrangement* outputs,
                                                   Steinberg::int32 output_count) override {
    return input_count == (fixture_sidechain ? 2 : 1) &&
                   (!fixture_sidechain || inputs[1] == Steinberg::Vst::SpeakerArr::kStereo) &&
                   output_count == 1 && *inputs == Steinberg::Vst::SpeakerArr::kStereo &&
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
  Steinberg::uint32 PLUGIN_API getLatencySamples() override { return 7; }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    return setup.symbolicSampleSize == Steinberg::Vst::kSample32 && setup.sampleRate == 48000.0 &&
#ifdef OSCILLATE_FIXTURE_TRUE_LATENCY
                   (setup.maxSamplesPerBlock == 64 || setup.maxSamplesPerBlock == 128)
#else
                   setup.maxSamplesPerBlock == 128
#endif
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (data.numSamples == 0) {
      if (fail_mode("flush") || data.numInputs != 0 || data.numOutputs != 0 ||
          !data.inputParameterChanges || data.inputParameterChanges->getParameterCount() != 1)
        return Steinberg::kResultFalse;
      auto* queue = data.inputParameterChanges->getParameterData(0);
      Steinberg::int32 offset = -1;
      Steinberg::Vst::ParamValue value = -1.0;
      if (!queue || queue->getParameterId() != 101 || queue->getPointCount() != 1 ||
          queue->getPoint(0, offset, value) != Steinberg::kResultOk || offset != 0)
        return Steinberg::kResultFalse;
      gain_ = static_cast<float>(value * 4.0);
      return Steinberg::kResultOk;
    }
    if (data.numInputs != (fixture_sidechain ? 2 : 1) ||
        (fixture_sidechain && !auxiliary_active_) || data.numOutputs != 1 || data.numSamples > 128)
      return Steinberg::kResultFalse;
    auto* queue = data.inputParameterChanges && data.inputParameterChanges->getParameterCount() > 0
                      ? data.inputParameterChanges->getParameterData(0)
                      : nullptr;
    Steinberg::int32 point = 0;
    for (int frame = 0; frame < data.numSamples; ++frame) {
      if (queue && queue->getParameterId() == 101 && point < queue->getPointCount()) {
        Steinberg::int32 offset = -1;
        Steinberg::Vst::ParamValue value = 0.0;
        if (queue->getPoint(point, offset, value) == Steinberg::kResultOk && offset == frame) {
          gain_ = static_cast<float>(value * 4.0);
          ++point;
        }
      }
      for (int channel = 0; channel < 2; ++channel) {
        const float input = data.inputs[0].channelBuffers32[channel][frame];
        const float processed =
            input * gain_ +
            (fixture_sidechain ? data.inputs[1].channelBuffers32[channel][frame] : 0.0F);
#ifdef OSCILLATE_FIXTURE_TRUE_LATENCY
        // This variant really emits seven frames late and has a three-frame
        // post-latency echo. It proves that the export PDC and tail bounds are
        // audible behavior, rather than trusting a latency declaration alone.
        const float delayed = latency_history_[channel][latency_cursor_];
        const float echo = tail_history_[channel][tail_cursor_];
        latency_history_[channel][latency_cursor_] = processed;
        tail_history_[channel][tail_cursor_] = processed;
        data.outputs[0].channelBuffers32[channel][frame] = delayed + 0.5F * echo;
#else
        data.outputs[0].channelBuffers32[channel][frame] = processed;
#endif
      }
#ifdef OSCILLATE_FIXTURE_TRUE_LATENCY
      latency_cursor_ = (latency_cursor_ + 1U) % 7U;
      tail_cursor_ = (tail_cursor_ + 1U) % 10U;
#endif
    }
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API getTailSamples() override {
#ifdef OSCILLATE_FIXTURE_TRUE_LATENCY
    return 3;
#else
    return 0;
#endif
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
  float gain_{2.0F};
  Steinberg::Vst::IConnectionPoint* peer_{nullptr};
  bool auxiliary_active_{false};
#ifdef OSCILLATE_FIXTURE_TRUE_LATENCY
  float latency_history_[2][7]{};
  float tail_history_[2][10]{};
  unsigned latency_cursor_{0};
  unsigned tail_cursor_{0};
#endif
};

class GainController final : public Steinberg::Vst::IEditController,
                             public Steinberg::Vst::IConnectionPoint {
public:
  GainController() { record("controller-create"); }
  ~GainController() { record("controller-destroy"); }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IEditController_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IEditController*>(this);
    else if (std::memcmp(iid, Steinberg::Vst::IConnectionPoint_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
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
  Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override {
    record("controller-connect");
    if (fail_mode("controller-connect") || !other)
      return Steinberg::kResultFalse;
    peer_ = other;
    peer_->addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) override {
    record("controller-disconnect");
    if (peer_ && other != peer_)
      return Steinberg::kResultFalse;
    if (peer_)
      peer_->release();
    peer_ = nullptr;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override {
    if (!message || std::strcmp(message->getMessageID(), "component-message") != 0)
      return Steinberg::kResultFalse;
    record("controller-notify");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown*) override {
    record("controller-initialize");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override {
    record("controller-terminate");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* stream) override {
    if (fail_mode("controller-component-not-implemented")) {
      record("controller-state-not-implemented");
      return Steinberg::kNotImplemented;
    }
    float gain = 0.0F;
    Steinberg::int32 read = 0;
    if (stream->read(&gain, sizeof(gain), &read) != Steinberg::kResultOk || read != sizeof(gain) ||
        !std::isfinite(gain))
      return Steinberg::kResultFalse;
    normalized_ = static_cast<double>(gain) / 4.0;
    record("controller-state");
    if (peer_) {
      FixtureMessage message("controller-message");
      return peer_->notify(&message);
    }
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    record("controller-private-restore");
    if (fail_mode("controller-private-restore"))
      return Steinberg::kResultFalse;
    Steinberg::int32 count = 0;
    std::uint8_t value = 0;
    if (stream->read(&value, sizeof(value), &count) != Steinberg::kResultOk ||
        count != sizeof(value))
      return Steinberg::kResultFalse;
    private_value_ = value;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    record("controller-private-save");
    if (fail_mode("controller-private-save"))
      return Steinberg::kResultFalse;
    if (fail_mode("controller-private-oversized")) {
      std::array<std::uint8_t, 4096> chunk{};
      for (int part = 0; part <= 1024; ++part) {
        if (stream->write(chunk.data(), static_cast<Steinberg::int32>(chunk.size())) !=
            Steinberg::kResultOk)
          return Steinberg::kResultFalse;
      }
    }
    return stream->write(&private_value_, sizeof(private_value_));
  }
  Steinberg::int32 PLUGIN_API getParameterCount() override { return 1; }
  Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 index,
                                                 Steinberg::Vst::ParameterInfo& info) override {
    if (index != 0)
      return Steinberg::kInvalidArgument;
    info = {};
    info.id = 101;
    info.title[0] = u'G';
    info.title[1] = u'a';
    info.title[2] = u'i';
    info.title[3] = u'n';
    info.defaultNormalizedValue = 0.5;
    info.flags = Steinberg::Vst::ParameterInfo::kCanAutomate;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID,
                                                      Steinberg::Vst::ParamValue,
                                                      Steinberg::Vst::String128) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID,
                                                      Steinberg::Vst::TChar*,
                                                      Steinberg::Vst::ParamValue&) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::Vst::ParamValue PLUGIN_API
  normalizedParamToPlain(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue value) override {
    return value * 4.0;
  }
  Steinberg::Vst::ParamValue PLUGIN_API
  plainParamToNormalized(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue value) override {
    return value / 4.0;
  }
  Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID) override {
    return normalized_;
  }
  // The SDK fixes this convertible parameter order.
  Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id,
                                                   Steinberg::Vst::ParamValue value) override {
    if (fail_mode("controller-value") || id != 101)
      return Steinberg::kResultFalse;
    normalized_ = value;
    if (fail_mode("controller-private-edit"))
      private_value_ = 0x7b;
    record("controller-value");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString) override { return nullptr; }

private:
  std::atomic<Steinberg::uint32> references_{1};
  Steinberg::Vst::IConnectionPoint* peer_{nullptr};
  double normalized_{0.5};
  std::uint8_t private_value_{0x4d};
};

class GainFactory final : public Steinberg::IPluginFactory2 {
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
  Steinberg::int32 PLUGIN_API countClasses() override { return 2; }
  Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index,
                                             Steinberg::PClassInfo* info) override {
    if ((index != 0 && index != 1) || !info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PClassInfo(index == 0 ? component_id : controller_id,
                                  Steinberg::PClassInfo::kManyInstances,
                                  index == 0 ? kVstAudioEffectClass : kVstComponentControllerClass,
                                  index == 0 ? "Connected Gain" : "Connected Gain Controller");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index,
                                              Steinberg::PClassInfo2* info) override {
    if ((index != 0 && index != 1) || !info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PClassInfo2(
        index == 0 ? component_id : controller_id, Steinberg::PClassInfo::kManyInstances,
        index == 0 ? kVstAudioEffectClass : kVstComponentControllerClass,
        index == 0 ? "Connected Gain" : "Connected Gain Controller", 0,
        index == 0 ? "Fx" : "Controller", "Fixture Vendor", "1.0.0", "VST 3.8");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid,
                                               void** object) override {
    if (!object)
      return Steinberg::kNoInterface;
    if (std::memcmp(cid, component_id, sizeof(component_id)) == 0 &&
        std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(component_id)) == 0) {
      *object = static_cast<Steinberg::Vst::IComponent*>(new GainComponent());
      return Steinberg::kResultOk;
    }
    if (std::memcmp(cid, controller_id, sizeof(controller_id)) == 0 &&
        std::memcmp(iid, Steinberg::Vst::IEditController_iid, sizeof(controller_id)) == 0) {
      *object = static_cast<Steinberg::Vst::IEditController*>(new GainController());
      return Steinberg::kResultOk;
    }
    *object = nullptr;
    return Steinberg::kNoInterface;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
};

GainFactory factory;
} // namespace

extern "C" __attribute__((visibility("default"))) bool ModuleEntry(void*) {
  record("module-entry");
  return true;
}
extern "C" __attribute__((visibility("default"))) bool ModuleExit() {
  record("module-exit");
  return true;
}
extern "C" __attribute__((visibility("default"))) Steinberg::IPluginFactory* GetPluginFactory() {
  factory.addRef();
  return &factory;
}
