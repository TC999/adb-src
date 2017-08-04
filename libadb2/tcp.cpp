#include "provider.h"

#include <deque>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/stringprintf.h>
#include <uv.h>

#include <libadb2/context.h>
#include <libadb2/socket.h>
#include "protocol.h"

using namespace std::string_literals;

namespace adb {

static std::string address_to_string(struct sockaddr* sa) {
  char buf[INET6_ADDRSTRLEN];
  void* addr;
  if (sa->sa_family == AF_INET) {
    addr = &reinterpret_cast<struct sockaddr_in*>(sa)->sin_addr;
  } else if (sa->sa_family == AF_INET6) {
    addr = &reinterpret_cast<struct sockaddr_in6*>(sa)->sin6_addr;
  } else {
    LOG(FATAL) << "unknown sa_family: " << sa->sa_family;
  }

  int rc = uv_inet_ntop(sa->sa_family, addr, buf, sizeof(buf));
  if (rc != 0) {
    LOG(FATAL) << "inet_ntop failed: " << uv_strerror(rc);
  }
  return buf;
}

enum TcpDeviceState {
  kTcpDeviceStateConnecting,
  kTcpDeviceStateConnected,
  kTcpDeviceStateDisconnected,
};

struct ShellRequest {
  std::promise<std::unique_ptr<ShellInterface>> promise;
  ShellOptions options;
};

class TcpDevice : public LocalDeviceInterface {
 public:
  TcpDevice(TcpProvider* provider, unique_socket socket, DeviceId id, std::string_view address)
      : LocalDeviceInterface(provider->loop_),
        provider_(provider),
        socket_(loop_, std::move(socket)),
        id_(id),
        serial_(address),
        address_(address) {
    // Say hello, and start reading responses.
    SetProtocol(std::make_shared<InitialProtocol>(*this));

    PacketHeader hello;
    hello.command = A_CNXN;
    hello.arg0 = A_VERSION;
    hello.arg1 = MAX_PAYLOAD;
    hello.Update(0, 0);
    Send(&hello, sizeof(hello));

    // Start reading for a reply.
    auto read_cb = [this](ssize_t nread, const char* buf) {
      this->loop_.AssertOnMainThread();
      this->HandleRead(nread, buf);
    };
    socket_.BeginRead(read_cb);

    LOG(INFO) << *this << ": constructed for " << address;
  }

  ~TcpDevice() { LOG(INFO) << *this << ": destructing"; }

  TcpDevice(const TcpDevice& copy) = delete;
  TcpDevice(TcpDevice&& move) = delete;

  virtual DeviceId GetId() override final { return id_; }

  virtual std::string GetSerial() override final {
    // TODO: Make this actually return the serial?
    return address_;
  }

  virtual std::unordered_set<std::string> GetFeatureSet() override final {
    LOG(FATAL) << "TcpDevice::GetFeatureSet unimplemented";
    return {};
  }

  virtual std::future<std::unique_ptr<ShellInterface>> OpenShell(ShellOptions options) override final {
    std::promise<std::unique_ptr<ShellInterface>> promise;
    ShellRequest req{
        .promise = std::move(promise), .options = std::move(options),
    };

    auto shared_req = std::make_shared<ShellRequest>(std::move(req));
    auto future = shared_req->promise.get_future();
    loop_.Run([this, shared_req]() {
      protocol_->OpenShell(std::move(shared_req->promise), std::move(shared_req->options));
    });
    return future;
  }

  virtual std::future<std::unique_ptr<FileSyncInterface>> OpenFileSync() override final {
    return protocol_->OpenFileSync();
    ;
  }

  virtual std::string ToString() const override final {
    return "TcpDevice("s + std::to_string(id_) + ")";
  }

  virtual void Close() override final { LOG(INFO) << *this << ": closing"; }

  using LocalDeviceInterface::Send;

  void Send(const void* data, size_t len) REQUIRES(loop_) {
    const char* p = reinterpret_cast<const char*>(data);
    std::vector<char> buf(p, p + len);
    Send(std::move(buf));
  }

  virtual void Send(std::vector<char> buf) override final REQUIRES(loop_) {
    auto callback = [this](std::vector<char>& data, int status) {
      if (status != 0) {
        LOG(ERROR) << *this << ": write failed: " << uv_strerror(status);
      } else {
        LOG(VERBOSE) << *this << ": write of " << data.size() << " bytes succeeded";
      }
    };

    size_t len = buf.size();
    socket_.loop_.AssertOnMainThread();
    if (!socket_.Write(std::move(buf), callback)) {
      Close();
    } else {
      LOG(VERBOSE) << *this << ": enqueued " << len << " byte write";
    }
  }

  void HandleRead(ssize_t nread, const char* buf) REQUIRES(loop_) {
    if (nread == UV_EOF) {
      LOG(INFO) << *this << ": hit EOF while reading";
      Close();
      return;
    }

    LOG(VERBOSE) << *this << ": read " << nread << " byte" << (nread == 1 ? "" : "s");
    received_.insert(received_.end(), buf, buf + nread);
    protocol_->HandleInput(received_);
  }

  TcpProvider* provider_;

  StreamHandle socket_;

  const DeviceId id_;
  std::string serial_;
  const std::string address_;

  TcpDeviceState state_ = kTcpDeviceStateConnecting;

  std::deque<char> received_;  // but unparsed...
};

TcpProvider::TcpProvider(LocalContextInterface* context)
    : context_(context), loop_("TCPProvider") {}

TcpProvider::~TcpProvider() {
  LOG(INFO) << "TcpProvider: destructing";
  auto close_fn = [this]() {
    this->loop_.AssertOnMainThread();
    // Close all of our sockets.
    this->devices_.clear();
  };
  loop_.Run(close_fn).wait();
}

// TODO: Make this variadic and pass through to StringAppendV.
static void ReportConnectFailure(ConnectRequest* req, const std::string& reason) {
  LOG(ERROR) << "TcpProvider: " << reason;
  req->promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
}

void TcpProvider::Connect(std::shared_ptr<ConnectRequest> req) {
  LOG(INFO) << "TcpProvider: handling Connect(" << req->hostname << ":" << req->port << ")";

  loop_.Run([this, req]() {
    this->loop_.AssertOnMainThread();
    HandleConnect(req);
  });
}

void TcpProvider::HandleConnect(std::shared_ptr<ConnectRequest> req) {
  // Double check to make sure we haven't already connected to this address.
  // TODO: Add a second map for this, instead of iterating?
  std::string address = req->hostname + ":" + std::to_string(req->port);
  for (auto& [_, device] : devices_) {
    if (device->address_ == address) {
      std::string msg = "already connected to " + address;
      LOG(ERROR) << "TcpProvider: " << msg;
      req->promise.set_exception(std::make_exception_ptr(std::runtime_error(msg)));
      return;
    }
  }

  // TODO: Resolve asynchronously?
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG | AI_NUMERICSERV;
  struct addrinfo* res;
  int rc = getaddrinfo(req->hostname.c_str(), std::to_string(req->port).c_str(), &hints, &res);
  if (rc != 0) {
    ReportConnectFailure(
        req.get(), android::base::StringPrintf("failed to resolve '%s': %s", req->hostname.c_str(),
                                               gai_strerror(rc)));
    return;
  }

  std::string resolved_addr = address_to_string(res->ai_addr);
  LOG(INFO) << "TcpProvider: resolved " << req->hostname << " to " << resolved_addr;

  // TODO: Connect asynchronously? Cancelling a uv_tcp_connect is somewhat difficult.
  struct addrinfo* cur = res;
  unique_socket sock;
  while (cur) {
    sock.reset(socket(cur->ai_family, SOCK_STREAM, IPPROTO_TCP));
    if (sock == -1) {
      // TODO(win32): WSAGetLastError?
      ReportConnectFailure(req.get(), "failed to create socket: "s + strerror(errno));
      goto exit;
    }

    rc = connect(sock.get(), cur->ai_addr, cur->ai_addrlen);
    if (rc == 0) {
      DeviceId device_id = context_->AcquireDeviceId();
      auto device = std::make_shared<TcpDevice>(this, std::move(sock), device_id, address);
      devices_[device_id] = device;

      // The original adb immediately reported success after `adb connect foo:1234`.
      req->promise.set_value(device);
      goto exit;
    }

    cur = cur->ai_next;
  }

  ReportConnectFailure(req.get(), "failed to connect");

exit:
  freeaddrinfo(res);
}

}  // namespace adb
