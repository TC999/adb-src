#include <libadb2/event.h>

#include <pthread.h>
#include <stdio.h>

#include <chrono>
#include <future>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <android-base/logging.h>
#include <uv.h>

#include "util/uv.h"

using namespace std::chrono_literals;

struct RunLoop::Impl {
  Impl(std::string name) : name_(std::move(name)) {
    int rc = uv_loop_init(&uv_loop_);
    CHECK_EQ(0, rc) << "failed to initialize uv_loop_t";

    rc = uv_async_init(&uv_loop_, &uv_async_, uv_async_callback);
    CHECK_EQ(0, rc) << "failed to initialize uv_async_t";
    uv_async_.data = this;

    thread_ = std::thread([this] { ThreadMain(); });

    // Wait until the thread actually starts.
    while (running_mutex_.try_lock()) {
      running_mutex_.unlock();
      std::this_thread::yield();
    }
  }

  ~Impl() {
    CHECK_NE(std::this_thread::get_id(), thread_.get_id())
        << "destructed RunLoop on its own thread";

    bool result = RunFunction([this]() {
      uv_close(reinterpret_cast<uv_handle_t*>(&uv_async_), nullptr);

      std::lock_guard<std::mutex> lock(queue_mutex_);
      terminating_ = true;
    });

    CHECK(result);

    // Wait for the thread to terminate.
    if (!running_mutex_.try_lock_for(1s)) {
      LOG(ERROR) << "RunLoop thread failed to terminate, dumping uv_loop state";
      uv_print_all_handles(&uv_loop_, stderr);
      LOG(FATAL) << "terminating";
    }

    thread_.join();
    CHECK(queue_.empty());

    CHECK_EQ(0, uv_loop_close(&uv_loop_));
  }

  bool IsOnMainThread() const { return std::this_thread::get_id() == thread_.get_id(); }

  bool RunFunction(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (terminating_) {
      return false;
    }
    queue_.emplace_back(std::move(fn));
    uv_async_send(&uv_async_);
    return true;
  }

  uv_loop_t* Loop() { return &uv_loop_; }

  void ThreadMain() {
    char buf[16] = {};
    strncpy(buf, name_.data(), std::min(sizeof(buf) - 1, name_.size()));
    pthread_setname_np(pthread_self(), buf);

    std::lock_guard<std::timed_mutex> running_lock(running_mutex_);
    LOG(INFO) << "run loop '" << name_ << "' initialized";
    uv_run(&uv_loop_, UV_RUN_DEFAULT);
    LOG(INFO) << "run loop '" << name_ << "' terminating";

    // Flush the run queue at the end of the thread.
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto&& fn : queue_) {
      fn();
    }
    queue_.clear();

    // Count the number of handles left before we go.
    int handle_count = 0;
    uv_walk(&uv_loop_, [](uv_handle_t*, void* p) { ++*static_cast<int*>(p); }, &handle_count);
    if (handle_count != 0) {
      LOG(ERROR) << "run loop '" << name_ << "' terminating with handles:";
      uv_print_all_handles(&uv_loop_, stderr);
      abort();
    }
  }

  static void uv_async_callback(uv_async_t* async) {
    Impl* impl = static_cast<Impl*>(async->data);
    std::list<std::function<void()>> current;

    {
      std::lock_guard<std::mutex> lock(impl->queue_mutex_);
      current.swap(impl->queue_);
    }

    for (auto& fn : current) {
      fn();
    }
  }

  std::string name_;

  std::thread thread_;
  std::timed_mutex running_mutex_;

  uv_loop_t uv_loop_;
  uv_async_t uv_async_;

  std::mutex queue_mutex_;
  bool terminating_ GUARDED_BY(queue_mutex_) = false;
  std::list<std::function<void()>> queue_ GUARDED_BY(queue_mutex_);
};

RunLoop::RunLoop(std::string name) : impl_(std::make_unique<Impl>(std::move(name))) {}
RunLoop::~RunLoop() = default;

bool RunLoop::IsOnMainThread() const {
  return impl_->IsOnMainThread();
}

void RunLoop::AssertOnMainThread() const {
  CHECK(impl_->IsOnMainThread());
}

bool RunLoop::RunFunction(std::function<void()> fn) {
  return impl_->RunFunction(std::move(fn));
}

struct StreamHandle::Impl {
  virtual ~Impl() {}

  virtual void Close() = 0;

  virtual void BeginRead(std::function<void(ssize_t rc, const char* buf)> callback) = 0;
  virtual void StopRead() = 0;

  virtual bool Write(std::vector<char> buf,
                     std::function<void(std::vector<char>&, int)> write_callback) = 0;

  // libuv only handles pipes, sockets, and TTYs, we have to emulate its
  // behavior for regular files ourselves.
  struct FileImpl;
  struct UvImpl;
};

struct StreamHandle::Impl::UvImpl : public StreamHandle::Impl {
  UvImpl(RunLoop& loop, unique_socket fd)
      : loop_(loop), uv_(UvFromFd(loop.impl_->Loop(), std::move(fd))) {
    loop_.AssertOnMainThread();
  }

  virtual ~UvImpl() = default;

  virtual void Close() override final {
    std::visit([&](auto&& uv_stream) { uv_stream.Close(); }, uv_);
  }

  virtual void BeginRead(std::function<void(ssize_t rc, const char* buf)> callback) override final {
    std::visit([&](auto&& uv_stream) { uv_stream.BeginRead(callback); }, uv_);
  }

  virtual void StopRead() override final {
    std::visit([&](auto&& uv_stream) { uv_stream.StopRead(); }, uv_);
  }

  virtual bool Write(std::vector<char> buf,
                     std::function<void(std::vector<char>&, int)> write_callback) override final {
    return std::visit(
        [&](auto&& uv_stream) { return uv_stream.Write(std::move(buf), write_callback); }, uv_);
  }

  RunLoop& loop_;
  std::variant<UvPipe, UvTcp, UvTty> uv_;
};

StreamHandle::StreamHandle(RunLoop& loop, unique_socket fd) : loop_(loop) {
  impl_ = std::make_unique<Impl::UvImpl>(loop, std::move(fd));
}
StreamHandle::~StreamHandle() = default;

void StreamHandle::Close() {
  impl_->Close();
}

void StreamHandle::BeginRead(std::function<void(ssize_t rc, const char* buf)> callback) {
  impl_->BeginRead(callback);
}

void StreamHandle::StopRead() {
  impl_->StopRead();
}

bool StreamHandle::Write(std::vector<char> buf,
                         std::function<void(std::vector<char>&, int)> write_callback) {
  return impl_->Write(std::move(buf), write_callback);
}
