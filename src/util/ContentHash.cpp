#include "util/ContentHash.h"

#include <blake3.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include "util/Hex.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::util {
namespace {

// The official BLAKE3 C implementation (third_party/blake3), held in the opaque
// storage the header declares so blake3.h stays out of every TU that names a hash.
static_assert(sizeof(blake3_hasher) <= sizeof(ContentHasher), "grow ContentHasher::state_");
static_assert(alignof(blake3_hasher) <= 8, "raise ContentHasher::state_'s alignment");

blake3_hasher* State(unsigned char* storage) {
  return std::launder(reinterpret_cast<blake3_hasher*>(storage));
}
const blake3_hasher* State(const unsigned char* storage) {
  return std::launder(reinterpret_cast<const blake3_hasher*>(storage));
}

}  // namespace

std::optional<ContentHash> ContentHash::FromRaw(std::string_view raw) {
  if (raw.size() != kBytes) {
    return std::nullopt;
  }
  ContentHash hash;
  std::memcpy(hash.bytes.data(), raw.data(), kBytes);
  return hash;
}

std::string ContentHash::Hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(kBytes * 2, '\0');
  for (std::size_t i = 0; i < kBytes; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

std::optional<ContentHash> ContentHash::FromHex(std::string_view hex) {
  if (hex.size() != kBytes * 2) {
    return std::nullopt;
  }
  ContentHash hash;
  for (std::size_t i = 0; i < kBytes; ++i) {
    const auto byte = ParseHexByte(hex[2 * i], hex[2 * i + 1]);
    if (!byte) {
      return std::nullopt;
    }
    hash.bytes[i] = *byte;
  }
  return hash;
}

ContentHasher::ContentHasher() {
  blake3_hasher_init(new (state_) blake3_hasher);
}

void ContentHasher::Update(std::string_view bytes) {
  blake3_hasher_update(State(state_), bytes.data(), bytes.size());
}

ContentHash ContentHasher::Finish() const {
  ContentHash hash;
  blake3_hasher_finalize(State(state_), hash.bytes.data(), hash.bytes.size());
  return hash;
}

ContentHash HashContent(std::string_view bytes) {
  ContentHasher hasher;
  hasher.Update(bytes);
  return hasher.Finish();
}

std::optional<ContentHash> HashFileDescriptor(int fd, std::uint64_t* size_out,
                                              const std::function<void(std::string_view)>& sink) {
#if defined(__unix__) || defined(__APPLE__)
  ContentHasher hasher;
  std::uint64_t total = 0;
  char buffer[64 * 1024];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got > 0) {
      const std::string_view chunk(buffer, static_cast<std::size_t>(got));
      hasher.Update(chunk);
      if (sink) {
        sink(chunk);
      }
      total += static_cast<std::uint64_t>(got);
      continue;
    }
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      return std::nullopt;
    }
    break;
  }
  if (size_out != nullptr) {
    *size_out = total;
  }
  return hasher.Finish();
#else
  (void)fd;
  (void)size_out;
  (void)sink;
  return std::nullopt;
#endif
}

std::optional<ContentHash> HashFileContent(const std::filesystem::path& path,
                                           std::uint64_t* size_out) {
#if defined(__unix__) || defined(__APPLE__)
  // O_NONBLOCK so opening a FIFO cannot hang; the fstat below then refuses it.
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return std::nullopt;
  }
  struct stat info {};
  std::optional<ContentHash> hash;
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
    hash = HashFileDescriptor(fd, size_out);
  }
  ::close(fd);
  return hash;
#else
  (void)path;
  (void)size_out;
  return std::nullopt;
#endif
}

}  // namespace microide::util
