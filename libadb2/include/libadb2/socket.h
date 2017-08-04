#pragma once

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

class unique_socket {
 public:
  explicit unique_socket(int sock) : sock_(sock) {}
  unique_socket() : sock_(-1) {}
  unique_socket(const unique_socket& copy) = delete;
  unique_socket(unique_socket&& move) : sock_(move.release()) {}

  ~unique_socket() {
    if (sock_ != -1) {
      close(sock_);
    }
  }

  unique_socket& operator=(unique_socket&& move) {
    reset(move.release());
    return *this;
  }

  bool operator==(int rhs) { return get() == rhs; }

  bool operator!=(int rhs) { return !(*this == rhs); }

  int get() { return sock_; }
  int release() {
    int sock = sock_;
    sock_ = -1;
    return sock;
  }

  void reset(int new_socket = -1) {
    release();
    sock_ = new_socket;
  }

 private:
  int sock_;
};

namespace adb {
bool Socketpair(unique_socket* left, unique_socket* right);
}
