#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "util/Rgba8.h"

namespace microide::terminal {

namespace cell_attr {
enum Bit : std::uint16_t {
  kBold = 1u << 0,
  kDim = 1u << 1,
  kItalic = 1u << 2,
  kUnderline = 1u << 3,
  kDoubleUnderline = 1u << 4,
  kBlink = 1u << 5,
  kInverse = 1u << 6,
  kHidden = 1u << 7,
  kStrikethrough = 1u << 8,
  kWideTrailing = 1u << 9,
  kFgBasic = 1u << 10,
};
}

enum class TerminalUnderlineVariant : std::uint8_t {
  Straight = 0,
  Curly = 1,
  Dotted = 2,
  Dashed = 3,
};

class TerminalColor {
 public:
  constexpr TerminalColor() = default;
  constexpr TerminalColor(std::nullopt_t) {}  // NOLINT(google-explicit-constructor)
  constexpr TerminalColor(util::Rgba8 color)  // NOLINT(google-explicit-constructor)
      : value_{color.r, color.g, color.b, 0xff} {}

  constexpr bool has_value() const { return value_.a != 0; }
  constexpr explicit operator bool() const { return has_value(); }
  constexpr const util::Rgba8& operator*() const { return value_; }
  constexpr const util::Rgba8* operator->() const { return &value_; }
  constexpr util::Rgba8 value_or(util::Rgba8 fallback) const {
    return has_value() ? value_ : fallback;
  }
  constexpr void reset() { value_ = kDefault; }

  friend constexpr bool operator==(const TerminalColor&, const TerminalColor&) = default;

 private:
  static constexpr util::Rgba8 kDefault{0, 0, 0, 0};
  util::Rgba8 value_ = kDefault;
};

struct TerminalStyle {
  TerminalColor foreground;
  TerminalColor background;
  std::uint16_t attrs = 0;

  constexpr bool has(std::uint16_t bit) const { return (attrs & bit) != 0; }
  void set(std::uint16_t bit, bool on) {
    if (on) {
      attrs |= bit;
    } else {
      attrs &= static_cast<std::uint16_t>(~bit);
    }
  }

  constexpr bool bold() const { return has(cell_attr::kBold); }
  constexpr bool dim() const { return has(cell_attr::kDim); }
  constexpr bool italic() const { return has(cell_attr::kItalic); }
  constexpr bool underline() const { return has(cell_attr::kUnderline); }
  constexpr bool double_underline() const { return has(cell_attr::kDoubleUnderline); }
  constexpr bool blink() const { return has(cell_attr::kBlink); }
  constexpr bool inverse() const { return has(cell_attr::kInverse); }
  constexpr bool hidden() const { return has(cell_attr::kHidden); }
  constexpr bool strikethrough() const { return has(cell_attr::kStrikethrough); }
  constexpr bool wide_trailing() const { return has(cell_attr::kWideTrailing); }

  static constexpr std::uint16_t kFgBasicIndexShift = 11;
  static constexpr std::uint16_t kFgBasicIndexMask = static_cast<std::uint16_t>(0x7u << 11);
  constexpr bool has_basic_foreground() const { return has(cell_attr::kFgBasic); }
  constexpr int basic_foreground_index() const {
    return (attrs & kFgBasicIndexMask) >> kFgBasicIndexShift;
  }
  void set_basic_foreground(int index) {
    attrs = static_cast<std::uint16_t>((attrs & ~kFgBasicIndexMask) |
                                       (static_cast<std::uint16_t>(index & 0x7) << kFgBasicIndexShift));
    set(cell_attr::kFgBasic, true);
  }
  void clear_basic_foreground() { set(cell_attr::kFgBasic, false); }

  static constexpr std::uint16_t kUnderlineVariantShift = 14;
  static constexpr std::uint16_t kUnderlineVariantMask = static_cast<std::uint16_t>(0x3u << 14);
  constexpr TerminalUnderlineVariant underline_variant() const {
    return static_cast<TerminalUnderlineVariant>((attrs & kUnderlineVariantMask) >> kUnderlineVariantShift);
  }
  void set_underline_variant(TerminalUnderlineVariant variant) {
    attrs = static_cast<std::uint16_t>((attrs & ~kUnderlineVariantMask) |
                                       (static_cast<std::uint16_t>(variant) << kUnderlineVariantShift));
  }
};

struct TerminalCell {
  std::array<char, 5> bytes{};
  std::uint8_t length = 0;
  TerminalStyle style;
  std::uint16_t extra = 0;

  std::string_view DisplayText() const {
    return length == 0 ? std::string_view{} : std::string_view(bytes.data(), length);
  }

  char ascii_character() const {
    if (length == 1) {
      return bytes[0];
    }
    return '\0';
  }

  void SetAscii(char c) {
    bytes[0] = c;
    length = 1;
  }

  void SetUtf8(std::string_view glyph) {
    const std::size_t copy_len = std::min<std::size_t>(glyph.size(), bytes.size());
    for (std::size_t i = 0; i < copy_len; ++i) {
      bytes[i] = glyph[i];
    }
    length = static_cast<std::uint8_t>(copy_len);
  }
};

inline bool SameCell(const TerminalCell& a, const TerminalCell& b) {
  if (a.length != b.length || a.extra != b.extra || a.style.attrs != b.style.attrs ||
      a.style.foreground != b.style.foreground || a.style.background != b.style.background) {
    return false;
  }
  for (std::uint8_t i = 0; i < a.length; ++i) {
    if (a.bytes[i] != b.bytes[i]) {
      return false;
    }
  }
  return true;
}

static_assert(sizeof(TerminalCell) == 18, "a terminal cell is 18 bytes; see TerminalColor");

struct TerminalLine {
  std::vector<TerminalCell> cells;
  bool wrapped_from_previous = false;
};

inline bool SameLine(const TerminalLine& a, const TerminalLine& b) {
  if (a.wrapped_from_previous != b.wrapped_from_previous || a.cells.size() != b.cells.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.cells.size(); ++i) {
    if (!SameCell(a.cells[i], b.cells[i])) {
      return false;
    }
  }
  return true;
}

enum class TerminalCursorShape : std::uint8_t {
  Block,
  Underline,
  Bar,
};

struct TerminalCursorSnapshot {
  std::size_t row = 0;
  std::size_t column = 0;
  bool visible = true;
  TerminalCursorShape shape = TerminalCursorShape::Block;
  bool blinking = true;
  bool wide = false;
  friend bool operator==(const TerminalCursorSnapshot&, const TerminalCursorSnapshot&) = default;
};

struct TerminalUnderlineColor {
  std::uint16_t extra = 0;
  TerminalColor color;
};

inline TerminalColor FindUnderlineColor(std::span<const TerminalUnderlineColor> colors,
                                        std::uint16_t extra) {
  if (extra == 0 || colors.empty()) {
    return {};
  }
  const auto found = std::lower_bound(
      colors.begin(), colors.end(), extra,
      [](const TerminalUnderlineColor& entry, std::uint16_t id) { return entry.extra < id; });
  return found != colors.end() && found->extra == extra ? found->color : TerminalColor{};
}

struct TerminalLineRangeSnapshot {
  std::uint64_t generation = 0;
  std::vector<TerminalLine> lines;
  std::vector<TerminalUnderlineColor> underline_colors;
};

}
