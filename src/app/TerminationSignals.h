#pragma once

#include <cstdint>
#include <thread>

namespace microide::app {

class TerminationSignalWatcher {
 public:
  explicit TerminationSignalWatcher(std::uint32_t event_type);
  ~TerminationSignalWatcher();
  TerminationSignalWatcher(const TerminationSignalWatcher&) = delete;
  TerminationSignalWatcher& operator=(const TerminationSignalWatcher&) = delete;

  bool installed() const { return installed_; }

 private:
  std::thread thread_;
  bool installed_ = false;
};

}
