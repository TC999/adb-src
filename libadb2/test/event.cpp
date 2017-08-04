#include <libadb2/event.h>

#include <signal.h>
#include <sys/types.h>
#include <syscall.h>

#include <gtest/gtest.h>
#include <uv.h>

#include <libadb2/socket.h>

TEST(RunLoop, smoke) {
  RunLoop loop("smoke");
}

TEST(RunLoop, run) {
  RunLoop loop("run");
  volatile int x;
  auto future = loop.Run([&loop, &x]() {
    loop.AssertOnMainThread();
    x = 0xdeadbeef;
    return x;
  });
  future.wait();
  ASSERT_EQ(future.get(), x);
}

TEST(RunLoop, exit) {
  pid_t tid;

  {
    RunLoop loop("exit");
    auto get_tid = []() { return syscall(__NR_gettid); };
    auto future_1 = loop.Run(get_tid);
    auto future_2 = loop.Run(get_tid);
    future_1.wait();
    future_2.wait();
    tid = future_1.get();
    ASSERT_EQ(tid, future_2.get());
    ASSERT_NE(tid, get_tid());
  }
  ASSERT_EQ(-1, syscall(__NR_tgkill, getpid(), tid, 0));
  ASSERT_EQ(ESRCH, errno);
}

TEST(RunLoop, AssertOnMainThread) {
  RunLoop loop("AssertOnMainThread");
  auto future = loop.Run([&loop]() { loop.AssertOnMainThread(); });
  future.wait();
  ASSERT_DEATH(loop.AssertOnMainThread(), "");
}

TEST(StreamHandle, smoke) {
  static int iteration_count = 0;

  // Manually track ownership of handle, since it needs to be destructed on the run loop.
  StreamHandle* handle;
  RunLoop loop("smoke");
  unique_socket fd1, fd2;
  std::string output;
  ASSERT_TRUE(adb::Socketpair(&fd1, &fd2));
  auto callback = [&loop, &handle, &fd2]() {
    loop.AssertOnMainThread();
    handle = new StreamHandle(loop, std::move(fd2));

    handle->loop_.AssertOnMainThread();
    handle->BeginRead([&handle](ssize_t rc, const char* buf) {
      handle->loop_.AssertOnMainThread();
      if (iteration_count++ == 0) {
        ASSERT_EQ(3, rc);
        ASSERT_EQ(0, memcmp("foo", buf, 3));
        const char* buf = "bar";
        handle->Write(std::vector<char>(buf, buf + 3), nullptr);
      } else {
        ASSERT_EQ(UV_EOF, rc);
        handle->StopRead();
        handle->Close();
        delete handle;
      }
    });

    const char* buf = "foo";
    handle->Write(std::vector<char>(buf, buf + 3), nullptr);
  };

  loop.Run(callback).wait();

  char buf[16];
  ssize_t rc = read(fd1.get(), buf, sizeof(buf));
  ASSERT_EQ(3, rc);
  ASSERT_EQ(0, memcmp("foo", buf, 3));

  rc = write(fd1.get(), "foo", 3);
  ASSERT_EQ(3, rc);

  rc = read(fd1.get(), buf, sizeof(buf));
  ASSERT_EQ(3, rc);
  ASSERT_EQ(0, memcmp("bar", buf, 3));

  ASSERT_EQ(0, shutdown(fd1.get(), SHUT_WR));
  ASSERT_EQ(0, read(fd1.get(), buf, sizeof(buf)));
}
