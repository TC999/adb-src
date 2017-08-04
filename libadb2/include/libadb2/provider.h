#pragma once

namespace adb {

class ProviderInterface {
 public:
  ProviderInterface();
  virtual ~ProviderInterface();

  ProviderInterface(const ProviderInterface& copy) = delete;
  ProviderInterface(ProviderInterface&& move) = delete;
};

}  // namespace adb
