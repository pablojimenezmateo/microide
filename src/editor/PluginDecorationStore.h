#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::editor {

inline constexpr bool SameSdlColor(SDL_Color a, SDL_Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Sentinel `anchor_column` meaning "render at end of line".
inline constexpr std::uint32_t kInlineTextEndOfLine = UINT32_MAX;

// Style flags packed into TextStyleDecoration::flags.
enum DecorationStyleFlag : std::uint8_t {
  kDecorationUnderline = 1u << 0,
  kDecorationStrikethrough = 1u << 1,
  kDecorationBold = 1u << 2,
  kDecorationItalic = 1u << 3,
  kDecorationWholeLine = 1u << 4,  // ignore columns; style spans the whole line
};

// Per-range inline text styling. Drives RowFillSpan (background) + a syntax-run
// foreground recolor overlay + DecoratedUnderline (underline/strike). POD; the
// hot path reads these directly, never allocating.
struct TextStyleDecoration {
  std::uint32_t line = 0;          // 0-based logical line
  std::uint32_t start_column = 0;  // byte column (inclusive)
  std::uint32_t end_column = 0;    // byte column (exclusive); ignored when kDecorationWholeLine
  SDL_Color foreground{0, 0, 0, 0};  // a==0 => no foreground recolor
  SDL_Color background{0, 0, 0, 0};  // a==0 => no background fill
  SDL_Color line_color{0, 0, 0, 0};  // underline/strike color; a==0 => use foreground
  std::uint8_t flags = 0;

  bool operator==(const TextStyleDecoration& o) const {
    return line == o.line && start_column == o.start_column && end_column == o.end_column &&
           SameSdlColor(foreground, o.foreground) && SameSdlColor(background, o.background) &&
           SameSdlColor(line_color, o.line_color) && flags == o.flags;
  }
};

// Built-in gutter icon vocabulary. Matches the host's existing shape set
// (BreakpointRender / DiagnosticsRender) so no raster asset is needed in Phase A.
enum class GutterIconShape : std::uint8_t {
  Dot,
  Circle,
  Diamond,
  Triangle,
  Bookmark,
  Check,
  Dash,
  Square,
};

struct GutterMarkDecoration {
  std::uint32_t line = 0;
  GutterIconShape shape = GutterIconShape::Dot;
  SDL_Color color{};
  std::uint8_t priority = 0;  // higher wins the single plugin gutter slot when stacked

  bool operator==(const GutterMarkDecoration& o) const {
    return line == o.line && shape == o.shape && SameSdlColor(color, o.color) &&
           priority == o.priority;
  }
};

// End-of-line / inline virtual text (Error Lens message, GitLens blame).
struct InlineTextDecoration {
  std::uint32_t line = 0;
  std::uint32_t anchor_column = kInlineTextEndOfLine;
  std::string text;
  SDL_Color color{};
  SDL_Color background{0, 0, 0, 0};  // a==0 => none

  bool operator==(const InlineTextDecoration& o) const {
    return line == o.line && anchor_column == o.anchor_column && text == o.text &&
           SameSdlColor(color, o.color) && SameSdlColor(background, o.background);
  }
};

// Clickable line-level command indicator (rendered as an end-of-line affordance
// in Phase A/B; the above-line form lands with variable-height insets later).
struct CodeLensDecoration {
  std::uint32_t line = 0;
  std::string text;
  std::string command;  // command name dispatched on click

  bool operator==(const CodeLensDecoration&) const = default;
};

// One atomic publish unit for an (owner, path): a plugin replaces all of its
// decorations for a file in a single call so re-publish is cheap and consistent.
struct PluginDecorationData {
  std::vector<TextStyleDecoration> text_styles;
  std::vector<GutterMarkDecoration> gutter_marks;
  std::vector<InlineTextDecoration> inline_texts;
  std::vector<CodeLensDecoration> code_lenses;

  bool empty() const {
    return text_styles.empty() && gutter_marks.empty() && inline_texts.empty() &&
           code_lenses.empty();
  }
  bool operator==(const PluginDecorationData&) const = default;
};

// Merged-per-path view consumed by the renderer. Each kind is sorted by line
// (then a stable secondary key) at publish time so per-row resolution is a
// lower_bound + contiguous slice: O(visible decorations), not O(total).
struct FileDecorations {
  std::filesystem::path path;
  std::vector<TextStyleDecoration> text_styles;  // sorted by (line, start_column)
  std::vector<GutterMarkDecoration> gutter_marks;  // sorted by (line, -priority)
  std::vector<InlineTextDecoration> inline_texts;  // sorted by (line, anchor_column)
  std::vector<CodeLensDecoration> code_lenses;     // sorted by line

  // Contiguous decorations whose `.line == line`. Empty span when none.
  std::span<const TextStyleDecoration> TextStylesForLine(std::uint32_t line) const;
  std::span<const GutterMarkDecoration> GutterMarksForLine(std::uint32_t line) const;
  std::span<const InlineTextDecoration> InlineTextsForLine(std::uint32_t line) const;
  std::span<const CodeLensDecoration> CodeLensesForLine(std::uint32_t line) const;

  bool empty() const {
    return text_styles.empty() && gutter_marks.empty() && inline_texts.empty() &&
           code_lenses.empty();
  }
  bool operator==(const FileDecorations&) const = default;
};

// Owner/path-keyed store of plugin-published editor decorations, modeled on
// DiagnosticsStore: re-publish replaces an owner's contribution atomically, and
// a merged-per-path view is what the renderer reads. Mutators return a bool that
// is the redraw signal: true when the merged view actually changed, false for a
// no-op (e.g. an identical republish) so the host can skip a needless repaint.
class PluginDecorationStore {
 public:
  bool ReplaceForOwnerFile(std::string_view owner,
                           const std::filesystem::path& path,
                           PluginDecorationData data);
  bool ClearOwner(std::string_view owner);
  bool ClearOwnerFile(std::string_view owner, const std::filesystem::path& path);
  bool RetargetPathPrefix(const std::filesystem::path& old_prefix,
                          const std::filesystem::path& new_prefix);
  bool ClearPathPrefix(const std::filesystem::path& path_prefix);
  void Clear();

  // Merged decorations for `path`, or nullptr if none. The pointer and the
  // string_views/spans reachable through it stay valid until the next mutation.
  const FileDecorations* FindByPath(const std::filesystem::path& path) const;

  bool empty() const { return merged_by_path_.empty(); }

 private:
  struct OwnerFileDecorations {
    std::filesystem::path path;
    PluginDecorationData data;

    bool operator==(const OwnerFileDecorations&) const = default;
  };

  static std::string PathKey(const std::filesystem::path& path);
  void RebuildPath(std::string_view path_key);

  std::unordered_map<std::string,
                     std::unordered_map<std::string, OwnerFileDecorations,
                                        util::TransparentStringHash, std::equal_to<>>,
                     util::TransparentStringHash, std::equal_to<>>
      by_owner_;
  std::unordered_map<std::string, FileDecorations, util::TransparentStringHash, std::equal_to<>>
      merged_by_path_;
};

}  // namespace microide::editor
