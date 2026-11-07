#include "qt_vst3_run_loop.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace oscillate::plugin_ui {

QtVst3RunLoop::QtVst3RunLoop() : owner_(QThread::currentThread()) {}
QtVst3RunLoop::~QtVst3RunLoop() {
  for (auto& entry : fds_) {
    delete entry.notifier;
    entry.handler->release();
  }
  for (auto& entry : timers_) {
    delete entry.timer;
    entry.handler->release();
  }
}

bool QtVst3RunLoop::on_owner_thread() const noexcept { return QThread::currentThread() == owner_; }

Steinberg::tresult PLUGIN_API QtVst3RunLoop::queryInterface(const Steinberg::TUID iid,
                                                            void** object) {
  if (!object)
    return Steinberg::kInvalidArgument;
  *object = nullptr;
  if (!iid)
    return Steinberg::kInvalidArgument;
  if (std::memcmp(iid, Steinberg::Linux::IRunLoop_iid, sizeof(Steinberg::TUID)) != 0 &&
      std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0)
    return Steinberg::kNoInterface;
  *object = this;
  addRef();
  return Steinberg::kResultOk;
}
Steinberg::uint32 PLUGIN_API QtVst3RunLoop::addRef() { return ++references_; }
Steinberg::uint32 PLUGIN_API QtVst3RunLoop::release() { return --references_; }

Steinberg::tresult PLUGIN_API QtVst3RunLoop::registerEventHandler(
    Steinberg::Linux::IEventHandler* handler, Steinberg::Linux::FileDescriptor fd) {
  if (!on_owner_thread() || !handler || fd < 0 || fds_.size() >= max_registrations_ ||
      std::ranges::any_of(fds_, [fd](const FdEntry& entry) { return entry.fd == fd; }))
    return Steinberg::kInvalidArgument;
  try {
    auto notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    QObject::connect(notifier.get(), &QSocketNotifier::activated, notifier.get(),
                     [this, handler, fd] {
                       handler->addRef();
                       try {
                         handler->onFDIsSet(fd);
                       } catch (...) {
                         unregisterEventHandler(handler);
                       }
                       handler->release();
                     });
    handler->addRef();
    try {
      fds_.push_back({handler, fd, notifier.get()});
    } catch (...) {
      handler->release();
      throw;
    }
    fds_.back().notifier = notifier.release();
    return Steinberg::kResultTrue;
  } catch (...) {
    return Steinberg::kResultFalse;
  }
}
Steinberg::tresult PLUGIN_API
QtVst3RunLoop::unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) {
  if (!on_owner_thread() || !handler)
    return Steinberg::kInvalidArgument;
  if (std::ranges::find(fds_, handler, &FdEntry::handler) == fds_.end())
    return Steinberg::kResultFalse;
  // Remove every matching entry first: a final release() may destroy the
  // handler, whose destructor may unregister other handlers here.
  std::size_t released = 0;
  std::erase_if(fds_, [handler, &released](const FdEntry& entry) {
    if (entry.handler != handler)
      return false;
    entry.notifier->setEnabled(false);
    QObject::disconnect(entry.notifier, nullptr, entry.notifier, nullptr);
    entry.notifier->deleteLater();
    ++released;
    return true;
  });
  for (; released > 0; --released)
    handler->release();
  return Steinberg::kResultTrue;
}
Steinberg::tresult PLUGIN_API QtVst3RunLoop::registerTimer(
    Steinberg::Linux::ITimerHandler* handler, Steinberg::Linux::TimerInterval milliseconds) {
  if (!on_owner_thread() || !handler || milliseconds == 0 ||
      milliseconds >
          static_cast<Steinberg::Linux::TimerInterval>(std::numeric_limits<int>::max()) ||
      timers_.size() >= max_registrations_ ||
      std::ranges::any_of(timers_,
                          [handler](const TimerEntry& entry) { return entry.handler == handler; }))
    return Steinberg::kInvalidArgument;
  try {
    auto timer = std::make_unique<QTimer>();
    timer->setInterval(static_cast<int>(milliseconds));
    QObject::connect(timer.get(), &QTimer::timeout, timer.get(), [this, handler] {
      handler->addRef();
      try {
        handler->onTimer();
      } catch (...) {
        unregisterTimer(handler);
      }
      handler->release();
    });
    timer->start();
    handler->addRef();
    try {
      timers_.push_back({handler, timer.get()});
    } catch (...) {
      timer->stop();
      handler->release();
      throw;
    }
    timers_.back().timer = timer.release();
    return Steinberg::kResultTrue;
  } catch (...) {
    return Steinberg::kResultFalse;
  }
}
Steinberg::tresult PLUGIN_API
QtVst3RunLoop::unregisterTimer(Steinberg::Linux::ITimerHandler* handler) {
  if (!on_owner_thread() || !handler)
    return Steinberg::kInvalidArgument;
  const auto found = std::ranges::find(timers_, handler, &TimerEntry::handler);
  if (found == timers_.end())
    return Steinberg::kResultFalse;
  found->timer->stop();
  QObject::disconnect(found->timer, nullptr, found->timer, nullptr);
  found->timer->deleteLater();
  // Erase before releasing: a final release() may unregister other timers.
  timers_.erase(found);
  handler->release();
  return Steinberg::kResultTrue;
}

} // namespace oscillate::plugin_ui
