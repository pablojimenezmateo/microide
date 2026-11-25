#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalSession.h"

namespace microide::workspace {

struct TerminalSelectionPoint {
  std::size_t row = 0;
  std::size_t column = 0;
  friend bool operator==(const TerminalSelectionPoint&, const TerminalSelectionPoint&) = default;
};

struct TerminalSelectionBounds {
  TerminalSelectionPoint start{};
  TerminalSelectionPoint end{};
  friend bool operator==(const TerminalSelectionBounds&, const TerminalSelectionBounds&) = default;
};

std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head);
terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button);
std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing);
void AppendTerminalLineSlice(std::string& out,
                             const terminal::TerminalLine& line,
                             std::size_t start,
                             std::size_t end);
std::string TerminalLineText(const terminal::TerminalLine& line);
std::string TrimTrailingTerminalBlanks(std::string text);
inline constexpr std::size_t kDefaultTerminalSelectionCopyMaxBytes = 8u * 1024u * 1024u;
std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection,
                                         std::size_t max_bytes = kDefaultTerminalSelectionCopyMaxBytes);

class TerminalSelectionTextBuilder {
 public:
  explicit TerminalSelectionTextBuilder(const TerminalSelectionBounds& selection,
                                        std::size_t max_bytes = kDefaultTerminalSelectionCopyMaxBytes)
      : selection_(selection), max_bytes_(max_bytes) {}
  bool AddRow(std::size_t row, const terminal::TerminalLine& line);
  std::string Finish(bool next_row_continues);

 private:
  bool SettlePending(bool continued, bool more_rows);

  TerminalSelectionBounds selection_;
  std::size_t max_bytes_;
  std::string text_;
  std::size_t pending_content_end_ = 0;
  bool pending_ = false;
  bool done_ = false;
};
bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column);

std::optional<TerminalSelectionBounds> TerminalWordBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row,
                                                            std::size_t column);
std::optional<TerminalSelectionBounds> TerminalLineBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row);

enum class TerminalSelectionUnit : std::uint8_t { Cell, Word, Line };

struct TerminalSelectionEnds {
  TerminalSelectionPoint anchor{};
  TerminalSelectionPoint head{};
};

TerminalSelectionEnds ExtendTerminalSelection(TerminalSelectionUnit unit,
                                              const TerminalSelectionBounds& origin,
                                              const terminal::TerminalLine& line,
                                              TerminalSelectionPoint point);

inline constexpr std::size_t kDefaultLastTerminalCommandMaxLines = 20000;
inline constexpr std::size_t kDefaultLastTerminalCommandMaxBytes = 8u * 1024u * 1024u;

std::string BuildLastTerminalCommandTranscript(
    const std::vector<std::string>& rows,
    std::string_view trimmed_prompt_prefix,
    std::string_view invocation_first_line,
    bool source_truncated,
    std::size_t max_bytes = kDefaultLastTerminalCommandMaxBytes);

}
