#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

#include <cstddef>
#include <vector>

namespace oscillate::plugin_ui {

// Owned and called exclusively on the Qt UI thread. The editor must detach
// and release every IRunLoop reference before this object is destroyed.
class QtVst3RunLoop final : public Steinberg::Linux::IRunLoop {
public:
  QtVst3RunLoop();
  ~QtVst3RunLoop();
  QtVst3RunLoop(const QtVst3RunLoop&) = delete;
  QtVst3RunLoop& operator=(const QtVst3RunLoop&) = delete;

  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override;
  Steinberg::uint32 PLUGIN_API addRef() override;
  Steinberg::uint32 PLUGIN_API release() override;
  Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* handler,
                                                     Steinberg::Linux::FileDescriptor fd) override;
  Steinberg::tresult PLUGIN_API
  unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) override;
  Steinberg::tresult PLUGIN_API
  registerTimer(Steinberg::Linux::ITimerHandler* handler,
                Steinberg::Linux::TimerInterval milliseconds) override;
  Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* handler) override;

private:
  struct FdEntry {
    Steinberg::Linux::IEventHandler* handler;
    int fd;
    QSocketNotifier* notifier;
  };
  struct TimerEntry {
    Steinberg::Linux::ITimerHandler* handler;
    QTimer* timer;
  };
  [[nodiscard]] bool on_owner_thread() const noexcept;
  std::vector<FdEntry> fds_;
  std::vector<TimerEntry> timers_;
  QThread* owner_;
  Steinberg::uint32 references_{1};
  static constexpr std::size_t max_registrations_ = 64;
};

} // namespace oscillate::plugin_ui
