#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace microide::project::remote {

class LinkHeartbeat {
 public:
  struct Config {
    std::int64_t interval_ms = 2000;
    std::size_t max_missed = 3;
  };

  LinkHeartbeat() : LinkHeartbeat(Config{}) {}
  explicit LinkHeartbeat(Config config) : config_(config) {}

  std::optional<std::int64_t> Tick(std::int64_t now_ms);

  void OnPong(std::int64_t sent_ms, std::int64_t now_ms);

  std::int64_t NextTickDelayMs(std::int64_t now_ms) const;

  bool dead() const { return dead_; }
  std::size_t outstanding() const { return outstanding_; }
  std::optional<std::int64_t> rtt_ms() const { return rtt_ms_; }

 private:
  Config config_;
  std::optional<std::int64_t> last_sent_ms_;
  std::int64_t first_sent_ms_ = 0;
  std::size_t outstanding_ = 0;
  bool dead_ = false;
  std::optional<std::int64_t> rtt_ms_;
};

}
