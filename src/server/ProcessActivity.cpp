#include "server/ProcessActivity.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace microide::server {
namespace {

template <typename T>
bool ParseField(std::string_view text, T* out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *out);
  return error == std::errc() && end == text.data() + text.size();
}

// /proc/<pid>/stat into `buffer`; the line is a few hundred bytes (comm is at most
// 16), so one read takes it whole. 0 when the process went between the readdir
// and the open, which is ordinary.
std::size_t ReadStat(const char* name, char* buffer, std::size_t size) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%s/stat", name);
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  ssize_t got = 0;
  do {
    got = ::read(fd, buffer, size);
  } while (got < 0 && errno == EINTR);
  ::close(fd);
  return got > 0 ? static_cast<std::size_t>(got) : 0;
}

}  // namespace

std::optional<ProcessActivity::Stat> ProcessActivity::ParseStat(std::string_view line) {
  // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so the
  // fields start after the LAST ')'.
  const std::size_t open = line.find(" (");
  const std::size_t close = line.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
    return std::nullopt;
  }
  Stat stat;
  if (!ParseField(line.substr(0, open), &stat.pid) || stat.pid <= 0) {
    return std::nullopt;
  }
  // Fields 3.. (1-based, as proc(5) numbers them) split on single spaces.
  std::string_view rest = line.substr(close + 1);
  std::uint64_t utime = 0;
  std::uint64_t stime = 0;
  std::int64_t cutime = 0;
  std::int64_t cstime = 0;
  int field = 2;
  bool rss_seen = false;
  while (!rest.empty() && field < 24) {
    if (rest.front() != ' ') {
      return std::nullopt;
    }
    rest.remove_prefix(1);
    const std::size_t end = std::min(rest.find(' '), rest.size());
    std::string_view value = rest.substr(0, end);
    if (end == rest.size() && !value.empty() && value.back() == '\n') {
      value.remove_suffix(1);
    }
    rest.remove_prefix(end);
    ++field;
    bool ok = true;
    switch (field) {
      case 4:
        ok = ParseField(value, &stat.ppid);
        break;
      case 14:
        ok = ParseField(value, &utime);
        break;
      case 15:
        ok = ParseField(value, &stime);
        break;
      case 16:
        ok = ParseField(value, &cutime);
        break;
      case 17:
        ok = ParseField(value, &cstime);
        break;
      case 22:
        ok = ParseField(value, &stat.start_time);
        break;
      case 24:
        ok = ParseField(value, &stat.rss_pages);
        rss_seen = ok;
        break;
      default:
        break;
    }
    if (!ok) {
      return std::nullopt;
    }
  }
  if (!rss_seen || stat.ppid < 0 || cutime < 0 || cstime < 0) {
    return std::nullopt;
  }
  stat.own_ticks = utime + stime;
  stat.ticks = stat.own_ticks + static_cast<std::uint64_t>(cutime) + static_cast<std::uint64_t>(cstime);
  stat.rss_pages = std::max<std::int64_t>(stat.rss_pages, 0);
  return stat;
}

std::vector<ProcessActivity::Figures> ProcessActivity::Sample(std::span<const Root> roots) {
  std::vector<Stat> table;
  if (DIR* proc = ::opendir("/proc"); proc != nullptr) {
    table.reserve(512);
    char buffer[1024];
    while (const dirent* entry = ::readdir(proc)) {
      if (entry->d_name[0] < '1' || entry->d_name[0] > '9') {
        continue;  // ".", "self", "sys", ...: not a process
      }
      const std::size_t got = ReadStat(entry->d_name, buffer, sizeof(buffer));
      if (const std::optional<Stat> stat = ParseStat(std::string_view(buffer, got)); stat.has_value()) {
        table.push_back(*stat);
      }
    }
    ::closedir(proc);
  }
  static const std::int64_t ticks_per_second = std::max<long>(::sysconf(_SC_CLK_TCK), 1);
  static const std::int64_t page_bytes = std::max<long>(::sysconf(_SC_PAGESIZE), 1);
  return Measure(table, roots, std::chrono::steady_clock::now(), ticks_per_second, page_bytes);
}

std::vector<ProcessActivity::Figures> ProcessActivity::Measure(std::span<const Stat> table,
                                                               std::span<const Root> roots,
                                                               std::chrono::steady_clock::time_point now,
                                                               std::int64_t ticks_per_second,
                                                               std::int64_t page_bytes) {
  // Children by parent: indices into `table` sorted by ppid, so a pid's children
  // are one equal_range.
  std::vector<std::size_t> by_parent(table.size());
  for (std::size_t i = 0; i < table.size(); ++i) {
    by_parent[i] = i;
  }
  std::sort(by_parent.begin(), by_parent.end(),
            [&](std::size_t a, std::size_t b) { return table[a].ppid < table[b].ppid; });
  std::unordered_map<int, std::size_t> by_pid;
  by_pid.reserve(table.size());
  for (std::size_t i = 0; i < table.size(); ++i) {
    by_pid.emplace(table[i].pid, i);
  }
  const auto find = [&](int pid) -> const Stat* {
    const auto it = by_pid.find(pid);
    return it != by_pid.end() ? &table[it->second] : nullptr;
  };
  // Which root's walk last visited each entry (r + 1): one array for every walk.
  std::vector<std::size_t> visited(table.size(), 0);

  std::vector<Figures> figures(roots.size());
  std::lock_guard lock(mutex_);
  std::unordered_map<std::uint64_t, Previous> next;
  next.reserve(roots.size());
  std::vector<std::size_t> pending;
  for (std::size_t r = 0; r < roots.size(); ++r) {
    const Root& root = roots[r];
    const Stat* top = root.pid > 0 ? find(root.pid) : nullptr;
    if (top == nullptr) {
      continue;
    }
    std::uint64_t ticks = root.subtree ? top->ticks : top->own_ticks;
    std::int64_t pages = top->rss_pages;
    if (root.subtree) {
      // Bounded by the table: a parent loop (impossible, but this is parsed text)
      // cannot spin, since every entry is visited at most once per root.
      pending.assign(1, static_cast<std::size_t>(top - table.data()));
      visited[pending.front()] = r + 1;
      while (!pending.empty()) {
        const int parent = table[pending.back()].pid;
        pending.pop_back();
        auto it = std::lower_bound(by_parent.begin(), by_parent.end(), parent,
                                   [&](std::size_t index, int pid) { return table[index].ppid < pid; });
        for (; it != by_parent.end() && table[*it].ppid == parent; ++it) {
          if (visited[*it] == r + 1) {
            continue;
          }
          visited[*it] = r + 1;
          ticks += table[*it].ticks;
          pages += table[*it].rss_pages;
          pending.push_back(*it);
        }
      }
    }
    figures[r].rss_bytes = pages * page_bytes;
    const std::uint64_t key = KeyOf(root);
    if (const auto it = previous_.find(key);
        it != previous_.end() && it->second.start_time == top->start_time && now > it->second.at) {
      const double seconds = std::chrono::duration<double>(now - it->second.at).count();
      // A child reaped by something outside the tree takes its ticks with it, so
      // the sum can step back: that reads as idle, never as a negative share.
      const std::uint64_t spent = ticks > it->second.ticks ? ticks - it->second.ticks : 0;
      const double permille = static_cast<double>(spent) / static_cast<double>(ticks_per_second) / seconds * 1000.0;
      figures[r].cpu_permille = static_cast<std::int32_t>(std::min(permille + 0.5, 1e9));
    }
    next[key] = Previous{.start_time = top->start_time, .ticks = ticks, .at = now};
  }
  // Only what this sample asked for is kept: a pid that is gone, or that no row
  // names any more, is dropped rather than accumulated for the server's lifetime.
  previous_ = std::move(next);
  return figures;
}

}  // namespace microide::server
