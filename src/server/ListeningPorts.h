#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace microide::server {

// The TCP ports `uid` listens on that a forward to the host's `localhost:<port>`
// reaches — bound to a loopback address or to every address — from the text of
// /proc/net/tcp or /proc/net/tcp6 (VS Code's port detection reads the same).
// Sorted, unique. Lines that do not parse are skipped.
std::vector<std::uint16_t> ParseListeningPorts(std::string_view proc_net_tcp, std::uint32_t uid);

// This user's such ports now, IPv4 and IPv6 together; empty where /proc is not.
std::vector<std::uint16_t> ListeningPorts();

}  // namespace microide::server
