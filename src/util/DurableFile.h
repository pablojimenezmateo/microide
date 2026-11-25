#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <optional>
#include <span>

namespace microide::util {

bool WriteFileBytesDurable(const std::filesystem::path& path, std::span<const std::byte> bytes,
                           std::error_code* error = nullptr);

bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error = nullptr);

bool RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error = nullptr);

bool FsyncDirectory(const std::filesystem::path& directory);

inline constexpr std::string_view kTemporaryStagingMarker = ".microide-staging.";

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path);

bool IsTemporaryStagingFilename(std::string_view filename);

bool TemporaryStagingIsAbandoned(std::string_view filename);

inline constexpr std::chrono::minutes kAbandonedStagingMinAge{10};
bool SweepAbandonedTemporaryStaging(std::string_view absolute_path,
                                    std::chrono::seconds min_age = kAbandonedStagingMinAge);

struct FilePermissions {
  bool valid = false;
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
};

FilePermissions CaptureFilePermissions(const std::filesystem::path& path);

void ApplyFilePermissions(const std::filesystem::path& path, const FilePermissions& permissions);

}
