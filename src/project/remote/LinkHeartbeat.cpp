#include "project/remote/LinkHeartbeat.h"

namespace microide::project::remote {

std::optional<std::int64_t> LinkHeartbeat::Tick(std::int64_t now_ms) {
  if (dead_) {
    return std::nullopt;
  }
  if (last_sent_ms_.has_value() && now_ms - *last_sent_ms_ < config_.interval_ms) {
    return std::nullopt;
  }
  // A full interval since the last ping: if it and the ones before it all went
  // unanswered, that is `max_missed` misses.
  if (outstanding_ >= config_.max_missed) {
    dead_ = true;
    return std::nullopt;
  }
  if (!last_sent_ms_.has_value()) {
    first_sent_ms_ = now_ms;
  }
  last_sent_ms_ = now_ms;
  ++outstanding_;
  return now_ms;
}

void LinkHeartbeat::OnPong(std::int64_t sent_ms, std::int64_t now_ms) {
  if (dead_ || !last_sent_ms_.has_value() || sent_ms < first_sent_ms_ ||
      sent_ms > *last_sent_ms_ || now_ms < sent_ms) {
    return;
  }
  outstanding_ = 0;
  const std::int64_t sample = now_ms - sent_ms;
  rtt_ms_ = rtt_ms_.has_value() ? *rtt_ms_ + (sample - *rtt_ms_) / 8 : sample;
}

}  // namespace microide::project::remote
