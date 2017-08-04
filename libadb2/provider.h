#pragma once

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <android-base/thread_annotations.h>
#include <uv.h>

#include <libadb2/context.h>
#include <libadb2/device.h>
#include <libadb2/event.h>
#include <libadb2/types.h>

#include "packet.h"

namespace adb {

struct ConnectRequest {
  std::string hostname;
  int port;
  std::promise<std::shared_ptr<DeviceInterface>> promise;
};

struct KillRequest {};

class ProviderInterface {
 public:
  ProviderInterface();
  virtual ~ProviderInterface();

  ProviderInterface(const ProviderInterface& copy) = delete;
  ProviderInterface(ProviderInterface&& move) = delete;
};

class LocalDeviceInterface : public DeviceInterface {
 public:
  LocalDeviceInterface(RunLoop& loop);

  virtual DeviceState GetState() override final;
  virtual DeviceState SetState(DeviceState new_state) final;
  virtual DeviceState WaitForStateChange(DeviceState original_state) override final;

  virtual void Close() REQUIRES(loop_) = 0;

  // Returns the previous Protocol.
  std::shared_ptr<ProtocolInterface> SetProtocol(std::shared_ptr<ProtocolInterface> protocol);

  // Enqueue data to be sent to the device.
  virtual void Send(std::vector<char> data) REQUIRES(loop_) = 0;

  // Enqueue an adb_v1 packet to be sent to the device.
  // The default implementation will call Send(std::vector<char>) twice.
  virtual void Send(Packet packet) REQUIRES(loop_);

  RunLoop& loop_;

 protected:
  std::shared_ptr<ProtocolInterface> protocol_;

 private:
  DeviceState state_ GUARDED_BY(state_mutex_) = kDeviceStateConnecting;
  std::mutex state_mutex_;
  std::condition_variable state_cv_;
};

class TcpDevice;
class TcpProvider : public ProviderInterface {
 public:
  explicit TcpProvider(LocalContextInterface* context);
  virtual ~TcpProvider();

  TcpProvider(const TcpProvider& copy) = delete;
  TcpProvider(TcpProvider&& move) = delete;

  void Connect(std::shared_ptr<ConnectRequest> req);

 private:
  void HandleConnect(std::shared_ptr<ConnectRequest> req) REQUIRES(loop_);

  LocalContextInterface* context_;

 public:
  RunLoop loop_;

 private:
  std::unordered_map<DeviceId, std::shared_ptr<TcpDevice>> devices_ GUARDED_BY(loop_);
};

class UsbProvider : public ProviderInterface {};
}
