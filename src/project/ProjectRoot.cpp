#include "project/ProjectRoot.h"

#include <filesystem>

namespace microide::project {

std::filesystem::path DetectProjectRoot(const std::filesystem::path& start) {
  std::error_code error;
  const std::filesystem::path requested_start = start.empty() ? std::filesystem::current_path(error) : start;
  if (error) {
    return start.lexically_normal();
  }

  std::filesystem::path absolute = std::filesystem::absolute(requested_start, error);
  if (error) {
    return requested_start.lexically_normal();
  }

  if (std::filesystem::is_regular_file(absolute, error)) {
    absolute = absolute.parent_path();
  }
  return absolute.lexically_normal();
}

}  // namespace microide::project
