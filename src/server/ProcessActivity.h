#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microide::server {

class ProcessActivity {
 public:
  struct Stat {
    int pid = 0;
    int ppid = 0;
    int tpgid = -1;
    std::uint64_t ticks = 0;
    std::uint64_t own_ticks = 0;
    std::uint64_t start_time = 0;
    std::int64_t rss_pages = 0;
  };
  static std::optional<Stat> ParseStat(std::string_view line);

  struct Root {
    int pid = -1;
    bool subtree = true;
  };
  struct Figures {
    std::int64_t rss_bytes = -1;
    std::int32_t cpu_permille = -1;
    bool operator==(const Figures&) const = default;
  };

  static std::string ForegroundCommand(int shell_pid);

  static std::vector<Stat> ReadTable(std::span<const Root> roots);

  std::vector<Figures> Sample(std::span<const Root> roots);
  std::vector<Figures> Measure(std::span<const Stat> table, std::span<const Root> roots,
                               std::chrono::steady_clock::time_point now, std::int64_t ticks_per_second,
                               std::int64_t page_bytes);

 private:
  struct Previous {
    std::uint64_t start_time = 0;
    std::uint64_t ticks = 0;
    std::chrono::steady_clock::time_point at{};
  };
  static std::uint64_t KeyOf(const Root& root) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(root.pid)) << 1) | (root.subtree ? 1u : 0u);
  }
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, Previous> previous_;
};

}
