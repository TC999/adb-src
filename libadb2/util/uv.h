#pragma once

#include <list>
#include <functional>
#include <type_traits>
#include <variant>
#include <vector>

#include <android-base/logging.h>
#include <uv.h>

#include <libadb2/socket.h>

template <typename T>
struct uv_cast;

#define ALLOW_CAST(FromType) \
  explicit uv_cast(FromType from) : value(reinterpret_cast<decltype(value)>(from)) {}

template <>
struct uv_cast<uv_handle_t*> {
  ALLOW_CAST(uv_async_t*);
  ALLOW_CAST(uv_stream_t*);
  ALLOW_CAST(uv_pipe_t*);
  ALLOW_CAST(uv_tcp_t*);
  ALLOW_CAST(uv_tty_t*);

  operator uv_handle_t*() { return value; }

  uv_handle_t* value;
};

template <>
struct uv_cast<uv_stream_t*> {
  ALLOW_CAST(uv_pipe_t*);
  ALLOW_CAST(uv_tcp_t*);
  ALLOW_CAST(uv_tty_t*);

  operator uv_stream_t*() { return value; }

  uv_stream_t* value;
};

// RAII wrapper class for uv_loop_t.
class UvLoop {
 public:
  UvLoop() {
    int rc = uv_loop_init(&loop_);
    CHECK_EQ(0, rc) << "failed to initialize uv_loop";
  }

  UvLoop(const UvLoop& copy) = delete;
  UvLoop(UvLoop&& move) = delete;

  ~UvLoop() { uv_loop_close(&loop_); }

  uv_loop_t* get() { return &loop_; }

 private:
  uv_loop_t loop_;
};

// A semi-RAII wrapper that acts as storage for a uv_handle_t.
template <typename T>
class UvHandle {
 protected:
  UvHandle() : handle_(new T()) {}
  ~UvHandle() { reset(); }

  UvHandle(const UvHandle& copy) = delete;
  UvHandle(UvHandle&& move) {
    this->handle_ = nullptr;
    std::swap(this->handle_, move.handle_);
  }

  UvHandle& operator=(const UvHandle& copy) = delete;
  UvHandle& operator=(UvHandle&& move) {
    reset();
    std::swap(this->handle_, move.handle_);
    return *this;
  }

  T* operator->() { return handle_; }
  T& operator*() { return *handle_; }

  T* get() { return handle_; }
  bool valid() { return handle_; }

  void reset() {
    if (handle_ != nullptr) {
      uv_close(reinterpret_cast<uv_handle_t*>(handle_),
               [](uv_handle_t* handle) { delete reinterpret_cast<T*>(handle); });
      handle_ = nullptr;
    }
  }

 public:
  void Close() { reset(); }

 private:
  T* handle_;
};

class UvAsync : public UvHandle<uv_async_t> {
 public:
  UvAsync(uv_loop_t* loop, std::function<void()> callback) : callback_(callback) {
    int rc = uv_async_init(loop, get(), UvAsyncCallback);
    CHECK_EQ(0, rc);
    get()->data = this;
  }

  void Notify() {
    CHECK(this->valid());
    uv_async_send(get());
  }

 private:
  static void UvAsyncCallback(uv_async_t* handle) {
    static_cast<UvAsync*>(handle->data)->callback_();
  }

  std::function<void()> callback_;
};

// Wrapper class for a uv_write and the data for that write.
struct StreamWrite {
  StreamWrite(std::vector<char> data, std::function<void(std::vector<char>&, int)> callback)
      : data(std::move(data)), callback(callback) {}

  // uv_write will store pointers into data, and iterators can be invalidated upon a move.
  StreamWrite(const StreamWrite& copy) = delete;
  StreamWrite(StreamWrite&& move) = delete;

  uv_write_t request;
  std::vector<char> data;
  std::function<void(std::vector<char>& data, int status)> callback;
};

template <typename T>
class UvStream : public UvHandle<T> {
 public:
  UvStream() {}

  bool BeginRead(std::function<void(ssize_t, const char*)> callback, size_t buffer_size = 65536) {
    CHECK(this->valid());

    CHECK(callback);
    CHECK(read_callback_ == nullptr);
    read_callback_ = callback;

    CHECK(read_buffer_ == nullptr);
    read_buffer_ = std::make_unique<char[]>(buffer_size);
    read_buffer_size_ = buffer_size;
    CHECK(read_buffer_ != nullptr);

    auto alloc_cb = +[](uv_handle_t* handle, size_t, uv_buf_t* buf) {
      UvStream* stream = static_cast<UvStream*>(handle->data);
      buf->base = stream->read_buffer_.get();
      buf->len = stream->read_buffer_size_;
    };

    auto read_cb_ = +[](uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
      UvStream* stream = static_cast<UvStream*>(handle->data);
      stream->read_callback_(nread, buf->base);
    };

    this->get()->data = this;
    int rc = uv_read_start(uv_cast<uv_stream_t*>(this->get()), alloc_cb, read_cb_);
    if (rc != 0) {
      LOG(ERROR) << "uv_read_start failed: " << uv_strerror(rc);
      return false;
    }

    return true;
  }

  bool StopRead() {
    CHECK(this->valid());
    int rc = uv_read_stop(uv_cast<uv_stream_t*>(this->get()));
    if (rc != 0) {
      LOG(ERROR) << "uv_read_stop failed: " << uv_strerror(rc);
      return false;
    }

    read_buffer_.reset();
    read_buffer_size_ = 0;
    return true;
  }

  bool Write(std::vector<char> buf, std::function<void(std::vector<char>&, int)> write_callback) {
    CHECK(this->valid());
    size_t len = buf.size();
    outgoing_.emplace_back(std::move(buf), write_callback);

    auto& write_req = outgoing_.back();
    write_req.request.data = this;

    uv_buf_t bufs = {.base = write_req.data.data(), .len = len};
    int rc =
        uv_write(&write_req.request, uv_cast<uv_stream_t*>(this->get()), &bufs, 1, stream_write_cb);
    if (rc != 0) {
      LOG(ERROR) << "failed to enqueue write: " << uv_strerror(rc);
      outgoing_.pop_back();
      return false;
    }
    return true;
  }

  static void stream_write_cb(uv_write_t* req, int status) {
    UvStream* stream = static_cast<UvStream*>(req->data);
    auto& write = stream->outgoing_.front();
    if (write.callback) {
      write.callback(write.data, status);
    }
    stream->outgoing_.pop_front();
  }

 private:
  std::list<StreamWrite> outgoing_;
  std::unique_ptr<char[]> read_buffer_;
  size_t read_buffer_size_;
  std::function<void(ssize_t, const char*)> read_callback_;
};

class UvTcp : public UvStream<uv_tcp_t> {
 public:
  UvTcp(uv_loop_t* loop, unique_socket fd) {
    int rc = uv_tcp_init(loop, get());
    CHECK_EQ(0, rc);
    rc = uv_tcp_open(get(), fd.release());
    CHECK_EQ(0, rc);
  }
};

class UvPipe : public UvStream<uv_pipe_t> {
 public:
  UvPipe(uv_loop_t* loop, unique_socket fd) {
    int rc = uv_pipe_init(loop, get(), 0);
    CHECK_EQ(0, rc);
    rc = uv_pipe_open(get(), fd.release());
    CHECK_EQ(0, rc);
  }
};

class UvTty : public UvStream<uv_tty_t> {
 public:
  UvTty(uv_loop_t* loop, unique_socket fd) {
    int rc = uv_tty_init(loop, get(), fd.release(), 1);
    CHECK_EQ(0, rc);
  }
};

std::variant<UvPipe, UvTcp, UvTty> UvFromFd(uv_loop_t* loop, unique_socket fd);
