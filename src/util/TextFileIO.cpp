#include "util/TextFileIO.h"

#include <cstring>
#include <fstream>
#include <system_error>

namespace microide::util {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return std::nullopt;
  }
  file.seekg(0, std::ios::beg);

  std::string content(static_cast<std::size_t>(size), '\0');
  if (!content.empty()) {
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
      return std::nullopt;
    }
  }
  return content;
}

bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return false;
  }
  file.seekg(0, std::ios::beg);

  // resize() keeps any capacity the caller already allocated, so reused buffers
  // only grow toward the largest file rather than reallocating per file.
  out.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    file.read(out.data(), static_cast<std::streamsize>(size));
    if (!file) {
      return false;
    }
    // Any embedded NUL means the file is binary; skip it. memchr bails at the
    // first NUL, so binaries with an early NUL cost only one scan up to it.
    if (std::memchr(out.data(), '\0', out.size()) != nullptr) {
      return false;
    }
  }
  return true;
}

bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text) {
  if (path.empty()) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  const std::filesystem::path temp_path = path.string() + ".tmp";
  std::filesystem::remove(temp_path, error);
  error.clear();

  {
    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) {
      file.close();
      std::filesystem::remove(temp_path, error);
      return false;
    }
  }

  std::filesystem::rename(temp_path, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temp_path, path, error);
  }
  if (error) {
    std::filesystem::remove(temp_path, error);
    return false;
  }
  return true;
}

}  // namespace microide::util
