#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>

#include "server/ListeningPorts.h"
#include "server/ProcessActivity.h"

namespace microide::server {

// Which project a listening port came from (remote-projects § 6.5, ports/*): the
// workspace root of the microide terminal or process whose tree holds the
// listening socket. Two projects open on one host share its ports watch, and a
// port announced with no owner raised its "Port 3000 is available" row in
// whichever project was in front -- news about the wrong project.
//
// Only microide's own trees are searched (tens of processes, not the box's
// thousands), and only for ports that are new since the last sample.
struct OwnerRoot {
  int pid = -1;      // a terminal's shell or a spawned process
  std::string root;  // the workspace root it was opened under; "" = none
};

// port -> root, for each port of `listeners` whose socket a process in one of
// `roots`' trees holds (any one of them, should several). Ports nobody here owns,
// and owners with an empty root, are absent.
std::map<std::uint16_t, std::string> AttributeListeners(std::span<const Listener> listeners,
                                                        std::span<const OwnerRoot> roots);

// The pure half, for tests: the index into `roots` of the tree `pid` is in,
// walking `parents` (pid -> ppid) up from it; -1 for none. Bounded, so a ppid
// cycle in a torn read cannot loop.
int OwningRootIndex(int pid, const std::unordered_map<int, int>& parents,
                    const std::unordered_map<int, std::size_t>& root_index);

}  // namespace microide::server
