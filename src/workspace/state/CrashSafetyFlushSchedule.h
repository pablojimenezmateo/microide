#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace microide::workspace {

struct CrashSafetyFlushSchedule {
  static constexpr std::uint64_t kQuietMs = 2500;
  static constexpr std::uint64_t kMaxWaitMs = 10000;

  void Note(std::uint64_t now_ms) {
    if (!armed_) {
      armed_ = true;
      first_ms_ = now_ms;
    }
    last_ms_ = now_ms;
  }

  void Touch(std::uint64_t now_ms) {
    if (armed_) {
      last_ms_ = now_ms;
    }
  }

  bool armed() const { return armed_; }

  bool Due(std::uint64_t now_ms) const { return armed_ && now_ms >= Deadline(); }

  std::optional<std::uint64_t> DelayMs(std::uint64_t now_ms) const {
    if (!armed_) {
      return std::nullopt;
    }
    const std::uint64_t deadline = Deadline();
    return now_ms >= deadline ? 0 : deadline - now_ms;
  }

  void Disarm() { armed_ = false; }

 private:
  std::uint64_t Deadline() const { return std::min(last_ms_ + kQuietMs, first_ms_ + kMaxWaitMs); }

  std::uint64_t first_ms_ = 0;
  std::uint64_t last_ms_ = 0;
  bool armed_ = false;
};

}
