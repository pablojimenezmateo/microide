#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace microide::terminal {

// Text-attribute bits packed into TerminalStyle::attrs. Replaces the previous
// pair of `bold`/`inverse` bools so the full SGR attribute set fits without
// growing the cell (two uint8 flags -> one uint16 bitfield is memory-neutral
// and keeps TerminalCell trivially copyable for bulk snapshot/scrollback memcpy).
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
  // The cell is the trailing half of a double-width glyph. It carries no glyph
  // of its own; the renderer skips it so the wide lead cell paints across both
  // columns. Kept in the style so it travels with snapshot copies.
  kWideTrailing = 1u << 9,
  // The foreground was set via a basic SGR color (30..37) whose brightness must
  // track the bold flag: `\e[1m\e[31m` and `\e[31m\e[1m` both yield bright red,
  // and `\e[22m` reverts to dark red. Bits 11..13 hold the palette index (0..7)
  // so SGR 1/22 can re-resolve the RGB. Explicit bright colors (90..97) do NOT
  // set this bit, so `\e[91m…\e[22m` stays bright. Free bits, memory-neutral.
  kFgBasic = 1u << 10,
};
}  // namespace cell_attr

struct TerminalStyle {
  std::optional<SDL_Color> foreground;
  std::optional<SDL_Color> background;
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

  // Basic-palette foreground tracking (bits 11..13). See cell_attr::kFgBasic.
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
};

struct TerminalCell {
  // Inline UTF-8 storage. ASCII glyphs use length=1; multi-byte UTF-8 sequences fit in 2..4 bytes.
  // length=0 marks an empty/uninitialized cell. The cell is now trivially copyable, so terminal
  // snapshots and scrollback trims become bulk memcpys instead of per-cell std::string moves.
  // (Round-2 Finding 8.)
  std::array<char, 4> bytes{};
  std::uint8_t length = 0;
  TerminalStyle style;

  std::string_view DisplayText() const {
    return length == 0 ? std::string_view{} : std::string_view(bytes.data(), length);
  }

  // Convenience for callers that only care about the ASCII case (tests, scratch fixtures, etc.).
  // Returns '\0' for empty cells or for cells whose first byte is a UTF-8 lead byte.
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

struct TerminalLine {
  std::vector<TerminalCell> cells;
  bool wrapped_from_previous = false;
};

struct TerminalCursorSnapshot {
  std::size_t row = 0;
  std::size_t column = 0;
  bool visible = true;
};

struct TerminalLineRangeSnapshot {
  std::uint64_t generation = 0;
  std::vector<TerminalLine> lines;
};

}  // namespace microide::terminal
