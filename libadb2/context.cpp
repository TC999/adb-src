#include <libadb2/context.h>

#include <stdlib.h>
#include <unistd.h>

#include <exception>
#include <stdexcept>

#include <android-base/logging.h>
#include <uv.h>

#include "provider.h"
#include <libadb2/event.h>

namespace adb {

struct Context::Impl : public LocalContextInterface {
  Impl() : loop_("adb context"), tcp_provider_(new TcpProvider(this)), next_device_id_(1) {}

  ~Impl() {
    auto close_fn = [this]() {
      loop_.AssertOnMainThread();

      // Destruct our providers, to close all of their connections.
      this->tcp_provider_ = nullptr;
      this->usb_provider_ = nullptr;
    };
    loop_.Run(close_fn).wait();
  }

  Impl(const Impl& copy) = delete;
  Impl(Impl&& move) = delete;

  virtual std::unordered_map<DeviceId, std::shared_ptr<DeviceInterface>> GetDevices() override final {
    LOG(FATAL) << "GetDevices unimplemented";
    abort();
  }

  virtual std::shared_ptr<DeviceInterface> GetDevice(DeviceId id) override final {
    LOG(FATAL) << "GetDevice unimplemented";
    abort();
  }

  std::future<std::shared_ptr<DeviceInterface>> ConnectDevice(std::string_view hostname,
                                                              int port) override final {
    auto arg = std::make_shared<ConnectRequest>();
    arg->hostname = hostname;
    arg->port = port;

    auto future = arg->promise.get_future();
    auto connect = [this, arg]() {
      loop_.AssertOnMainThread();

      if (this->tcp_provider_) {
        this->tcp_provider_->Connect(arg);
      } else {
        arg->promise.set_exception(std::make_exception_ptr(
            std::logic_error("attempted to connect without a TCP provider")));
      }
    };

    loop_.Run(connect);
    return future;
  }

  virtual DeviceId AcquireDeviceId() override final { return next_device_id_++; }

  RunLoop loop_;

  std::unique_ptr<TcpProvider> tcp_provider_ GUARDED_BY(loop_) PT_GUARDED_BY(loop_);
  std::unique_ptr<UsbProvider> usb_provider_ GUARDED_BY(loop_) PT_GUARDED_BY(loop_);

  std::atomic<DeviceId> next_device_id_;
};

Context::Context() : impl_(new Context::Impl()) {}

Context::~Context() {}

std::unordered_map<DeviceId, std::shared_ptr<DeviceInterface>> Context::GetDevices() {
  return impl_->GetDevices();
}

std::shared_ptr<DeviceInterface> Context::GetDevice(DeviceId id) {
  return impl_->GetDevice(id);
}

std::future<std::shared_ptr<DeviceInterface>> Context::ConnectDevice(std::string_view hostname,
                                                                     int port) {
  return impl_->ConnectDevice(hostname, port);
}

DeviceId Context::AcquireDeviceId() {
  return impl_->AcquireDeviceId();
}
}
