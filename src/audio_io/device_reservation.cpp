#include <oscillate/audio_io/device_reservation.hpp>

#include <oscillate/foundation/log.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <dbus/dbus.h>

namespace oscillate::audio_io {
namespace {
constexpr const char* service_prefix = "org.freedesktop.ReserveDevice1.Audio";
constexpr const char* path_prefix = "/org/freedesktop/ReserveDevice1/Audio";
constexpr const char* reserve_interface = "org.freedesktop.ReserveDevice1";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";
// How long the answering thread waits for a message before it looks at its
// stop request again.
constexpr int dispatch_timeout_ms = 100;

class ScopedError {
public:
  ScopedError() { dbus_error_init(&error_); }
  ScopedError(const ScopedError&) = delete;
  ScopedError& operator=(const ScopedError&) = delete;
  ~ScopedError() { dbus_error_free(&error_); }
  DBusError* get() noexcept { return &error_; }
  [[nodiscard]] std::string message() const {
    return dbus_error_is_set(&error_) != 0 && error_.message != nullptr ? error_.message
                                                                        : "no reply";
  }

private:
  DBusError error_{};
};

struct MessageRef {
  DBusMessage* message = nullptr;
  MessageRef() = default;
  explicit MessageRef(DBusMessage* value) : message(value) {}
  MessageRef(const MessageRef&) = delete;
  MessageRef& operator=(const MessageRef&) = delete;
  ~MessageRef() {
    if (message != nullptr)
      dbus_message_unref(message);
  }
};
} // namespace

struct DbusDeviceReservation::Impl {
  std::string application_name;
  std::string service;
  std::string path;
  std::string device_name;
  DBusConnection* connection = nullptr;
  bool path_registered = false;
  std::atomic<bool> stop{false};
  std::thread dispatcher;

  // Answers calls to the reservation object: refuses RequestRelease, and
  // reports who holds the card.
  static DBusHandlerResult on_message(DBusConnection* connection, DBusMessage* message,
                                      void* data) {
    const auto& self = *static_cast<const Impl*>(data);
    if (dbus_message_is_method_call(message, reserve_interface, "RequestRelease") != 0) {
      MessageRef reply(dbus_message_new_method_return(message));
      dbus_bool_t granted = FALSE;
      if (reply.message == nullptr || dbus_message_append_args(reply.message, DBUS_TYPE_BOOLEAN,
                                                               &granted, DBUS_TYPE_INVALID) == 0)
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
      (void)dbus_connection_send(connection, reply.message, nullptr);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, properties_interface, "Get") != 0) {
      const char* interface = nullptr;
      const char* property = nullptr;
      ScopedError error;
      if (dbus_message_get_args(message, error.get(), DBUS_TYPE_STRING, &interface,
                                DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID) == 0 ||
          std::strcmp(interface, reserve_interface) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      MessageRef reply(dbus_message_new_method_return(message));
      if (reply.message == nullptr)
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
      DBusMessageIter iterator;
      DBusMessageIter variant;
      dbus_message_iter_init_append(reply.message, &iterator);
      bool appended = false;
      if (std::strcmp(property, "Priority") == 0) {
        const dbus_int32_t value = DbusDeviceReservation::priority;
        appended = dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT,
                                                    DBUS_TYPE_INT32_AS_STRING, &variant) != 0 &&
                   dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value) != 0 &&
                   dbus_message_iter_close_container(&iterator, &variant) != 0;
      } else if (std::strcmp(property, "ApplicationName") == 0 ||
                 std::strcmp(property, "ApplicationDeviceName") == 0) {
        const char* value = std::strcmp(property, "ApplicationName") == 0
                                ? self.application_name.c_str()
                                : self.device_name.c_str();
        appended = dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT,
                                                    DBUS_TYPE_STRING_AS_STRING, &variant) != 0 &&
                   dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
                                                  static_cast<const void*>(&value)) != 0 &&
                   dbus_message_iter_close_container(&iterator, &variant) != 0;
      } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }
      if (!appended)
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
      (void)dbus_connection_send(connection, reply.message, nullptr);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  [[nodiscard]] int request_name(unsigned int flags, std::string& failure) {
    ScopedError error;
    const int result = dbus_bus_request_name(connection, service.c_str(), flags, error.get());
    if (result < 0)
      failure = error.message();
    return result;
  }

  // Asks the current owner to give the card up; true when it agreed.
  [[nodiscard]] bool request_release(std::string& failure) {
    MessageRef call(dbus_message_new_method_call(service.c_str(), path.c_str(), reserve_interface,
                                                 "RequestRelease"));
    const dbus_int32_t requested_priority = DbusDeviceReservation::priority;
    if (call.message == nullptr ||
        dbus_message_append_args(call.message, DBUS_TYPE_INT32, &requested_priority,
                                 DBUS_TYPE_INVALID) == 0) {
      failure = "out of memory";
      return false;
    }
    ScopedError error;
    MessageRef reply(dbus_connection_send_with_reply_and_block(
        connection, call.message, DbusDeviceReservation::request_timeout_ms, error.get()));
    if (reply.message == nullptr) {
      failure = error.message();
      return false;
    }
    dbus_bool_t granted = FALSE;
    ScopedError parse;
    if (dbus_message_get_args(reply.message, parse.get(), DBUS_TYPE_BOOLEAN, &granted,
                              DBUS_TYPE_INVALID) == 0) {
      failure = parse.message();
      return false;
    }
    if (granted == FALSE)
      failure = "it refused to give the card up";
    return granted != FALSE;
  }

  void close() noexcept {
    if (dispatcher.joinable()) {
      stop.store(true, std::memory_order_release);
      dispatcher.join();
    }
    if (connection == nullptr)
      return;
    ScopedError error;
    (void)dbus_bus_release_name(connection, service.c_str(), error.get());
    if (path_registered)
      (void)dbus_connection_unregister_object_path(connection, path.c_str());
    path_registered = false;
    dbus_connection_flush(connection);
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    connection = nullptr;
  }
};

DbusDeviceReservation::DbusDeviceReservation(std::string application_name)
    : impl_(std::make_unique<Impl>()) {
  impl_->application_name = std::move(application_name);
}

DbusDeviceReservation::~DbusDeviceReservation() { release(); }

foundation::Result<DeviceReservation::Outcome> DbusDeviceReservation::acquire(int card) {
  using R = foundation::Result<Outcome>;
  if (card < 0)
    return R::failure({foundation::ErrorCode::invalid_argument, "Invalid ALSA card index"});
  if (impl_->connection != nullptr)
    return R::failure({foundation::ErrorCode::conflict, "A sound card is already reserved"});
  (void)dbus_threads_init_default();
  ScopedError error;
  impl_->connection = dbus_bus_get_private(DBUS_BUS_SESSION, error.get());
  if (impl_->connection == nullptr) {
    foundation::log::info(foundation::DiagnosticCategory::device,
                          "No session bus ({}); ALSA card {} opens without a reservation",
                          error.message(), card);
    return R::success(Outcome::unmanaged);
  }
  dbus_connection_set_exit_on_disconnect(impl_->connection, FALSE);
  impl_->service = service_prefix + std::to_string(card);
  impl_->path = path_prefix + std::to_string(card);
  impl_->device_name = "hw:" + std::to_string(card);
  const DBusObjectPathVTable table{nullptr, &Impl::on_message, nullptr, nullptr, nullptr, nullptr};
  impl_->path_registered = dbus_connection_register_object_path(
                               impl_->connection, impl_->path.c_str(), &table, impl_.get()) != 0;
  std::string failure;
  int owned = impl_->request_name(DBUS_NAME_FLAG_DO_NOT_QUEUE, failure);
  if (owned == DBUS_REQUEST_NAME_REPLY_EXISTS) {
    if (!impl_->request_release(failure)) {
      impl_->close();
      return R::failure({foundation::ErrorCode::conflict,
                         "The sound server keeps card " + std::to_string(card) + ": " + failure});
    }
    owned =
        impl_->request_name(DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_REPLACE_EXISTING, failure);
  }
  if (owned != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
      owned != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
    impl_->close();
    return R::failure(
        {foundation::ErrorCode::conflict, "Cannot reserve card " + std::to_string(card) +
                                              (failure.empty() ? std::string{} : ": " + failure)});
  }
  impl_->stop.store(false, std::memory_order_release);
  try {
    impl_->dispatcher = std::thread([impl = impl_.get()] {
      while (!impl->stop.load(std::memory_order_acquire) &&
             dbus_connection_read_write_dispatch(impl->connection, dispatch_timeout_ms) != 0) {
      }
    });
  } catch (const std::system_error&) {
    // Unanswered owner-side calls time out at the caller; the card stays ours.
    foundation::log::warning(foundation::DiagnosticCategory::device,
                             "Cannot start the reservation thread for card {}", card);
  }
  foundation::log::info(foundation::DiagnosticCategory::device, "Reserved ALSA card {} ({})", card,
                        impl_->service);
  return R::success(Outcome::reserved);
}

void DbusDeviceReservation::release() noexcept {
  if (impl_->connection == nullptr)
    return;
  impl_->close();
  foundation::log::info(foundation::DiagnosticCategory::device, "Released {}", impl_->service);
}

} // namespace oscillate::audio_io
