#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

struct ProjectReplaceOutcome {
  enum class Status : std::uint8_t {
    NothingToDo,
    BlockedByDirty,
    CapExceeded,
    InvalidReplacement,
    Applied,
  };

  struct WrittenFile {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::size_t replacements = 0;
  };

  Status status = Status::NothingToDo;
  std::string blocked_relative_path;
  std::vector<WrittenFile> written;
  std::size_t failed_write_count = 0;
  std::string first_write_failure;

  std::filesystem::path project_root;
  std::uint64_t generation = 0;
};

}
