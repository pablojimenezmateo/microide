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

inline constexpr std::uint32_t kInlineTextEndOfLine = UINT32_MAX;

enum DecorationStyleFlag : std::uint8_t {
  kDecorationUnderline = 1u << 0,
  kDecorationStrikethrough = 1u << 1,
  kDecorationBold = 1u << 2,
  kDecorationItalic = 1u << 3,
  kDecorationWholeLine = 1u << 4,
};

struct TextStyleDecoration {
  std::uint32_t line = 0;
  std::uint32_t start_column = 0;
  std::uint32_t end_column = 0;
  SDL_Color foreground{0, 0, 0, 0};
  SDL_Color background{0, 0, 0, 0};
  SDL_Color line_color{0, 0, 0, 0};
  std::uint8_t flags = 0;

  bool operator==(const TextStyleDecoration& o) const {
    return line == o.line && start_column == o.start_column && end_column == o.end_column &&
           SameSdlColor(foreground, o.foreground) && SameSdlColor(background, o.background) &&
           SameSdlColor(line_color, o.line_color) && flags == o.flags;
  }
};

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
  std::uint8_t priority = 0;

  bool operator==(const GutterMarkDecoration& o) const {
    return line == o.line && shape == o.shape && SameSdlColor(color, o.color) &&
           priority == o.priority;
  }
};

struct InlineTextDecoration {
  std::uint32_t line = 0;
  std::uint32_t anchor_column = kInlineTextEndOfLine;
  std::string text;
  SDL_Color color{};
  SDL_Color background{0, 0, 0, 0};

  bool operator==(const InlineTextDecoration& o) const {
    return line == o.line && anchor_column == o.anchor_column && text == o.text &&
           SameSdlColor(color, o.color) && SameSdlColor(background, o.background);
  }
};

struct CodeLensDecoration {
  std::uint32_t line = 0;
  std::string text;
  std::string command;
  std::uint64_t payload = 0;

  bool activatable() const { return payload != 0 || !command.empty(); }

  bool operator==(const CodeLensDecoration&) const = default;
};

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

struct FileDecorations {
  std::filesystem::path path;
  std::vector<TextStyleDecoration> text_styles;
  std::vector<GutterMarkDecoration> gutter_marks;
  std::vector<InlineTextDecoration> inline_texts;
  std::vector<CodeLensDecoration> code_lenses;

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

  const FileDecorations* FindByPath(const std::filesystem::path& path) const;
  const FileDecorations* FindByPathKey(std::string_view path_key) const;

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

}
