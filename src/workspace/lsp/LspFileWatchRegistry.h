#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace {

enum class LspFileChangeType : int {
  Created = 1,
  Changed = 2,
  Deleted = 3,
};

inline constexpr int kLspWatchKindCreate = 1;
inline constexpr int kLspWatchKindChange = 2;
inline constexpr int kLspWatchKindDelete = 4;
inline constexpr int kLspWatchKindAll =
    kLspWatchKindCreate | kLspWatchKindChange | kLspWatchKindDelete;

class LspFileWatchRegistry {
 public:
  bool Register(const util::JsonValue& registration,
                const std::filesystem::path& project_root);

  bool Unregister(std::string_view id);


  bool empty() const { return registrations_.empty(); }
  std::size_t size() const { return registrations_.size(); }

  bool WantsChange(std::string_view relative_path,
                   std::string_view absolute_path,
                   LspFileChangeType change) const;

  std::size_t PatternCountForTesting() const;

 private:
  struct Registration {
    std::string id;
    std::vector<std::string> relative_patterns;
    std::vector<std::string> absolute_patterns;
    int kind = kLspWatchKindAll;
  };

  std::vector<Registration> registrations_;
};

inline constexpr std::size_t kMaxLspFileWatchRegistrations = 64;
inline constexpr std::size_t kMaxLspFileWatchPatternsPerRegistration = 128;

}
