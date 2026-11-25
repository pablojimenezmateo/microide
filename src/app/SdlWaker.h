#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace microide::app {

void InstallSdlWaker();

class SdlBlockingWaitScope {
 public:
  SdlBlockingWaitScope();
  ~SdlBlockingWaitScope();
  SdlBlockingWaitScope(const SdlBlockingWaitScope&) = delete;
  SdlBlockingWaitScope& operator=(const SdlBlockingWaitScope&) = delete;
};

struct SdlWakeCounts {
  std::uint64_t pushes = 0;
  std::uint64_t nudges = 0;
  std::uint64_t first_push_in_wait_ms = 0;
};
SdlWakeCounts ReadSdlWakeCounts();

inline constexpr std::uint64_t kLateWakeReportMs = 250;

inline std::optional<std::string> DescribeLateWake(std::uint64_t owed_ms, bool timed, std::int32_t timeout_ms,
                                                   SdlWakeCounts before, SdlWakeCounts after,
                                                   bool has_event, std::uint32_t event_type) {
  const std::uint64_t pushed = after.pushes - before.pushes;
  if (owed_ms < kLateWakeReportMs || pushed == 0) {
    return std::nullopt;
  }
  std::string line = "microide idle-wait: woke " + std::to_string(owed_ms) + " ms after its first wake (" +
                     (timed ? "timeout " + std::to_string(timeout_ms) + " ms" : std::string("no timeout")) +
                     ") with " + std::to_string(pushed) + " wake(s) pushed during it and " +
                     std::to_string(after.nudges - before.nudges) + " relay nudge(s); ended by ";
  line += has_event ? "event " + std::to_string(event_type) : std::string("its timeout");
  return line;
}

bool IsSdlWakeNudge(std::uint32_t event_type);

void InstallSdlLogSink();

std::uint32_t RegisterSdlWakeChannel(const char* name);

std::string SdlEventName(std::uint32_t event_type);

}
