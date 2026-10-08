#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace microide::project::remote {

// The link's liveness and round-trip estimate (dev-docs/design/remote-projects.md
// § 6.4). The client pings every `interval_ms` on the interactive lane; any answer
// proves the link and clears the misses; `max_missed` pings in a row without one
// declares it dead — 6 s at the defaults, where ssh's own keepalive takes 45.
//
// Pure: no clock of its own, every call takes `now_ms`, so the rule is tested with
// made-up time rather than by waiting for it.
class LinkHeartbeat {
 public:
  struct Config {
    std::int64_t interval_ms = 2000;
    std::size_t max_missed = 3;
  };

  LinkHeartbeat() : LinkHeartbeat(Config{}) {}
  explicit LinkHeartbeat(Config config) : config_(config) {}

  // Called on every tick. Returns the stamp to put in a ping when one is due (the
  // caller sends it), and declares the link dead once `max_missed` pings went
  // unanswered for a full interval each.
  std::optional<std::int64_t> Tick(std::int64_t now_ms);

  // An answer to the ping stamped `sent_ms`. Stamps this heartbeat never sent are
  // ignored (a peer cannot make the RTT up); a late answer still proves liveness.
  void OnPong(std::int64_t sent_ms, std::int64_t now_ms);

  bool dead() const { return dead_; }
  std::size_t outstanding() const { return outstanding_; }
  // Smoothed round trip (TCP's 1/8 gain), once there has been an answer.
  std::optional<std::int64_t> rtt_ms() const { return rtt_ms_; }

 private:
  Config config_;
  std::optional<std::int64_t> last_sent_ms_;
  std::int64_t first_sent_ms_ = 0;  // stamps below this were never ours
  std::size_t outstanding_ = 0;
  bool dead_ = false;
  std::optional<std::int64_t> rtt_ms_;
};

}  // namespace microide::project::remote
