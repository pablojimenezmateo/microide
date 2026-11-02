#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "project/remote/RemoteProtocol.h"

namespace microide::server {

// The TCP ports `uid` listens on that a forward to the host's `localhost:<port>`
// reaches — bound to a loopback address or to every address. Sorted, unique.

// One listening socket: its port and its inode -- the number /proc/<pid>/fd/N
// links name as `socket:[inode]`, which is what says which process holds it
// (ListenerOwners.h).
struct Listener {
  std::uint16_t port = 0;
  std::uint64_t inode = 0;
  friend auto operator<=>(const Listener&, const Listener&) = default;
};

// This user's such sockets now, IPv4 and IPv6 together, sorted by (port, inode)
// and unique (a dual-stack server holds one port twice); empty where neither
// source below is available. Sampled every 2 s while a client watches the host's
// ports.
//
// It asks the kernel's sock_diag netlink interface for LISTEN sockets only (what
// `ss -l` does). The kernel then walks the listening hash alone. /proc/net/tcp
// instead prints EVERY socket of the host, and its iterator walks the whole
// established-connection table, which is sized by RAM rather than by how many
// sockets exist: 8 ms of CPU per sample on an idle 64 GB machine with 23 sockets,
// which was the remote server's whole idle cost (dev-docs/project/known-tech-debt.md
// TD-2026-10-28-416). The procfs text stays as the fallback where netlink is
// refused (a seccomp profile, a sandbox without NETLINK_SOCK_DIAG).
std::vector<Listener> ListeningSockets();
// net.ipv4.ip_local_port_range as the kernel prints it ("32768\t60999"); nullopt for
// anything else. EphemeralPortRange reads this host's.
std::optional<project::remote::PortRange> ParsePortRange(std::string_view text);
std::optional<project::remote::PortRange> EphemeralPortRange();
// The ports of `listeners`, sorted and unique.
std::vector<std::uint16_t> PortsOf(std::span<const Listener> listeners);
// PortsOf(ListeningSockets()).
std::vector<std::uint16_t> ListeningPorts();

// The two sources on their own, for the test that holds them to the same answer.
// nullopt: that source is not available here.
std::optional<std::vector<Listener>> ListeningSocketsFromSockDiag();
std::optional<std::vector<Listener>> ListeningSocketsFromProc();

// The text of /proc/net/tcp or /proc/net/tcp6 (VS Code's port detection reads
// the same). Lines that do not parse are skipped. Sorted and unique.
std::vector<Listener> ParseListeningSockets(std::string_view proc_net_tcp, std::uint32_t uid);

// One reply buffer of a SOCK_DIAG_BY_FAMILY dump: a run of netlink messages, each
// an inet_diag_msg. Appends this user's forwardable listeners to `listeners`
// (unsorted); returns true once the dump's NLMSG_DONE is in it, false while more
// is to come. An NLMSG_ERROR or a truncated message ends the dump too (true),
// with what came before it kept.
bool ParseSockDiagListeners(std::span<const std::uint8_t> reply, std::uint32_t uid,
                            std::vector<Listener>& listeners);

}  // namespace microide::server
