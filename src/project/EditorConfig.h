#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/StringUtil.h"
#include "util/TransparentStringHash.h"

namespace microide::project {

struct EditorConfigProperties {
  std::optional<bool> soft_tabs;
  std::optional<int> indent_width;
  std::optional<int> tab_size;
  std::optional<util::LineEnding> line_ending;
  std::optional<bool> trim_trailing_whitespace;
  std::optional<bool> insert_final_newline;
  std::optional<int> max_line_length;
  enum UnsetKey : unsigned {
    kUnsetIndentStyle = 1u << 0,
    kUnsetIndentSize = 1u << 1,
    kUnsetTabWidth = 1u << 2,
    kUnsetEndOfLine = 1u << 3,
    kUnsetTrimTrailingWhitespace = 1u << 4,
    kUnsetInsertFinalNewline = 1u << 5,
    kUnsetMaxLineLength = 1u << 6,
  };
  unsigned unset = 0;

  bool any() const {
    return soft_tabs.has_value() || indent_width.has_value() || tab_size.has_value() ||
           line_ending.has_value() || trim_trailing_whitespace.has_value() ||
           insert_final_newline.has_value() || max_line_length.has_value() || unset != 0;
  }

  void MergeOver(const EditorConfigProperties& other);
};

struct EditorConfigFile {
  bool root = false;

  struct Section {
    std::vector<std::string> patterns;
    EditorConfigProperties properties;
  };

  std::vector<Section> sections;
};

EditorConfigFile ParseEditorConfig(std::string_view text);

inline constexpr std::size_t kMaxEditorConfigBytes = 256 * 1024;

inline constexpr std::size_t kMaxEditorConfigSections = 256;
inline constexpr std::size_t kMaxEditorConfigSectionNameBytes = 4096;

class EditorConfigResolver {
 public:
  void SetProjectRoot(std::filesystem::path project_root);
  const std::filesystem::path& project_root() const { return project_root_; }

  void EnsureProjectRoot(const std::filesystem::path& normalized_project_root) const;

  const EditorConfigProperties& Resolve(const std::filesystem::path& absolute_path) const;

  void Invalidate() const;

  bool FoundAnyConfig() const { return found_any_config_; }

  std::size_t CachedPathCountForTesting() const { return resolved_.size(); }
  std::size_t CachedDirectoryCountForTesting() const { return directories_.size(); }

 private:
  struct DirectoryEntry {
    bool has_config = false;
    EditorConfigFile config;
  };

  const DirectoryEntry& EntryForDirectory(const std::filesystem::path& directory) const;

  mutable std::filesystem::path project_root_;
  mutable std::unordered_map<std::string, DirectoryEntry> directories_;
  mutable std::unordered_map<std::string, EditorConfigProperties, util::TransparentStringHash,
                             std::equal_to<>>
      resolved_;
  mutable bool found_any_config_ = false;
};

inline constexpr std::size_t kMaxEditorConfigResolvedPaths = 4096;

}
