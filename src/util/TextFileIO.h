#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace microide::util {

inline constexpr std::uintmax_t kMaxTextFileBytes = 512ull * 1024 * 1024;

inline constexpr std::uintmax_t kMaxSearchFileBytes = 32ull * 1024 * 1024;

bool IsRegularFileFollowingSymlinks(const std::filesystem::path& path);

std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
enum class WriteTarget { Replace, CreateNew };
[[nodiscard]] bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text,
                                           std::error_code* error = nullptr,
                                           WriteTarget target = WriteTarget::Replace,
                                           const std::filesystem::path& staging = {});

std::string DescribeWriteError(const std::error_code& error);

std::filesystem::path ResolveSymlinkTarget(const std::filesystem::path& path);

std::filesystem::path ResolveRealPath(const std::filesystem::path& path);

std::vector<std::string> ReadFileLineWindow(const std::filesystem::path& path,
                                            std::size_t first_line, std::size_t last_line,
                                            std::uintmax_t max_bytes = kMaxTextFileBytes);

enum class TextFileReadStatus {
  Ok,
  Missing,
  Unreadable,
  Binary,
  TooLarge,
};

struct TextFileReadResult {
  TextFileReadStatus status = TextFileReadStatus::Missing;
  std::string content;

  bool ok() const { return status == TextFileReadStatus::Ok; }
  bool missing() const { return status == TextFileReadStatus::Missing; }
  bool is_error() const {
    return status == TextFileReadStatus::Unreadable || status == TextFileReadStatus::Binary ||
           status == TextFileReadStatus::TooLarge;
  }
};

TextFileReadResult ReadTextFileClassified(const std::filesystem::path& path);

struct FileSignature {
  bool exists = false;
  bool error = false;
  std::uint64_t mtime_ticks = 0;
  std::uintmax_t size = 0;

  bool has_content_hash = false;
  std::size_t content_hash = 0;

  bool racy = false;

  bool SameContentAs(const FileSignature& other) const {
    return SameStatAs(other) && !racy && !other.racy;
  }
  bool SameStatAs(const FileSignature& other) const {
    return exists && other.exists && !error && !other.error &&
           mtime_ticks == other.mtime_ticks && size == other.size;
  }
};

std::size_t QuickContentHash(std::string_view bytes);
std::uint64_t QuickContentHashProbe();

FileSignature StatFileSignature(const std::filesystem::path& path);

FileSignature SignatureForKnownContent(const std::filesystem::path& path,
                                       std::string_view bytes);

std::optional<std::uint64_t> FileModificationTick(const std::filesystem::path& path);

bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out,
                           std::uintmax_t max_bytes = kMaxSearchFileBytes, bool* had_utf8_bom = nullptr);

}
