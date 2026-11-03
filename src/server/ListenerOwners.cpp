#include "server/ListenerOwners.h"

#include <cstdio>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include "util/Parse.h"

namespace microide::server {
namespace {

// The socket inodes `pid` holds open: its /proc/<pid>/fd links that read
// `socket:[N]`. Another user's process (or one gone since it was listed) has
// none to read.
template <typename Visit>
void ForEachSocketInode(int pid, Visit&& visit) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/fd", pid);
  DIR* fds = ::opendir(path);
  if (fds == nullptr) {
    return;
  }
  char link[64 + sizeof(dirent::d_name)];  // "/proc/<pid>/fd/" + any entry name
  char target[64];
  while (const dirent* entry = ::readdir(fds)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    std::snprintf(link, sizeof(link), "/proc/%d/fd/%s", pid, entry->d_name);
    const ssize_t got = ::readlink(link, target, sizeof(target));
    if (got <= 0 || got >= static_cast<ssize_t>(sizeof(target))) {
      continue;
    }
    const std::string_view text(target, static_cast<std::size_t>(got));
    constexpr std::string_view kPrefix = "socket:[";
    if (!text.starts_with(kPrefix) || !text.ends_with(']')) {
      continue;
    }
    const std::optional<std::int64_t> inode =
        util::ParseInt64(text.substr(kPrefix.size(), text.size() - kPrefix.size() - 1));
    if (inode.has_value() && *inode > 0) {
      visit(static_cast<std::uint64_t>(*inode));
    }
  }
  ::closedir(fds);
}

}  // namespace

int OwningRootIndex(int pid, const std::unordered_map<int, int>& parents,
                    const std::unordered_map<int, std::size_t>& root_index) {
  // Deeper than any real chain (shell -> make -> sh -> node ...), and a bound on
  // a cycle a torn read could produce.
  constexpr int kMaxDepth = 64;
  for (int depth = 0; depth < kMaxDepth && pid > 0; ++depth) {
    if (const auto root = root_index.find(pid); root != root_index.end()) {
      return static_cast<int>(root->second);
    }
    const auto parent = parents.find(pid);
    if (parent == parents.end()) {
      return -1;
    }
    pid = parent->second;
  }
  return -1;
}

std::map<std::uint16_t, std::string> AttributeListeners(std::span<const Listener> listeners,
                                                        std::span<const OwnerRoot> roots) {
  std::map<std::uint16_t, std::string> owners;
  if (listeners.empty() || roots.empty()) {
    return owners;
  }
  std::unordered_map<std::uint64_t, std::uint16_t> port_of_inode;
  for (const Listener& listener : listeners) {
    port_of_inode.emplace(listener.inode, listener.port);
  }
  std::vector<ProcessActivity::Root> trees;
  std::unordered_map<int, std::size_t> root_index;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].pid > 0 && !roots[i].root.empty() && root_index.emplace(roots[i].pid, i).second) {
      trees.push_back(ProcessActivity::Root{.pid = roots[i].pid, .subtree = true});
    }
  }
  if (trees.empty()) {
    return owners;
  }
  const std::vector<ProcessActivity::Stat> table = ProcessActivity::ReadTable(trees);
  std::unordered_map<int, int> parents;
  parents.reserve(table.size());
  for (const ProcessActivity::Stat& stat : table) {
    parents.emplace(stat.pid, stat.ppid);
  }
  for (const ProcessActivity::Stat& stat : table) {
    const int index = OwningRootIndex(stat.pid, parents, root_index);
    if (index < 0) {
      continue;  // the fallback table holds the whole box; most of it is not ours
    }
    ForEachSocketInode(stat.pid, [&](std::uint64_t inode) {
      if (const auto port = port_of_inode.find(inode); port != port_of_inode.end()) {
        owners.emplace(port->second, roots[static_cast<std::size_t>(index)].root);
      }
    });
  }
  return owners;
}

}  // namespace microide::server
