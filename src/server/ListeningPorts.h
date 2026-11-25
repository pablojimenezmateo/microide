#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "project/remote/RemoteProtocol.h"

namespace microide::server {

struct Listener {
  std::uint16_t port = 0;
  std::uint64_t inode = 0;
  friend auto operator<=>(const Listener&, const Listener&) = default;
};

std::vector<Listener> ListeningSockets();
std::optional<project::remote::PortRange> ParsePortRange(std::string_view text);
std::optional<project::remote::PortRange> EphemeralPortRange();
std::vector<std::uint16_t> PortsOf(std::span<const Listener> listeners);
std::vector<std::uint16_t> ListeningPorts();

std::optional<std::vector<Listener>> ListeningSocketsFromSockDiag();
std::optional<std::vector<Listener>> ListeningSocketsFromProc();

std::vector<Listener> ParseListeningSockets(std::string_view proc_net_tcp, std::uint32_t uid);

bool ParseSockDiagListeners(std::span<const std::uint8_t> reply, std::uint32_t uid,
                            std::vector<Listener>& listeners);

}
