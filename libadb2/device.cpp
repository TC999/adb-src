#include <libadb2/device.h>

#include <condition_variable>
#include <memory>
#include <mutex>

#include <android-base/logging.h>

#include "provider.h"

namespace adb {

std::ostream& operator<<(std::ostream& os, DeviceState state) {
  switch (state) {
    case kDeviceStateConnecting:
      return os << "connecting";
    case kDeviceStateUnauthorized:
      return os << "unauthorized";
    case kDeviceStateOnline:
      return os << "online";
    case kDeviceStateOffline:
      return os << "offline";
    default:
      LOG(FATAL) << "unknown DeviceState " << static_cast<int>(state);
      abort();
  }
}

LocalDeviceInterface::LocalDeviceInterface(RunLoop& loop) : loop_(loop) {}

DeviceState LocalDeviceInterface::GetState() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

DeviceState LocalDeviceInterface::SetState(DeviceState new_state) {
  DeviceState old_state;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    old_state = state_;
    state_ = new_state;
  }

  if (old_state != new_state) {
    state_cv_.notify_all();
  }

  LOG(INFO) << *this << ": changing state from " << old_state << " to " << new_state;

  return old_state;
}

// Thread safety annotations can't model unique_lock.
DeviceState LocalDeviceInterface::WaitForStateChange(DeviceState original_state)
    NO_THREAD_SAFETY_ANALYSIS {
  LOG(VERBOSE) << *this << ": starting WaitForStateChange, previous state = " << original_state;
  std::unique_lock<std::mutex> lock(state_mutex_);
  auto pred = [this, original_state]()
      NO_THREAD_SAFETY_ANALYSIS { return state_ != original_state; };
  state_cv_.wait(lock, pred);
  LOG(VERBOSE) << *this << ": finished WaitForStateChange, new state = " << state_;
  return state_;
}

std::shared_ptr<ProtocolInterface> LocalDeviceInterface::SetProtocol(
    std::shared_ptr<ProtocolInterface> new_protocol) {
  std::shared_ptr<ProtocolInterface> old_protocol = this->protocol_;
  this->protocol_ = new_protocol;
  return old_protocol;
}

void LocalDeviceInterface::Send(Packet packet) {
  packet.UpdateHeader();

  std::vector<char> header;
  const char* header_data = reinterpret_cast<char*>(&packet.header);
  header.assign(header_data, header_data + sizeof(packet.header));

  Send(std::move(header));
  if (!packet.data.empty()) {
    Send(std::move(packet.data));
  }
}

std::ostream& operator<<(std::ostream& os, const DeviceInterface& device) {
  return os << device.ToString();
}

}  // namespace adb
