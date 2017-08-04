#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <string_view>
#include <vector>

#include <android-base/macros.h>

#include <libadb2/socket.h>

#include "packet.h"
#include "provider.h"
#include "util/uv.h"

// ADB protocol version.
#define A_VERSION 0x01000000

namespace adb {

constexpr size_t MAX_PAYLOAD_V1 = 4 * 1024;
constexpr size_t MAX_PAYLOAD_V2 = 256 * 1024;
constexpr size_t MAX_PAYLOAD = MAX_PAYLOAD_V2;

constexpr size_t LINUX_MAX_SOCKET_SIZE = 4194304;

// A ProtocolInterface is the object responsible for handling the specified protocol.
class ProtocolInterface {
 public:
  explicit ProtocolInterface(LocalDeviceInterface& device);
  virtual ~ProtocolInterface();

  virtual void HandleInput(std::deque<char>& input_buffer) REQUIRES(loop_) = 0;

  virtual void OpenShell(std::promise<std::unique_ptr<ShellInterface>>, ShellOptions) = 0;
  virtual std::future<std::unique_ptr<FileSyncInterface>> OpenFileSync() = 0;

 protected:
  LocalDeviceInterface& device_;
  RunLoop& loop_;
};

class AdbPacketProtocol : public ProtocolInterface {
 public:
  explicit AdbPacketProtocol(LocalDeviceInterface& device);

  virtual void HandleInput(std::deque<char>& input_buffer) override final REQUIRES(loop_);
  virtual void HandlePacket(Packet& packet) REQUIRES(loop_) = 0;

  virtual void OpenShell(std::promise<std::unique_ptr<ShellInterface>>, ShellOptions) override {
    LOG(FATAL) << "AdbPacketProtocol: OpenShell unimplemented";
    abort();
  }

  virtual std::future<std::unique_ptr<FileSyncInterface>> OpenFileSync() override {
    LOG(FATAL) << "AdbPacketProtocol: OpenFileSync unimplemented";
    abort();
  }
};

// Protocol implementation responsible for initial handshake and handoff to a real implementation.
class InitialProtocol : public AdbPacketProtocol {
 public:
  explicit InitialProtocol(LocalDeviceInterface& device);

  virtual void HandlePacket(Packet& packet) override final;
};

namespace protocol_v1 {

class AuthProtocol : public AdbPacketProtocol {
 public:
  explicit AuthProtocol(LocalDeviceInterface& device);
  virtual void HandlePacket(Packet& packet) override final;
};

using SocketId = uint32_t;

class LocalSocket {
 public:
  using ReadCallback = std::function<void(const char*, size_t)>;
  using WriteCallback = std::function<void()>;

  LocalSocket(LocalDeviceInterface& device, unique_socket socket, SocketId local_id,
              SocketId adbd_id);

  void Write(std::vector<char> buf) REQUIRES(device_.loop_);
  void Close() REQUIRES(device_.loop_);

 private:
  void ReportClose();

  LocalDeviceInterface& device_;
  SocketId local_id_;
  SocketId adbd_id_;

  StreamHandle stream_;
};

class Protocol : public AdbPacketProtocol {
 public:
  explicit Protocol(LocalDeviceInterface& device);
  virtual void HandlePacket(Packet& packet) override final;
  void OpenSocket(const std::string& name, std::function<void(unique_socket)>);

  virtual void OpenShell(std::promise<std::unique_ptr<ShellInterface>>, ShellOptions) override final;

 private:
  std::atomic<SocketId> local_socket_id_;
  std::unordered_map<SocketId, std::unique_ptr<LocalSocket>> local_sockets_;
  std::unordered_map<SocketId, std::function<void(unique_socket)>> opening_sockets_;
};

}  // namespace protocol_v1
}  // namespace adb
