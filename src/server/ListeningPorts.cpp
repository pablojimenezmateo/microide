#include "server/ListeningPorts.h"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

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

// The kernel prints an address as its 32-bit words in host (little-endian)
// order: 127.0.0.1 is 0100007F, ::1 is 00000000000000000000000001000000.
bool ForwardableAddress(std::string_view address) {
  if (address.find_first_not_of('0') == std::string_view::npos) {
    return true;  // 0.0.0.0 or ::
  }
  if (address.size() == 8) {
    return address.substr(6, 2) == "7F";  // 127.0.0.0/8
  }
  if (address.size() == 32) {
    if (address == "00000000000000000000000001000000") {
      return true;  // ::1
    }
    // ::ffff:127.x.y.z
    return address.substr(0, 16) == "0000000000000000" && address.substr(16, 8) == "FFFF0000" &&
           address.substr(30, 2) == "7F";
  }
  return false;
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

std::vector<std::uint16_t> ParseListeningPorts(std::string_view proc_net_tcp, std::uint32_t uid) {
  std::vector<std::uint16_t> ports;
  bool header = true;
  while (!proc_net_tcp.empty()) {
    const std::size_t end = std::min(proc_net_tcp.find('\n'), proc_net_tcp.size());
    std::string_view line = proc_net_tcp.substr(0, end);
    proc_net_tcp.remove_prefix(std::min(end + 1, proc_net_tcp.size()));
    if (std::exchange(header, false)) {
      continue;  // "  sl  local_address rem_address   st ..."
    }
    // sl local rem st tx:rx tr:when retrnsmt uid ...
    std::string_view fields[8];
    std::size_t count = 0;
    while (count < 8) {
      const std::size_t start = line.find_first_not_of(' ');
      if (start == std::string_view::npos) {
        break;
      }
      line.remove_prefix(start);
      const std::size_t stop = std::min(line.find(' '), line.size());
      fields[count++] = line.substr(0, stop);
      line.remove_prefix(stop);
    }
    if (count < 8 || fields[3] != "0A") {
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
    if (line_uid != uid || colon == std::string_view::npos || !ForwardableAddress(local.substr(0, colon))) {
      continue;
    }
    const std::optional<std::uint64_t> port = ParseHex(local.substr(colon + 1));
    if (port.has_value() && *port > 0 && *port <= 0xFFFF) {
      ports.push_back(static_cast<std::uint16_t>(*port));
    }
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
  return ports;
}

std::vector<std::uint16_t> ListeningPorts() {
  const std::uint32_t uid = static_cast<std::uint32_t>(::getuid());
  std::vector<std::uint16_t> ports;
  for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
    if (const std::optional<std::string> text = ReadProcTable(table)) {
      const std::vector<std::uint16_t> found = ParseListeningPorts(*text, uid);
      ports.insert(ports.end(), found.begin(), found.end());
    }
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
  return ports;
}

}  // namespace microide::server
