#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The default build is TimedInstrument with one Level parameter (ID 1). The
// LaneInstrument build sets this to expose inert continuous parameters 2..N
// for automation-capacity tests; it has its own class ID and name.
#ifndef OSCILLATE_FIXTURE_PARAMETERS
#define OSCILLATE_FIXTURE_PARAMETERS 1
#endif

namespace {
constexpr Steinberg::int32 parameter_count = OSCILLATE_FIXTURE_PARAMETERS;
static_assert(parameter_count >= 1 && parameter_count <= 128);
constexpr bool lane_build = parameter_count > 1;

struct LoggedPoint {
  std::uint32_t id{0};
  std::int32_t offset{0};
  double value{0.0};
};
// Every parameter point delivered to the most recent audio process call.
constexpr std::size_t max_logged_points = 4096;
LoggedPoint logged_points[max_logged_points];
std::size_t logged_count = 0;

struct TransportSnapshot {
  std::int64_t sample{0};
  double quarter{0.0};
  double tempo{0.0};
  bool playing{false};
};
TransportSnapshot last_transport;
const Steinberg::TUID class_id = {70, 71, 72, 73, 74, 75, 76, 77,
                                  78, 79, 80, 81, 82, 83, 84, lane_build ? 86 : 85};
constexpr const char* class_name = lane_build ? "Lane Instrument" : "Timed Instrument";

// With OSCILLATE_VST3_INSTRUMENT_PEDAL set, the controller's IMidiMapping
// assigns CC64 (sustain) to a hidden parameter that is not in the parameter
// list, as MIDI-learn targets often are, and process() accepts and logs it.
// Set to "visible", it assigns CC64 to the listed level parameter instead,
// which manual edits may also reach.
constexpr Steinberg::Vst::ParamID sustain_parameter = 900;
constexpr Steinberg::Vst::ParamID level_parameter = 1;
bool pedal_mode() { return std::getenv("OSCILLATE_VST3_INSTRUMENT_PEDAL") != nullptr; }
bool visible_pedal() {
  const auto* value = std::getenv("OSCILLATE_VST3_INSTRUMENT_PEDAL");
  return value && std::strcmp(value, "visible") == 0;
}

bool fail_mode(const char* mode) {
  const auto* value = std::getenv("OSCILLATE_VST3_INSTRUMENT_FAIL");
  return value && std::strcmp(value, mode) == 0;
}

class Instrument final : public Steinberg::Vst::IComponent,
                         public Steinberg::Vst::IAudioProcessor,
                         public Steinberg::Vst::IEditController,
                         public Steinberg::Vst::IMidiMapping {
public:
  Instrument() { inert_values_.fill(0.5); }
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
    else if (pedal_mode() &&
             std::memcmp(iid, Steinberg::Vst::IMidiMapping_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Vst::IMidiMapping*>(this);
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
  Steinberg::tresult PLUGIN_API setIoMode(Steinberg::Vst::IoMode mode) override {
    return !fail_mode("require-offline") || mode == Steinberg::Vst::kOfflineProcessing
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::int32 PLUGIN_API getBusCount(Steinberg::Vst::MediaType type,
                                          Steinberg::Vst::BusDirection direction) override {
    if (type == Steinberg::Vst::kAudio)
      return direction == Steinberg::Vst::kOutput ? 1 : 0;
    if (type == Steinberg::Vst::kEvent && direction == Steinberg::Vst::kInput)
      return fail_mode("two-event-buses") ? 2 : 1;
    return 0;
  }
  // The VST3 ABI fixes this argument order and integral parameter types.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Steinberg::tresult PLUGIN_API getBusInfo(Steinberg::Vst::MediaType type,
                                           Steinberg::Vst::BusDirection direction,
                                           Steinberg::int32 index,
                                           Steinberg::Vst::BusInfo& info) override {
    if (index != 0)
      return Steinberg::kInvalidArgument;
    info = {};
    info.mediaType = type;
    info.direction = direction;
    info.busType = Steinberg::Vst::kMain;
    info.channelCount = type == Steinberg::Vst::kEvent ? 1 : 2;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getRoutingInfo(Steinberg::Vst::RoutingInfo&,
                                               Steinberg::Vst::RoutingInfo&) override {
    return Steinberg::kNoInterface;
  }
  Steinberg::tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType, Steinberg::Vst::BusDirection,
                                            Steinberg::int32 index, Steinberg::TBool) override {
    return index == 0 ? Steinberg::kResultOk : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    Steinberg::int32 count = 0;
    float value = 0.0F;
    if (stream->read(&value, sizeof(value), &count) != Steinberg::kResultOk ||
        count != sizeof(value) || !std::isfinite(value) || value < 0.0F || value > 1.0F)
      return Steinberg::kResultFalse;
    level_ = value;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    return stream->write(&level_, sizeof(level_)) == Steinberg::kResultOk ? Steinberg::kResultOk
                                                                          : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* stream) override {
    Steinberg::int32 count = 0;
    float value = 0.0F;
    if (stream->read(&value, sizeof(value), &count) != Steinberg::kResultOk ||
        count != sizeof(value) || !std::isfinite(value) || value < 0.0F || value > 1.0F)
      return Steinberg::kResultFalse;
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API getParameterCount() override { return parameter_count; }
  Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 index,
                                                 Steinberg::Vst::ParameterInfo& info) override {
    if (index < 0 || index >= parameter_count)
      return Steinberg::kInvalidArgument;
    info = {};
    info.id = static_cast<Steinberg::Vst::ParamID>(index + 1);
    info.title[0] = u'L';
    info.title[1] = index == 0 ? u'e' : u'a';
    info.title[2] = index == 0 ? u'v' : u'n';
    info.title[3] = u'e';
    info.title[4] = index == 0 ? u'l' : u'\0';
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
    if (id < 1 || id > static_cast<Steinberg::Vst::ParamID>(parameter_count))
      return 0.0;
    return id == 1 ? level_ : inert_values_[id - 2];
  }
  Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id,
                                                   Steinberg::Vst::ParamValue value) override {
    if (id < 1 || id > static_cast<Steinberg::Vst::ParamID>(parameter_count) ||
        !std::isfinite(value) || value < 0.0 || value > 1.0)
      return Steinberg::kResultFalse;
    if (id == 1)
      level_ = static_cast<float>(value);
    else
      inert_values_[id - 2] = value;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString) override { return nullptr; }
  // The VST3 ABI fixes this argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Steinberg::tresult PLUGIN_API getMidiControllerAssignment(Steinberg::int32 bus,
                                                            Steinberg::int16 channel,
                                                            Steinberg::Vst::CtrlNumber controller,
                                                            Steinberg::Vst::ParamID& id) override {
    if (bus != 0 || channel != 0 || controller != 64)
      return Steinberg::kResultFalse;
    id = visible_pedal() ? level_parameter : sustain_parameter;
    return Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement*,
                                                   Steinberg::int32 input_count,
                                                   Steinberg::Vst::SpeakerArrangement* outputs,
                                                   Steinberg::int32 output_count) override {
    return input_count == 0 && output_count == 1 && outputs &&
                   *outputs == Steinberg::Vst::SpeakerArr::kStereo
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API
  getBusArrangement(Steinberg::Vst::BusDirection direction, Steinberg::int32 index,
                    Steinberg::Vst::SpeakerArrangement& arrangement) override {
    if (direction != Steinberg::Vst::kOutput || index != 0)
      return Steinberg::kResultFalse;
    arrangement = Steinberg::Vst::SpeakerArr::kStereo;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
    return size == Steinberg::Vst::kSample32 ? Steinberg::kResultOk : Steinberg::kResultFalse;
  }
  Steinberg::uint32 PLUGIN_API getLatencySamples() override { return 0; }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    if (fail_mode("require-offline") && setup.processMode != Steinberg::Vst::kOffline)
      return Steinberg::kResultFalse;
    return setup.symbolicSampleSize == Steinberg::Vst::kSample32 && setup.sampleRate == 48000.0 &&
                   setup.maxSamplesPerBlock == 128
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (fail_mode("require-offline") && data.processMode != Steinberg::Vst::kOffline)
      return Steinberg::kResultFalse;
    if (data.numSamples == 0) {
      if (data.numInputs != 0 || data.numOutputs != 0 || !data.inputParameterChanges ||
          data.inputParameterChanges->getParameterCount() != 1)
        return Steinberg::kResultFalse;
      auto* queue = data.inputParameterChanges->getParameterData(0);
      Steinberg::int32 offset = -1;
      Steinberg::Vst::ParamValue value = -1.0;
      if (!queue || queue->getParameterId() < 1 ||
          queue->getParameterId() > static_cast<Steinberg::Vst::ParamID>(parameter_count) ||
          queue->getPointCount() != 1 ||
          queue->getPoint(0, offset, value) != Steinberg::kResultOk || offset != 0)
        return Steinberg::kResultFalse;
      if (queue->getParameterId() == 1)
        level_ = static_cast<float>(value);
      else
        inert_values_[queue->getParameterId() - 2] = value;
      return Steinberg::kResultOk;
    }
    if (data.numInputs != 0 || data.numOutputs != 1 || !data.outputs || data.numSamples < 1 ||
        data.numSamples > 128 || !data.inputEvents || !data.processContext)
      return Steinberg::kResultFalse;
    const auto& context = *data.processContext;
    constexpr auto required = Steinberg::Vst::ProcessContext::kTempoValid |
                              Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
    if ((context.state & required) != required || context.sampleRate != 48000.0 ||
        context.projectTimeSamples < 0 || !std::isfinite(context.projectTimeMusic) ||
        !std::isfinite(context.tempo) || context.tempo <= 0.0)
      return Steinberg::kResultFalse;
    last_transport = {.sample = context.projectTimeSamples,
                      .quarter = context.projectTimeMusic,
                      .tempo = context.tempo,
                      .playing = (context.state & Steinberg::Vst::ProcessContext::kPlaying) != 0};
    const auto event_count = data.inputEvents->getEventCount();
    const auto queue_count =
        data.inputParameterChanges ? data.inputParameterChanges->getParameterCount() : 0;
    if (queue_count < 0 || queue_count > parameter_count + (pedal_mode() ? 1 : 0))
      return Steinberg::kResultFalse;
    Steinberg::Vst::IParamValueQueue* parameter_queue = nullptr;
    logged_count = 0;
    for (Steinberg::int32 queue_index = 0; queue_index < queue_count; ++queue_index) {
      auto* queue = data.inputParameterChanges->getParameterData(queue_index);
      const bool sustain = pedal_mode() && queue && queue->getParameterId() == sustain_parameter;
      if (!queue ||
          ((queue->getParameterId() < 1 ||
            queue->getParameterId() > static_cast<Steinberg::Vst::ParamID>(parameter_count)) &&
           !sustain))
        return Steinberg::kResultFalse;
      if (queue->getParameterId() == 1)
        parameter_queue = queue;
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
    Steinberg::int32 parameter_index = 0;
    int event_index = 0;
    Steinberg::Vst::Event event{};
    for (int frame = 0; frame < data.numSamples; ++frame) {
      if (parameter_queue) {
        while (parameter_index < parameter_queue->getPointCount()) {
          Steinberg::int32 offset = -1;
          Steinberg::Vst::ParamValue value = -1.0;
          if (parameter_queue->getPoint(parameter_index, offset, value) != Steinberg::kResultOk ||
              offset < frame)
            return Steinberg::kResultFalse;
          if (offset != frame)
            break;
          level_ = static_cast<float>(value);
          ++parameter_index;
        }
      }
      while (event_index < event_count) {
        if (data.inputEvents->getEvent(event_index, event) != Steinberg::kResultOk ||
            event.busIndex != 0 || event.sampleOffset < frame)
          return Steinberg::kResultFalse;
        if (event.sampleOffset != frame)
          break;
        if (event.type == Steinberg::Vst::Event::kNoteOnEvent) {
          if (event.noteOn.channel != 0 || event.noteOn.pitch != 60)
            return Steinberg::kResultFalse;
          active_ = true;
          velocity_ = event.noteOn.velocity;
        } else if (event.type == Steinberg::Vst::Event::kNoteOffEvent) {
          if (event.noteOff.channel != 0 || event.noteOff.pitch != 60)
            return Steinberg::kResultFalse;
          active_ = false;
          const auto* tail_setting = std::getenv("OSCILLATE_VST3_INSTRUMENT_TAIL");
          tail_left_ = tail_setting && (std::strcmp(tail_setting, "8") == 0 ||
                                        std::strcmp(tail_setting, "dishonest") == 0)
                           ? 8
                           : 0;
        } else {
          return Steinberg::kResultFalse;
        }
        ++event_index;
      }
      float sample = active_ ? level_ * velocity_ : 0.0F;
      if (!active_ && tail_left_ > 0) {
        sample = level_ * velocity_ * static_cast<float>(tail_left_) / 8.0F;
        --tail_left_;
      }
      data.outputs[0].channelBuffers32[0][frame] = sample;
      data.outputs[0].channelBuffers32[1][frame] = -sample;
    }
    return fail_mode("process") ? Steinberg::kResultFalse : Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API getTailSamples() override {
    const auto* setting = std::getenv("OSCILLATE_VST3_INSTRUMENT_TAIL");
    if (setting && std::strcmp(setting, "infinite") == 0)
      return UINT32_MAX;
    if (setting && std::strcmp(setting, "huge") == 0)
      return 480'001;
    return setting && std::strcmp(setting, "8") == 0 ? 8 : 0;
  }

private:
  std::atomic<Steinberg::uint32> references_{1};
  float level_{0.5F};
  std::array<double, static_cast<std::size_t>(parameter_count - 1)> inert_values_{};
  float velocity_{0.0F};
  bool active_{false};
  int tail_left_{0};
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
                                  "Audio Module Class", class_name);
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index,
                                              Steinberg::PClassInfo2* info) override {
    if (index != 0 || !info)
      return Steinberg::kInvalidArgument;
    *info = Steinberg::PClassInfo2(class_id, Steinberg::PClassInfo::kManyInstances,
                                   "Audio Module Class", class_name, 0, "Instrument",
                                   "Fixture Vendor", "1.0.0", "VST 3.8");
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid,
                                               void** object) override {
    if (!object || std::memcmp(cid, class_id, sizeof(class_id)) != 0 ||
        std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(class_id)) != 0)
      return Steinberg::kNoInterface;
    *object = static_cast<Steinberg::Vst::IComponent*>(new Instrument());
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
extern "C" __attribute__((visibility("default"))) bool
OscillateFixtureTransport(std::int64_t* sample, double* quarter, double* tempo, bool* playing) {
  if (!sample || !quarter || !tempo || !playing)
    return false;
  *sample = last_transport.sample;
  *quarter = last_transport.quarter;
  *tempo = last_transport.tempo;
  *playing = last_transport.playing;
  return true;
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
