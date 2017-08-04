#pragma once

#include <stdint.h>

namespace adb {

using DeviceId = uint64_t;

class ContextInterface;

class DeviceInterface;
class FileSyncInterface;
class ShellInterface;

class ProtocolInterface;

}  // namespace adb
