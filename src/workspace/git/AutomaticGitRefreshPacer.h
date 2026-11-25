#pragma once

#include <cstdint>

#include "util/Waker.h"

namespace microide::workspace {

class AutomaticGitRefreshPacer {
 public:
  static constexpr std::uint64_t kIntervalMs = 750;

  explicit AutomaticGitRefreshPacer(const util::WakeChannel& wake_channel)
      : wake_channel_(wake_channel) {}

  [[nodiscard]] bool Request(std::uint64_t now_ms, bool refresh_running);

  [[nodiscard]] bool Resume(std::uint64_t now_ms, bool refresh_running);

  [[nodiscard]] bool pending() const { return pending_; }
  [[nodiscard]] std::uint64_t wake_armed_for_ms() const { return wake_armed_for_ms_; }

 private:
  const util::WakeChannel& wake_channel_;
  bool pending_ = false;
  std::uint64_t next_allowed_ms_ = 0;
  std::uint64_t wake_armed_for_ms_ = 0;
};

}
