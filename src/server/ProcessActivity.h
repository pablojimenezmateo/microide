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

// What a host's microide objects cost (remote-host-activity D2): resident memory
// and CPU share of a pid, or of a pid and everything under it, from one read of
// those trees per sample -- down /proc/<pid>/task/*/children, so a host running
// thousands of processes costs what microide's tens do; a kernel without the
// children files gets one walk of all of /proc instead. CPU is a delta: the ticks a pid's tree spent since the
// previous sample that asked for that pid, over the time between the two -- so
// the first sample of a pid has none, and says so.
//
// Nothing here runs unless asked: the server samples for a request that wants
// activity, or on its 2 s sampler while a connection is subscribed. One instance
// is shared by both, so their deltas are against the same previous sample.
class ProcessActivity {
 public:
  // One process's line of /proc/<pid>/stat, the fields a sample reads.
  struct Stat {
    int pid = 0;
    int ppid = 0;
    int tpgid = -1;  // its controlling terminal's foreground process group; -1 = none
    // utime + stime + cutime + cstime: its own CPU plus that of the children it
    // reaped, so a compiler that came and went under `make` between two samples
    // is still counted -- in make's cutime -- rather than lost with its pid.
    std::uint64_t ticks = 0;
    std::uint64_t own_ticks = 0;  // utime + stime only
    std::uint64_t start_time = 0;  // ticks after boot: tells a reused pid apart
    std::int64_t rss_pages = 0;
  };
  // Parse one stat line; nullopt for anything that is not one (a truncated read,
  // a comm with no closing parenthesis, a field that is not a number).
  static std::optional<Stat> ParseStat(std::string_view line);

  // A pid to report on: alone (the server's own row) or with every descendant (a
  // terminal's shell and what it runs, a spawned tool and its children).
  struct Root {
    int pid = -1;
    bool subtree = true;
  };
  struct Figures {
    std::int64_t rss_bytes = -1;    // -1 = not found
    std::int32_t cpu_permille = -1;  // thousandths of one core; -1 = no previous sample yet
    bool operator==(const Figures&) const = default;
  };

  // What runs in the foreground of the terminal `shell_pid` controls -- the comm of
  // its foreground process group's leader (`watch`, `vim`, `make`) -- or "" while
  // the shell itself is in the foreground, or when it is gone.
  static std::string ForegroundCommand(int shell_pid);

  // Walk /proc once and report on each of `roots`, in order.
  std::vector<Figures> Sample(std::span<const Root> roots);
  // The same over a table already read (tests; Sample's own second half), taken
  // at `now`. `ticks_per_second` and `page_bytes` are the machine's.
  std::vector<Figures> Measure(std::span<const Stat> table, std::span<const Root> roots,
                               std::chrono::steady_clock::time_point now, std::int64_t ticks_per_second,
                               std::int64_t page_bytes);

 private:
  struct Previous {
    std::uint64_t start_time = 0;
    std::uint64_t ticks = 0;
    std::chrono::steady_clock::time_point at{};
  };
  // A root's key: its pid and whether it is summed with its tree.
  static std::uint64_t KeyOf(const Root& root) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(root.pid)) << 1) | (root.subtree ? 1u : 0u);
  }
  std::mutex mutex_;  // the sampler thread and a request's I/O thread both sample
  // By KeyOf; a root not asked for by the latest sample is dropped.
  std::unordered_map<std::uint64_t, Previous> previous_;
};

}  // namespace microide::server
