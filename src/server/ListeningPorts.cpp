#include "server/ListeningPorts.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util/Parse.h"
#include "util/PosixFd.h"

namespace microide::server {
namespace {

std::optional<std::uint64_t> ParseHex(std::string_view text) {
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    int digit = 0;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return std::nullopt;
    }
    value = value * 16 + static_cast<std::uint64_t>(digit);
  }
  return value;
}

// Whether a listener bound to this local address is reached by a forward to the
// host's `localhost:<port>`: every address (0.0.0.0, ::), 127.0.0.0/8, ::1 or
// ::ffff:127.x.y.z. `address` is in network byte order, 4 or 16 bytes: the one
// rule for both sources, so they cannot disagree about which ports count.
bool ForwardableAddress(std::span<const std::uint8_t> address) {
  if (std::all_of(address.begin(), address.end(), [](std::uint8_t b) { return b == 0; })) {
    return true;  // 0.0.0.0 or ::
  }
  if (address.size() == 4) {
    return address[0] == 127;
  }
  if (address.size() != 16) {
    return false;
  }
  constexpr std::array<std::uint8_t, 16> kLoopback6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  if (std::equal(address.begin(), address.end(), kLoopback6.begin())) {
    return true;
  }
  // ::ffff:127.x.y.z
  return std::all_of(address.begin(), address.begin() + 10, [](std::uint8_t b) { return b == 0; }) &&
         address[10] == 0xFF && address[11] == 0xFF && address[12] == 127;
}

// The kernel prints an address as its 32-bit words in host order (`%08X` of each
// raw __be32): 127.0.0.1 is 0100007F on a little-endian host. Decoding each word
// and copying it back out recovers the network-order bytes on the host that
// printed them.
std::optional<std::vector<std::uint8_t>> ProcAddressBytes(std::string_view text) {
  if (text.size() != 8 && text.size() != 32) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(text.size() / 2);
  for (std::size_t word = 0; word * 8 < text.size(); ++word) {
    const std::optional<std::uint64_t> value = ParseHex(text.substr(word * 8, 8));
    if (!value.has_value()) {
      return std::nullopt;
    }
    const auto raw = static_cast<std::uint32_t>(*value);
    std::memcpy(bytes.data() + word * 4, &raw, sizeof(raw));
  }
  return bytes;
}

// A procfs table: it states size 0, so it is read to EOF (bounded: thousands of
// sockets are a few hundred KiB).
std::optional<std::string> ReadProcTable(const char* path) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::nullopt;
  }
  std::string text;
  char buffer[16384];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0 || text.size() > (8u << 20)) {
      break;
    }
    text.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return text;
}

}  // namespace

void SortUnique(std::vector<Listener>& listeners) {
  std::sort(listeners.begin(), listeners.end());
  listeners.erase(std::unique(listeners.begin(), listeners.end()), listeners.end());
}

std::vector<Listener> ParseListeningSockets(std::string_view proc_net_tcp, std::uint32_t uid) {
  std::vector<Listener> listeners;
  bool header = true;
  while (!proc_net_tcp.empty()) {
    const std::size_t end = std::min(proc_net_tcp.find('\n'), proc_net_tcp.size());
    std::string_view line = proc_net_tcp.substr(0, end);
    proc_net_tcp.remove_prefix(std::min(end + 1, proc_net_tcp.size()));
    if (std::exchange(header, false)) {
      continue;  // "  sl  local_address rem_address   st ..."
    }
    // sl local rem st tx:rx tr:when retrnsmt uid timeout inode ...
    std::string_view fields[10];
    std::size_t count = 0;
    while (count < 10) {
      const std::size_t start = line.find_first_not_of(' ');
      if (start == std::string_view::npos) {
        break;
      }
      line.remove_prefix(start);
      const std::size_t stop = std::min(line.find(' '), line.size());
      fields[count++] = line.substr(0, stop);
      line.remove_prefix(stop);
    }
    if (count < 10 || fields[3] != "0A") {
      continue;  // not a whole line, or not LISTEN
    }
    std::uint64_t line_uid = 0;
    for (const char c : fields[7]) {
      if (c < '0' || c > '9') {
        line_uid = ~std::uint64_t{0};
        break;
      }
      line_uid = line_uid * 10 + static_cast<std::uint64_t>(c - '0');
    }
    const std::string_view local = fields[1];
    const std::size_t colon = local.find(':');
    if (line_uid != uid || colon == std::string_view::npos) {
      continue;
    }
    const std::optional<std::vector<std::uint8_t>> address = ProcAddressBytes(local.substr(0, colon));
    if (!address.has_value() || !ForwardableAddress(*address)) {
      continue;
    }
    const std::optional<std::uint64_t> port = ParseHex(local.substr(colon + 1));
    const std::optional<std::int64_t> inode = util::ParseInt64(fields[9]);
    if (port.has_value() && *port > 0 && *port <= 0xFFFF && inode.has_value() && *inode >= 0) {
      listeners.push_back(Listener{.port = static_cast<std::uint16_t>(*port),
                                   .inode = static_cast<std::uint64_t>(*inode)});
    }
  }
  SortUnique(listeners);
  return listeners;
}

bool ParseSockDiagListeners(std::span<const std::uint8_t> reply, std::uint32_t uid,
                            std::vector<Listener>& listeners) {
  std::size_t offset = 0;
  while (reply.size() - offset >= sizeof(nlmsghdr)) {
    nlmsghdr header{};
    std::memcpy(&header, reply.data() + offset, sizeof(header));
    if (header.nlmsg_len < sizeof(nlmsghdr) || header.nlmsg_len > reply.size() - offset) {
      return true;  // truncated or malformed: nothing after it can be framed
    }
    if (header.nlmsg_type == NLMSG_DONE || header.nlmsg_type == NLMSG_ERROR) {
      return true;
    }
    if (header.nlmsg_type == SOCK_DIAG_BY_FAMILY &&
        header.nlmsg_len >= NLMSG_LENGTH(sizeof(inet_diag_msg))) {
      inet_diag_msg message{};
      std::memcpy(&message, reply.data() + offset + NLMSG_HDRLEN, sizeof(message));
      const std::size_t address_size = message.idiag_family == AF_INET6 ? 16 : 4;
      std::array<std::uint8_t, 16> address{};
      std::memcpy(address.data(), message.id.idiag_src, address_size);
      // Asked for LISTEN only; the state is checked anyway, as the text path does.
      if (message.idiag_state == TCP_LISTEN && message.idiag_uid == uid &&
          ForwardableAddress(std::span<const std::uint8_t>(address.data(), address_size))) {
        const std::uint16_t port = ntohs(message.id.idiag_sport);
        if (port != 0) {
          listeners.push_back(Listener{.port = port, .inode = message.idiag_inode});
        }
      }
    }
    offset += NLMSG_ALIGN(header.nlmsg_len);
  }
  return false;
}

std::optional<std::vector<Listener>> ListeningSocketsFromSockDiag() {
  const std::uint32_t uid = static_cast<std::uint32_t>(::getuid());
  util::UniqueFd socket(::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG));
  if (!socket.IsValid()) {
    return std::nullopt;
  }
  std::vector<Listener> listeners;
  // Sized for a dump message's worth: the kernel fills up to a page-sized skb per
  // read, and a smaller buffer truncates a datagram.
  std::vector<std::uint8_t> buffer(32 * 1024);
  std::uint32_t sequence = 0;
  for (const std::uint8_t family : {std::uint8_t{AF_INET}, std::uint8_t{AF_INET6}}) {
    struct {
      nlmsghdr header;
      inet_diag_req_v2 request;
    } message{};
    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    message.header.nlmsg_seq = ++sequence;
    message.request.sdiag_family = family;
    message.request.sdiag_protocol = IPPROTO_TCP;
    message.request.idiag_states = 1U << TCP_LISTEN;  // the kernel skips the established table
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (::sendto(socket.Get(), &message, sizeof(message), 0, reinterpret_cast<const sockaddr*>(&kernel),
                 sizeof(kernel)) != static_cast<ssize_t>(sizeof(message))) {
      return std::nullopt;
    }
    for (bool done = false; !done;) {
      const ssize_t got = ::recv(socket.Get(), buffer.data(), buffer.size(), 0);
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0) {
        return std::nullopt;  // a refused or broken dump: the caller reads /proc instead
      }
      done = ParseSockDiagListeners(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)),
                                    uid, listeners);
    }
  }
  SortUnique(listeners);
  return listeners;
}

std::optional<std::vector<Listener>> ListeningSocketsFromProc() {
  const std::uint32_t uid = static_cast<std::uint32_t>(::getuid());
  std::vector<Listener> listeners;
  bool any = false;
  for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
    if (const std::optional<std::string> text = ReadProcTable(table)) {
      any = true;
      const std::vector<Listener> found = ParseListeningSockets(*text, uid);
      listeners.insert(listeners.end(), found.begin(), found.end());
    }
  }
  if (!any) {
    return std::nullopt;
  }
  SortUnique(listeners);
  return listeners;
}

std::vector<Listener> ListeningSockets() {
  if (std::optional<std::vector<Listener>> listeners = ListeningSocketsFromSockDiag()) {
    return std::move(*listeners);
  }
  return ListeningSocketsFromProc().value_or(std::vector<Listener>{});
}

std::optional<project::remote::PortRange> ParsePortRange(std::string_view text) {
  const std::size_t gap = text.find_first_of(" \t");
  if (gap == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view second = text.substr(gap);
  while (!second.empty() && (second.front() == ' ' || second.front() == '\t')) {
    second.remove_prefix(1);
  }
  while (!second.empty() && (second.back() == '\n' || second.back() == ' ')) {
    second.remove_suffix(1);
  }
  const std::optional<int> low = util::ParseInt(text.substr(0, gap));
  const std::optional<int> high = util::ParseInt(second);
  if (!low.has_value() || !high.has_value() || *low < 1 || *low > *high || *high > 65535) {
    return std::nullopt;
  }
  return project::remote::PortRange{.low = static_cast<std::uint16_t>(*low), .high = static_cast<std::uint16_t>(*high)};
}

std::optional<project::remote::PortRange> EphemeralPortRange() {
  const util::UniqueFd fd(::open("/proc/sys/net/ipv4/ip_local_port_range", O_RDONLY | O_CLOEXEC));
  if (fd.Get() < 0) {
    return std::nullopt;
  }
  char buffer[64];
  const ssize_t count = ::read(fd.Get(), buffer, sizeof(buffer) - 1);
  return count > 0 ? ParsePortRange(std::string_view(buffer, static_cast<std::size_t>(count))) : std::nullopt;
}

std::vector<std::uint16_t> PortsOf(std::span<const Listener> listeners) {
  std::vector<std::uint16_t> ports;
  ports.reserve(listeners.size());
  for (const Listener& listener : listeners) {
    if (ports.empty() || ports.back() != listener.port) {  // sorted by port first
      ports.push_back(listener.port);
    }
  }
  return ports;
}

std::vector<std::uint16_t> ListeningPorts() { return PortsOf(ListeningSockets()); }

}  // namespace microide::server
