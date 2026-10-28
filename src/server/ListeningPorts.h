#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace microide::server {

// The TCP ports `uid` listens on that a forward to the host's `localhost:<port>`
// reaches — bound to a loopback address or to every address. Sorted, unique.

// This user's such ports now, IPv4 and IPv6 together; empty where neither source
// below is available. Sampled every 2 s while a client watches the host's ports.
//
// It asks the kernel's sock_diag netlink interface for LISTEN sockets only (what
// `ss -l` does). The kernel then walks the listening hash alone. /proc/net/tcp
// instead prints EVERY socket of the host, and its iterator walks the whole
// established-connection table, which is sized by RAM rather than by how many
// sockets exist: 8 ms of CPU per sample on an idle 64 GB machine with 23 sockets,
// which was the remote server's whole idle cost (dev-docs/project/known-tech-debt.md
// TD-2026-10-28-416). The procfs text stays as the fallback where netlink is
// refused (a seccomp profile, a sandbox without NETLINK_SOCK_DIAG).
std::vector<std::uint16_t> ListeningPorts();

// The two sources on their own, for the test that holds them to the same answer.
// nullopt: that source is not available here.
std::optional<std::vector<std::uint16_t>> ListeningPortsFromSockDiag();
std::optional<std::vector<std::uint16_t>> ListeningPortsFromProc();

// The text of /proc/net/tcp or /proc/net/tcp6 (VS Code's port detection reads
// the same). Lines that do not parse are skipped.
std::vector<std::uint16_t> ParseListeningPorts(std::string_view proc_net_tcp, std::uint32_t uid);

// One reply buffer of a SOCK_DIAG_BY_FAMILY dump: a run of netlink messages, each
// an inet_diag_msg. Appends this user's forwardable ports to `ports` (unsorted);
// returns true once the dump's NLMSG_DONE is in it, false while more is to come.
// An NLMSG_ERROR or a truncated message ends the dump too (true), with what came
// before it kept.
bool ParseSockDiagListeners(std::span<const std::uint8_t> reply, std::uint32_t uid,
                            std::vector<std::uint16_t>& ports);

}  // namespace microide::server
