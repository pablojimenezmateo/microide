#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalInput.h"

namespace microide::terminal {

struct TerminalHostRun {
  enum class Kind : std::uint8_t { Promote, Lines, Gap };
  Kind kind = Kind::Lines;
  std::uint64_t count = 0;
};

struct TerminalHostCellExtra {
  std::uint16_t id = 0;
  std::string uri;
  TerminalColor underline_color;
};

struct TerminalHostNotification {
  std::string title;
  std::string body;
};

struct TerminalHostEdit {
  std::uint64_t token = 0;
  std::string path;
  std::uint32_t line = 0;
  std::string text;
  bool operator==(const TerminalHostEdit&) const = default;
};

struct TerminalHostFrame {
  static constexpr std::uint16_t kAlternateScreen = 1u << 0;
  static constexpr std::uint16_t kCursorVisible = 1u << 1;
  static constexpr std::uint16_t kCursorBlinking = 1u << 2;
  static constexpr std::uint16_t kMouseNormal = 1u << 3;
  static constexpr std::uint16_t kMouseDrag = 1u << 4;
  static constexpr std::uint16_t kMouseAny = 1u << 5;
  static constexpr std::uint16_t kFocusEvents = 1u << 6;
  static constexpr std::uint16_t kRunning = 1u << 7;
  static constexpr std::uint16_t kBell = 1u << 8;
  static constexpr std::uint16_t kReset = 1u << 9;
  static constexpr std::uint16_t kReverseVideo = 1u << 10;
  static constexpr unsigned kPointerShapeShift = 11;
  static constexpr std::uint16_t kPointerShapeMask = 0x1fu << kPointerShapeShift;
  TerminalPointerShape pointer_shape() const {
    const auto value = static_cast<std::uint8_t>((flags & kPointerShapeMask) >> kPointerShapeShift);
    return value < kTerminalPointerShapeCount ? static_cast<TerminalPointerShape>(value) : TerminalPointerShape::None;
  }
  static std::uint16_t PointerShapeFlags(TerminalPointerShape shape) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(shape) << kPointerShapeShift) & kPointerShapeMask;
  }

  std::uint32_t rows = 24;
  std::uint32_t columns = 80;
  std::uint16_t flags = kCursorVisible | kCursorBlinking | kRunning;
  TerminalCursorShape cursor_shape = TerminalCursorShape::Block;
  std::uint64_t echo_ack = 0;
  std::uint64_t previous_top = 0;
  std::uint64_t screen_top = 0;
  std::uint32_t cursor_row = 0;
  std::uint32_t cursor_column = 0;

  std::vector<TerminalHostRun> scrollback;
  std::vector<TerminalLine> scrollback_lines;

  std::vector<bool> screen_keep;
  std::vector<TerminalLine> screen_lines;

  std::optional<std::string> title;
  std::optional<std::string> working_directory;
  std::optional<std::string> clipboard;
  std::optional<TerminalHostNotification> notification;
  std::optional<std::string> open_url;
  std::optional<TerminalHostEdit> edit;
  std::vector<TerminalHostCellExtra> extras;

  bool has(std::uint16_t bit) const { return (flags & bit) != 0; }
};

inline constexpr std::uint32_t kMaxHostTerminalDimension = 4096;
inline constexpr std::size_t kMaxHostTerminalLineCells = 65536;
inline constexpr std::size_t kMaxHostTerminalTextBytes = 64u << 20;
inline constexpr std::size_t kMaxHostOpenUrlBytes = 8192;
inline constexpr std::size_t kMaxHostEditBytes = 1u << 20;
inline constexpr std::size_t kMaxHostEditPathBytes = 4096;

void EncodeTerminalLine(std::string& out, const TerminalLine& line);
void EncodeTerminalHostFrame(std::string& out, const TerminalHostFrame& frame);
std::optional<TerminalHostFrame> DecodeTerminalHostFrame(std::string_view bytes);

void EncodeTerminalLines(std::string& out, const std::vector<TerminalLine>& lines);
std::optional<std::vector<TerminalLine>> DecodeTerminalLines(std::string_view bytes,
                                                             std::uint64_t max_lines);

void EncodeTerminalInputEvent(std::string& out, const TerminalInputEvent& event);
bool DecodeTerminalInputEvents(std::string_view bytes, std::vector<TerminalInputEvent>& out);

}
