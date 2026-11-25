#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "project/remote/RemoteProtocol.h"
#include "util/ContentHash.h"

namespace microide::project::remote {

struct FileOpResult {
  enum class Status {
    Ok,
    Conflict,
    Error,
  };
  Status status = Status::Error;
  std::optional<util::ContentHash> current;
  std::string error;
  bool blocked = false;
  bool ok() const { return status == Status::Ok; }
};

FileOpResult ReadTreeFile(const std::filesystem::path& root, std::string_view path,
                               std::uint64_t max_bytes,
                               const std::function<void(std::string_view chunk)>& sink);

FileOpResult ReadHostFile(std::string_view path, std::uint64_t max_bytes,
                          const std::function<void(std::string_view chunk)>& sink);

enum class Durability { Synced, Deferred };

bool SyncTreeFilesystem(const std::filesystem::path& root);

std::optional<std::int64_t> TreeWriteTemporaryOwner(std::string_view name);
bool TreeWriteTemporaryIsAbandoned(std::string_view name);

FileOpResult WriteTreeFile(const std::filesystem::path& root, std::string_view path,
                                std::string_view content, const Precondition& expect,
                                std::optional<std::uint32_t> mode,
                                std::optional<std::int64_t> mtime_ns = std::nullopt,
                                Durability durability = Durability::Synced,
                                const std::filesystem::path& staging = {});

FileOpResult CopyFileIntoTree(const std::filesystem::path& root, std::string_view path,
                              const std::filesystem::path& source, const util::ContentHash& hash,
                              const Precondition& expect, std::optional<std::uint32_t> mode,
                              std::optional<std::int64_t> mtime_ns = std::nullopt,
                              Durability durability = Durability::Synced,
                              const std::filesystem::path& staging = {});

FileOpResult MakeTreeSymlink(const std::filesystem::path& root, std::string_view path,
                             std::string_view target);

FileOpResult MakeTreeDirectory(const std::filesystem::path& root, std::string_view path);
FileOpResult RenameTreeEntry(const std::filesystem::path& root, std::string_view from,
                                  std::string_view to, const Precondition& expect);
FileOpResult DeleteTreeEntry(const std::filesystem::path& root, std::string_view path,
                                  const Precondition& expect);
FileOpResult SetAsideTreeEntry(const std::filesystem::path& root, std::string_view path,
                               const Precondition& expect, const std::filesystem::path& aside,
                               bool* moved);
FileOpResult RestoreTreeEntry(const std::filesystem::path& root, std::string_view path,
                              const std::filesystem::path& aside);

}
