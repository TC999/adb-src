#include "util/uv.h"

#include <variant>

#include <android-base/logging.h>

std::variant<UvPipe, UvTcp, UvTty> UvFromFd(uv_loop_t* loop, int fd) {
  uv_handle_type type = uv_guess_handle(fd);
  switch (type) {
    case UV_TTY:
      return UvTty(loop, unique_socket(fd));
    case UV_NAMED_PIPE:
      return UvPipe(loop, unique_socket(fd));
    case UV_TCP:
      return UvTcp(loop, unique_socket(fd));
    case UV_FILE:
      LOG(FATAL) << "file redirection is currently unsupported";
      abort();
    default:
      LOG(FATAL) << "unhandled UV handle type: " << type;
      abort();
  }
}

std::variant<UvPipe, UvTcp, UvTty> UvFromFd(uv_loop_t* loop, unique_socket fd) {
  return UvFromFd(loop, fd.release());
}
