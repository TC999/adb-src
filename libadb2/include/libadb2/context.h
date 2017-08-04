#pragma once

#include <future>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "types.h"

namespace adb {

// An abstract interface for either a connection to an adb server (RemoteContext) or the
// implementation of the adb server itself (Context).
class ContextInterface {
 public:
  virtual ~ContextInterface() {
  }

  virtual std::unordered_map<DeviceId, std::shared_ptr<DeviceInterface>> GetDevices() = 0;
  virtual std::shared_ptr<DeviceInterface> GetDevice(DeviceId id) = 0;

  virtual std::future<std::shared_ptr<DeviceInterface>> ConnectDevice(std::string_view hostname,
                                                                      int port) = 0;
};

// Abstract interface for a local Context.
class LocalContextInterface : public ContextInterface {
 public:
  virtual ~LocalContextInterface() {
  }

  // Acquire the next available DeviceId.
  virtual DeviceId AcquireDeviceId() = 0;
};

// A local implementation of ContextInterface.
class Context : public LocalContextInterface {
 public:
  Context();
  virtual ~Context();
  virtual std::unordered_map<DeviceId, std::shared_ptr<DeviceInterface>> GetDevices() override;
  virtual std::shared_ptr<DeviceInterface> GetDevice(DeviceId id) override;

  virtual std::future<std::shared_ptr<DeviceInterface>> ConnectDevice(std::string_view hostname,
                                                                      int port) override;

  virtual DeviceId AcquireDeviceId() override final;

 private:
  struct Impl;
  std::unique_ptr<Context::Impl> impl_;
};

}
