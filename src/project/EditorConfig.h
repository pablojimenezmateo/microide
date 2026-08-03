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

// The subset of EditorConfig properties that map onto a setting microide
// actually has. Everything else in a `.editorconfig` is parsed and ignored
// rather than treated as an error, which is what the EditorConfig spec asks of
// a conforming reader.
//
// `charset` is deliberately not here: the editor writes UTF-8 unconditionally
// (TextViewport's encoding model is ASCII/UTF-8/Bytes), so honoring
// `charset = latin1` would be a lie. `max_line_length` is parsed but unused
// until there is a ruler to drive.
struct EditorConfigProperties {
  // indent_style = space | tab
  std::optional<bool> soft_tabs;
  // indent_size = <n> | tab. "tab" resolves to tab_width during resolution, so a
  // consumer never has to re-implement that rule.
  std::optional<int> indent_width;
  // tab_width = <n>. Defaults to indent_size when absent, per the spec.
  std::optional<int> tab_size;
  // end_of_line = lf | crlf | cr
  std::optional<util::LineEnding> line_ending;
  // trim_trailing_whitespace = true | false
  std::optional<bool> trim_trailing_whitespace;
  // insert_final_newline = true | false
  std::optional<bool> insert_final_newline;
  // max_line_length = <n> | off. Parsed for completeness; no consumer yet.
  std::optional<int> max_line_length;

  bool any() const {
    return soft_tabs.has_value() || indent_width.has_value() || tab_size.has_value() ||
           line_ending.has_value() || trim_trailing_whitespace.has_value() ||
           insert_final_newline.has_value() || max_line_length.has_value();
  }

  // Overlay `other` on top of this, with `other` winning where it has a value.
  // Used to fold a chain of .editorconfig files from the outermost inward.
  void MergeOver(const EditorConfigProperties& other);
};

// One parsed `.editorconfig` file. Sections keep source order because later
// matching sections override earlier ones.
struct EditorConfigFile {
  // `root = true` in the preamble stops the upward search at this file.
  bool root = false;

  struct Section {
    // Brace alternation is expanded at parse time (GlobMatches has no brace
    // support), and each entry is already normalized to match against a path
    // relative to the .editorconfig's own directory.
    std::vector<std::string> patterns;
    EditorConfigProperties properties;
  };

  std::vector<Section> sections;
};

// Parse `.editorconfig` text. Never fails: unknown properties, malformed values,
// and unterminated sections are skipped, matching the spec's "ignore what you do
// not understand" rule. Input is capped (see kMaxEditorConfigBytes) by the
// caller reading the file.
EditorConfigFile ParseEditorConfig(std::string_view text);

// Largest `.editorconfig` we will read. These files are configuration, not data;
// anything past this is not a config file we should be applying to every buffer.
inline constexpr std::size_t kMaxEditorConfigBytes = 256 * 1024;

// Upper bound on sections retained from one file, so a pathological file cannot
// turn every buffer's property resolution into an unbounded match loop.
inline constexpr std::size_t kMaxEditorConfigSections = 256;

// Resolves EditorConfig properties for a file, caching both the parsed
// `.editorconfig` files and the per-path result.
//
// Why the cache is load-bearing: ApplyEditorPreferences runs for every open tab
// in every editor group on every settings change, project activation, and session
// restore. Doing an uncached parent-directory walk with a file read per level
// there would put filesystem I/O on a path that currently touches no disk at all.
// After warm-up a resolve is one hash lookup.
//
// Not thread-safe: owned per project and used from the shell thread.
class EditorConfigResolver {
 public:
  // Files outside `project_root` are not resolved (the upward walk stops there),
  // so a stray `.editorconfig` in /home cannot reconfigure an unrelated project.
  void SetProjectRoot(std::filesystem::path project_root);
  const std::filesystem::path& project_root() const { return project_root_; }

  // Self-heal for callers that hold an already-normalized project root and cannot
  // be sure this resolver was ever pointed at it — a project-catalog slot that was
  // reset to a fresh state carries its root but a default-constructed resolver, and
  // an unset root resolves to "no opinion" *silently*. Allocation-free on the hot
  // path: it is a path compare, and only a genuine mismatch does any work.
  void EnsureProjectRoot(const std::filesystem::path& normalized_project_root) const;

  // Resolve for an absolute file path. Returns an all-empty property set when no
  // `.editorconfig` applies, which is the common case and costs one hash lookup
  // once warm.
  const EditorConfigProperties& Resolve(const std::filesystem::path& absolute_path) const;

  // Drop every cached parse and result. Called when a `.editorconfig` changes on
  // disk or the project root changes.
  void Invalidate() const;

  // True once a resolve has found at least one `.editorconfig` in this project.
  // Lets a caller skip re-applying preferences after an invalidation that cannot
  // have changed anything.
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
  // Mutable because resolution is a pure query with a memo behind it: callers
  // (ApplyEditorPreferences) are const and should not have to care.
  mutable std::unordered_map<std::string, DirectoryEntry> directories_;
  // Heterogeneous lookup (util::TransparentStringHash + std::equal_to<>), so a
  // memo hit never materializes the key. Resolve() runs once per open tab on
  // every preference application, and building a std::string from the path just
  // to hash it put an allocation on that path for every tab of every settings
  // change. The lookup now takes a string_view straight off the path's native
  // storage; only an insert allocates.
  mutable std::unordered_map<std::string, EditorConfigProperties, util::TransparentStringHash,
                             std::equal_to<>>
      resolved_;
  mutable bool found_any_config_ = false;
};

// Cap on memoized per-path results. Past it the memo is cleared rather than
// grown: a project can have more paths than tabs (compare/merge views resolve
// too), and an unbounded map here would be a slow leak across a long session.
inline constexpr std::size_t kMaxEditorConfigResolvedPaths = 4096;

}  // namespace microide::project
