#include "platform/TransientFileStore.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>

#include "util/ContentHash.h"
#include "util/DurableFile.h"

namespace microide::platform {

std::optional<std::filesystem::path> StoreTransientFile(const std::filesystem::path& root,
                                                        const std::string& name, std::string_view content,
                                                        std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  struct stat info {};
  // Ours and a real directory: a link planted there must not redirect the write.
  if (::lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
    *error = "the directory " + root.string() + " is not usable";
    return std::nullopt;
  }
  ::chmod(root.c_str(), 0700);
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code age_error;
    const auto written = std::filesystem::last_write_time(it->path(), age_error);
    if (!age_error && now - written > std::chrono::hours(24)) {
      std::error_code ignored;
      std::filesystem::remove_all(it->path(), ignored);
    }
  }
  static std::atomic<std::uint64_t> counter{0};
  const std::string token = util::HashContent(name + "\n" + std::to_string(now.time_since_epoch().count()) + "\n" +
                                              std::to_string(counter.fetch_add(1)))
                                .Hex()
                                .substr(0, 16);
  const std::filesystem::path directory = root / token;
  if (::mkdir(directory.c_str(), 0700) != 0) {
    *error = "cannot create " + directory.string() + ": " + std::strerror(errno);
    return std::nullopt;
  }
  const std::filesystem::path path = directory / name;
  std::error_code write_error;
  if (!util::WriteFileBytesDurable(
          path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(content.data()), content.size()),
          &write_error)) {
    *error = "cannot write " + path.string() + ": " + write_error.message();
    return std::nullopt;
  }
  ::chmod(path.c_str(), 0600);
  return path;
}

}  // namespace microide::platform
