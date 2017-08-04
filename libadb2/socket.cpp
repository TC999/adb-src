#include <libadb2/socket.h>

#include <sys/socket.h>

namespace adb {
bool Socketpair(unique_socket* left, unique_socket* right) {
  // TODO(win32): Implement.
  int sockfds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfds) != 0) {
    return false;
  }

  left->reset(sockfds[0]);
  right->reset(sockfds[1]);
  return true;
}
}  // namespace adb
