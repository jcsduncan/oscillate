#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Synthetic stereo effect for automation event-capacity tests. Parameter 1 is
// a gain (output = input * value); parameters 2..parameter_count are inert
// continuous lanes. Every point delivered to the latest audio process call is
// logged for an independent test oracle. Not a claim of third-party support.
namespace {
constexpr Steinberg::int32 parameter_count = 65;
constexpr Steinberg::Vst::ParamID gain_id = 1;

struct LoggedPoint {
  std::uint32_t id{0};
  std::int32_t offset{0};
  double value{0.0};
};
constexpr std::size_t max_logged_points = 4096;
LoggedPoint logged_points[max_logged_points];
std::size_t logged_count = 0;
std::size_t process_calls = 0;

const Steinberg::TUID class_id = {90, 91, 92,  93,  94,  95,  96,  97,
                                  98, 99, 100, 101, 102, 103, 104, 105};

class LaneEffect final : public Steinberg::Vst::IComponent,
                         public Steinberg::Vst::IAudioProcessor,
                         public Steinberg::Vst::IEditController {
public:
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IComponent*>(this);
    else if (std::memcmp(iid, Steinberg::Vst::IAudioProcessor_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IAudioProcessor*>(this);
    else if (std::memcmp(iid, Steinberg::Vst::IEditController_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IEditController*>(this);
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
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override { return Steinberg::kResultOk; }
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
  // Stateless apart from the gain: saved state is empty and ignored.
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API getParameterCount() override { return parameter_count; }
  Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 index,
                                                 Steinberg::Vst::ParameterInfo& info) override {
    if (index < 0 || index >= parameter_count)
      return Steinberg::kInvalidArgument;
    info = {};
    info.id = static_cast<Steinberg::Vst::ParamID>(index + 1);
    info.title[0] = index == 0 ? u'G' : u'L';
    info.title[1] = u'a';
    info.title[2] = index == 0 ? u'i' : u'n';
    info.title[3] = index == 0 ? u'n' : u'e';
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
    return value;
  }
  Steinberg::Vst::ParamValue PLUGIN_API
  plainParamToNormalized(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue value) override {
    return value;
  }
  Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID id) override {
    return id == gain_id ? gain_ : 0.5;
  }
  // The SDK fixes this convertible parameter order.
  Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id,
                                                   Steinberg::Vst::ParamValue value) override {
    if (id < 1 || id > static_cast<Steinberg::Vst::ParamID>(parameter_count) ||
        !std::isfinite(value) || value < 0.0 || value > 1.0)
      return Steinberg::kResultFalse;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString) override { return nullptr; }
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                   Steinberg::int32 input_count,
                                                   Steinberg::Vst::SpeakerArrangement* outputs,
                                                   Steinberg::int32 output_count) override {
    return input_count == 1 && output_count == 1 && inputs && outputs &&
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
  Steinberg::uint32 PLUGIN_API getLatencySamples() override { return 0; }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    if (setup.symbolicSampleSize != Steinberg::Vst::kSample32 || setup.sampleRate != 48000.0 ||
        (setup.maxSamplesPerBlock != 64 && setup.maxSamplesPerBlock != 128))
      return Steinberg::kResultFalse;
    max_frames_ = setup.maxSamplesPerBlock;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (data.numSamples == 0)
      return Steinberg::kResultOk; // Static parameter flush; this fixture keeps no state.
    if (data.numInputs != 1 || data.numOutputs != 1 || data.numSamples < 1 ||
        data.numSamples > max_frames_)
      return Steinberg::kResultFalse;
    ++process_calls;
    logged_count = 0;
    const auto queue_count =
        data.inputParameterChanges ? data.inputParameterChanges->getParameterCount() : 0;
    if (queue_count < 0 || queue_count > parameter_count)
      return Steinberg::kResultFalse;
    Steinberg::Vst::IParamValueQueue* gain_queue = nullptr;
    for (Steinberg::int32 queue_index = 0; queue_index < queue_count; ++queue_index) {
      auto* queue = data.inputParameterChanges->getParameterData(queue_index);
      if (!queue || queue->getParameterId() < 1 ||
          queue->getParameterId() > static_cast<Steinberg::Vst::ParamID>(parameter_count))
        return Steinberg::kResultFalse;
      if (queue->getParameterId() == gain_id)
        gain_queue = queue;
      Steinberg::int32 previous = -1;
      for (Steinberg::int32 point = 0; point < queue->getPointCount(); ++point) {
        Steinberg::int32 offset = -1;
        Steinberg::Vst::ParamValue value = -1.0;
        if (queue->getPoint(point, offset, value) != Steinberg::kResultOk || offset <= previous ||
            offset >= data.numSamples || logged_count == max_logged_points)
          return Steinberg::kResultFalse;
        logged_points[logged_count++] = {queue->getParameterId(), offset, value};
        previous = offset;
      }
    }
    Steinberg::int32 gain_point = 0;
    for (Steinberg::int32 frame = 0; frame < data.numSamples; ++frame) {
      while (gain_queue && gain_point < gain_queue->getPointCount()) {
        Steinberg::int32 offset = -1;
        Steinberg::Vst::ParamValue value = 0.0;
        if (gain_queue->getPoint(gain_point, offset, value) != Steinberg::kResultOk ||
            offset > frame)
          break;
        gain_ = value;
        ++gain_point;
      }
      for (int channel = 0; channel < 2; ++channel)
        data.outputs[0].channelBuffers32[channel][frame] =
            data.inputs[0].channelBuffers32[channel][frame] * static_cast<float>(gain_);
    }
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
  std::atomic<Steinberg::uint32> references_{1};
  double gain_{0.5};
  Steinberg::int32 max_frames_{0};
};

class Factory final : public Steinberg::IPluginFactory2 {
public:
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::IPluginFactory_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::IPluginFactory2_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0) {
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
  Steinberg::int32 PLUGIN_API countClasses() override { return 1; }
  Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index,
                                             Steinberg::PClassInfo* info) override {
    if (index != 0 || !info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PClassInfo(class_id, Steinberg::PClassInfo::kManyInstances,
                                  kVstAudioEffectClass, "Lane Effect");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index,
                                              Steinberg::PClassInfo2* info) override {
    if (index != 0 || !info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PClassInfo2(class_id, Steinberg::PClassInfo::kManyInstances,
                                   kVstAudioEffectClass, "Lane Effect", 0, "Fx", "Fixture Vendor",
                                   "1.0.0", "VST 3.8");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid,
                                               void** object) override {
    if (!object || std::memcmp(cid, class_id, sizeof(class_id)) != 0 ||
        std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(class_id)) != 0)
      return Steinberg::kNoInterface;
    *object = static_cast<Steinberg::Vst::IComponent*>(new LaneEffect());
    return Steinberg::kResultOk;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
};
Factory factory;
} // namespace

extern "C" __attribute__((visibility("default"))) bool ModuleEntry(void*) { return true; }
extern "C" __attribute__((visibility("default"))) bool ModuleExit() { return true; }
extern "C" __attribute__((visibility("default"))) Steinberg::IPluginFactory* GetPluginFactory() {
  factory.addRef();
  return &factory;
}
extern "C" __attribute__((visibility("default"))) std::size_t OscillateFixtureProcessCalls() {
  return process_calls;
}
extern "C" __attribute__((visibility("default"))) std::size_t
OscillateFixtureParameterPointCount() {
  return logged_count;
}
extern "C" __attribute__((visibility("default"))) bool
OscillateFixtureParameterPoint(std::size_t index, std::uint32_t* id, std::int32_t* offset,
                               double* value) {
  if (index >= logged_count || !id || !offset || !value)
    return false;
  *id = logged_points[index].id;
  *offset = logged_points[index].offset;
  *value = logged_points[index].value;
  return true;
}
