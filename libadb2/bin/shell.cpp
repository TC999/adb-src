#include <err.h>

#include <future>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include <libadb2/context.h>
#include <libadb2/device.h>

namespace adb {
namespace commands {
class Command {
 public:
  virtual ~Command() {}

  // Canonical name for this command.
  virtual std::string Name() = 0;

  // Other names this command can be known by.
  virtual std::unordered_set<std::string> Aliases() { return {}; }

  // Category of the command for organization in `adb help`.
  virtual std::string Category() { return "miscellaneous"; }

  // Short description of the command.
  virtual std::string Description() = 0;

  // Full help text for the command.
  virtual std::string HelpText() = 0;

  virtual int Execute(ContextInterface* context, DeviceInterface* device,
                      std::vector<std::string_view> args) = 0;
};

class ShellCommand : public Command {
 public:
  virtual ~ShellCommand() {}

  virtual std::string Name() override final { return "shell"; }

  virtual std::string Category() override final { return "shell"; }

  virtual std::string Description() override final { return "run a remote shell command"; }
  virtual std::string HelpText() override final { return "FIXME!"; }

  virtual int Execute(ContextInterface*, DeviceInterface* device,
                      std::vector<std::string_view> args) override {
    adb::DeviceState state = device->GetState();
    while (state == adb::kDeviceStateConnecting) {
      state = device->WaitForStateChange(state);
    }

    adb::ShellOptions options;
    options.io.in.reset(STDIN_FILENO);
    options.io.out.reset(STDOUT_FILENO);

    auto return_future = options.io.return_value.get_future();
    options.command = android::base::Join(args, " ");

    auto shell_future = device->OpenShell(std::move(options));
    shell_future.wait();
    auto shell_interface = shell_future.get();

    return_future.wait();
    return return_future.get();
  }
};
}
}

int main(int argc, char** argv) {
  if (argc < 1) {
    LOG(FATAL) << "invalid arguments";
  }

  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }

  const char* hostname = "localhost";
  int port = 5555;
#if 0
  if (argc == 3) {
    hostname = argv[1];
    if (!android::base::ParseInt(argv[2], &port, 1, 65535)) {
      errx(1, "failed to parse port %s", argv[1]);
    }
  } else if (argc != 1) {
    errx(1, "usage: adb2 [HOSTNAME PORT]");
  }
#endif

  android::base::SetMinimumLogSeverity(android::base::VERBOSE);
  adb::Context ctx;
  auto device_future = ctx.ConnectDevice(hostname, port);
  device_future.wait();
  auto device = device_future.get();
  if (!device) {
    errx(1, "failed to connect to device");
  }

  adb::commands::ShellCommand shell;
  shell.Execute(&ctx, device.get(), args);
}
