#pragma once

#include <future>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <libadb2/types.h>
#include <libadb2/socket.h>

namespace adb {

enum DeviceState {
  kDeviceStateConnecting,
  kDeviceStateUnauthorized,
  kDeviceStateOnline,
  kDeviceStateOffline,
};

std::ostream& operator<<(std::ostream& os, DeviceState state);

struct ShellIO {
  unique_socket in;
  unique_socket out;
  unique_socket err;

  std::promise<int> return_value;
};

struct ShellOptions {
  ShellIO io;

  std::string command;
};

class DeviceInterface {
 public:
  virtual ~DeviceInterface() {
  }

  virtual DeviceId GetId() = 0;
  virtual std::string GetSerial() = 0;
  virtual std::unordered_set<std::string> GetFeatureSet() = 0;

  virtual DeviceState GetState() = 0;
  virtual DeviceState WaitForStateChange(DeviceState original_state) = 0;

  virtual std::future<std::unique_ptr<ShellInterface>> OpenShell(ShellOptions options) = 0;
  virtual std::future<std::unique_ptr<FileSyncInterface>> OpenFileSync() = 0;

  virtual std::string ToString() const = 0;
};

std::ostream& operator<<(std::ostream& os, const DeviceInterface& device);

class ShellInterface {
 public:
  virtual void SetTerminalSize(int cols, int rows) = 0;
};

class FileSyncInterface {
 public:
  virtual std::vector<char> Pull(const std::string_view path) = 0;
};

}  // namespace adb
