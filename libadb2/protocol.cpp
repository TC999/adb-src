#include "protocol.h"

#include <deque>
#include <future>
#include <memory>
#include <string_view>

#include <android-base/logging.h>

#include <libadb2/event.h>
#include <libadb2/socket.h>

namespace adb {

static void SendPacket(LocalDeviceInterface& device, uint32_t command, uint32_t arg0, uint32_t arg1,
                       std::vector<char> data = {}) REQUIRES(device.loop_) {
  Packet packet;
  packet.header.command = command;
  packet.header.arg0 = arg0;
  packet.header.arg1 = arg1;
  packet.data = std::move(data);
  device.Send(std::move(packet));
}

ProtocolInterface::ProtocolInterface(LocalDeviceInterface& device)
    : device_(device), loop_(device_.loop_) {}

ProtocolInterface::~ProtocolInterface() {}

AdbPacketProtocol::AdbPacketProtocol(LocalDeviceInterface& device) : ProtocolInterface(device) {}

void AdbPacketProtocol::HandleInput(std::deque<char>& input_buffer) {
  device_.loop_.AssertOnMainThread();

  while (!input_buffer.empty()) {
    size_t len = input_buffer.size();
    LOG(VERBOSE) << "AdbPacketProtocol: handling " << len << " bytes of input";
    if (len < sizeof(PacketHeader)) {
      LOG(VERBOSE) << "AdbPacketProtocol: not enough data for a header";
      return;
    }

    PacketHeader header;
    auto header_begin = input_buffer.begin();
    auto header_end = header_begin + sizeof(header);
    std::copy(header_begin, header_end, reinterpret_cast<char*>(&header));
    len -= sizeof(header);

    if (header.command != ~header.magic) {
      LOG(ERROR) << "packet header failed verification";
      device_.Close();
      return;
    }

    if (len < header.data_length) {
      LOG(VERBOSE) << "need more data for packet, have " << len << ", need " << header.data_length;
      return;
    }

    auto data_end = header_end + header.data_length;

    Packet packet;
    packet.header = header;
    packet.data.resize(header.data_length);
    packet.data.assign(header_end, data_end);

    if (!packet.ValidateHeader()) {
      // TODO: Should the protocol implementation handle this instead?
      LOG(ERROR) << "packet checksum failed verification";
      device_.Close();
      return;
    }

    input_buffer.erase(header_begin, data_end);

    // We explicitly don't check the packet checksum here.
    HandlePacket(packet);
  }
}

InitialProtocol::InitialProtocol(LocalDeviceInterface& device) : AdbPacketProtocol(device) {}

void InitialProtocol::HandlePacket(Packet& packet) {
  // Dispatch to the appropriate protocol
  LOG(VERBOSE) << "received packet: " << packet;

  device_.loop_.AssertOnMainThread();

  switch (packet.header.command) {
    case A_CNXN: {
      // Parse the banner, and then hand it off.
      // TODO: Actually parse the banner.
      std::shared_ptr<ProtocolInterface> next = std::make_shared<protocol_v1::Protocol>(device_);
      std::shared_ptr<ProtocolInterface> self = device_.SetProtocol(next);
      device_.SetState(kDeviceStateOnline);
      return;
    }

    case A_AUTH: {
      std::shared_ptr<ProtocolInterface> next = std::make_shared<protocol_v1::AuthProtocol>(device_);
      std::shared_ptr<ProtocolInterface> self = device_.SetProtocol(next);
      std::static_pointer_cast<AdbPacketProtocol>(next)->HandlePacket(packet);
      return;
    }

    default: {
      LOG(ERROR) << "received unexpected initial packet: " << packet;
      device_.Close();
      return;
    }
  }
}

namespace protocol_v1 {

AuthProtocol::AuthProtocol(LocalDeviceInterface& device) : AdbPacketProtocol(device) {
  LOG(FATAL) << "AuthProtocol unimplemented";
}

void AuthProtocol::HandlePacket(Packet& packet) {
  LOG(FATAL) << "AuthProtocol unimplemented";
}

LocalSocket::LocalSocket(LocalDeviceInterface& device, unique_socket socket, SocketId local_id,
                         SocketId adbd_id)
    : device_(device),
      local_id_(local_id),
      adbd_id_(adbd_id),
      stream_(device.loop_, std::move(socket)) {
  auto read_callback = [this](ssize_t nread, const char* buf) {
    device_.loop_.AssertOnMainThread();
    if (nread == UV_EOF) {
      LOG(INFO) << "LocalSocket hit EOF";
      this->ReportClose();
    } else if (nread < 0) {
      LOG(ERROR) << "LocalSocket read failed: " << uv_strerror(nread);
    } else {
      SendPacket(device_, A_WRTE, local_id_, adbd_id_, std::vector<char>(buf, buf + nread));
    }
  };
  stream_.BeginRead(read_callback);
}

void LocalSocket::Write(std::vector<char> buf) {
  LOG(DEBUG) << "LocalSocket::Write: " << buf.size() << " bytes";
  stream_.loop_.AssertOnMainThread();
  stream_.Write(std::move(buf), nullptr);
}

void LocalSocket::Close() {
  ReportClose();
  stream_.loop_.AssertOnMainThread();
  stream_.Close();
}

void LocalSocket::ReportClose() {
  SendPacket(device_, A_CLSE, local_id_, adbd_id_);
}

Protocol::Protocol(LocalDeviceInterface& device) : AdbPacketProtocol(device), local_socket_id_(1) {}

void Protocol::HandlePacket(Packet& packet) {
  LOG(VERBOSE) << "Protocol::HandlePacket: " << packet;
  switch (packet.header.command) {
    case A_OKAY: {
      SocketId adbd_id = packet.header.arg0;
      SocketId local_id = packet.header.arg1;
      auto opening_it = opening_sockets_.find(local_id);
      if (opening_it != opening_sockets_.end()) {
        unique_socket local_fd, client_fd;
        if (!Socketpair(&local_fd, &client_fd)) {
          PLOG(FATAL) << "failed to create socketpair";
        } else {
          LOG(VERBOSE) << "created Socketpair [" << local_fd.get() << ", " << client_fd.get()
                       << "] for LocalSocket " << local_id;
        }

        auto local_socket =
            std::make_unique<LocalSocket>(device_, std::move(local_fd), local_id, adbd_id);
        local_sockets_.emplace(local_id, std::move(local_socket));
        opening_it->second(std::move(client_fd));
        opening_sockets_.erase(opening_it);
        LOG(INFO) << "successfully opened LocalSocket(id = " << local_id
                  << ", adbd_id = " << adbd_id << ")";
        break;
      }

      auto it = local_sockets_.find(local_id);
      if (it == local_sockets_.end()) {
        LOG(ERROR) << "device reported OKAY for nonexistent socket";
        SendPacket(device_, A_CLSE, local_id, adbd_id);
        break;
      }

      // TODO: Backpressure.
      break;
    }

    case A_WRTE: {
      SocketId adbd_id = packet.header.arg0;
      SocketId local_id = packet.header.arg1;
      auto it = local_sockets_.find(local_id);
      if (it == local_sockets_.end()) {
        auto opening_it = opening_sockets_.find(local_id);
        if (opening_it == opening_sockets_.end()) {
          LOG(ERROR) << "device sent data to closed socket";
        } else {
          LOG(ERROR) << "device sent data to opening socket";
        }
        SendPacket(device_, A_CLSE, local_id, adbd_id);
        break;
      } else {
        it->second->Write(std::move(packet.data));
        SendPacket(device_, A_OKAY, local_id, adbd_id);
        break;
      }
    }

    case A_CLSE: {
      SocketId adbd_id = packet.header.arg0;
      SocketId local_id = packet.header.arg1;
      auto it = local_sockets_.find(local_id);
      if (it == local_sockets_.end()) {
        auto opening_it = opening_sockets_.find(local_id);
        if (opening_it != opening_sockets_.end()) {
          opening_it->second(unique_socket());
          opening_sockets_.erase(opening_it);
        }
      } else {
        it->second->Close();
      }
      SendPacket(device_, A_CLSE, local_id, adbd_id);
      break;
    }

    default:
      LOG(ERROR) << "Protocol received unhandled packet: " << packet;
      device_.Close();
      return;
  };
}

void Protocol::OpenSocket(const std::string& name, std::function<void(unique_socket)> callback) {
  uint32_t id = local_socket_id_++;
  this->opening_sockets_.emplace(id, std::move(callback));

  // adbd expects the name to be null terminated for some reason.
  // str.begin(), str.end() + 1 is technically UB.
  std::vector<char> data(name.data(), name.data() + name.size() + 1);
  SendPacket(device_, A_OPEN, id, 0, std::move(data));
}

class LegacyShellInterface : public ShellInterface {
 public:
  LegacyShellInterface(RunLoop& loop, unique_socket socket, ShellIO io)
      : socket_(loop, std::move(socket)),
        in_(loop, std::move(io.in)),
        out_(loop, std::move(io.out)),
        return_promise(std::move(io.return_value)) {
    LOG(INFO) << "shell interface constructing";
    // The legacy shell protocol doesn't differentiate between stdout and stderr.
    socket_.BeginRead([this](ssize_t nread, const char* buf) {
      if (nread < 0) {
        LOG(INFO) << "read from shell socket failed: " << uv_strerror(nread);
        Close(0);
      } else {
        out_.Write(std::vector<char>(buf, buf + nread), nullptr);
      }
    });

    in_.BeginRead([this](ssize_t nread, const char* buf) {
      socket_.loop_.AssertOnMainThread();
      if (nread < 0) {
        LOG(INFO) << "read from stdin failed: " << uv_strerror(nread);
        Close(0);
      } else {
        socket_.Write(std::vector<char>(buf, buf + nread), nullptr);
      }
    });

    LOG(INFO) << "shell interface constructed";
  }

  void Close(int rc) {
    socket_.Close();
    in_.Close();
    out_.Close();
    return_promise.set_value(rc);
  }

  virtual void SetTerminalSize(int cols, int rows) override final {
    LOG(DEBUG) << "AdbShellInterface: SetTerminalSize(" << cols << ", " << rows << ")";
  }

 private:
  StreamHandle socket_;
  StreamHandle in_;
  StreamHandle out_;
  std::promise<int> return_promise;
};

struct ShellArgs {};

void Protocol::OpenShell(std::promise<std::unique_ptr<ShellInterface>> promise,
                         ShellOptions options) {
  // std::function must be copyable, so stash the promise and ShellIO in shared_ptrs.
  std::shared_ptr<ShellIO> io = std::make_shared<ShellIO>(std::move(options.io));
  auto shared_promise =
      std::make_shared<std::promise<std::unique_ptr<ShellInterface>>>(std::move(promise));
  auto callback = [this, shared_promise, io](unique_socket s) {
    LOG(INFO) << "shell callback called";
    auto shell_interface =
        std::make_unique<LegacyShellInterface>(this->device_.loop_, std::move(s), std::move(*io));
    shared_promise->set_value(std::move(shell_interface));
    LOG(INFO) << "shell callback done";
  };
  OpenSocket("shell:" + options.command, callback);
}

}  // namespace protocol_v1
}  // namespace adb
