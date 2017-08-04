#pragma once

#include <future>
#include <memory>
#include <string>

#include <android-base/thread_annotations.h>

#include "socket.h"

// clang warns for capability names that aren't 'mutex' or 'role'.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-attributes"
class CAPABILITY("run loop") RunLoop {
#pragma clang diagnostic pop
 public:
  explicit RunLoop(std::string name);
  ~RunLoop();

  RunLoop(const RunLoop& copy) = delete;
  RunLoop(RunLoop&& move) = delete;

  template<typename Fn>
  auto Run(Fn fn) -> std::future<decltype(fn())> {
    using T = decltype(fn());
    auto promise = std::make_shared<std::promise<T>>();
    auto wrapper = [promise, fn]() {
      if constexpr (std::is_same<void, T>::value) {
        fn();
        promise->set_value();
      } else {
        promise->set_value(fn());
      }
    };

    if (!RunFunction(wrapper)) {
      promise->set_exception(
          std::make_exception_ptr(std::runtime_error("failed to enqueue function")));
    }

    return promise->get_future();
  }

  bool IsOnMainThread() const;
  void AssertOnMainThread() const ASSERT_CAPABILITY(this);

 private:
  bool RunFunction(std::function<void()> fn);

 public:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class StreamHandle final {
 public:
  StreamHandle(RunLoop& loop, unique_socket fd) REQUIRES(loop);
  ~StreamHandle() REQUIRES(loop_);

  void Shutdown() REQUIRES(loop_);
  void Close() REQUIRES(loop_);

  // Callback takes a read result which is either a libuv errno (TODO: translate back to system?)
  // or the number of bytes read.
  void BeginRead(std::function<void(ssize_t rc, const char* buf)> callback) REQUIRES(loop_);
  void StopRead() REQUIRES(loop_);

  bool Write(std::vector<char> buf, std::function<void(std::vector<char>&, int)> write_callback)
      REQUIRES(loop_);

  RunLoop& loop_;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};
