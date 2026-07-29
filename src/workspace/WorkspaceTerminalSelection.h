#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalSession.h"

namespace microide::workspace {

struct TerminalSelectionPoint {
  std::size_t row = 0;
  std::size_t column = 0;
};

struct TerminalSelectionBounds {
  TerminalSelectionPoint start{};
  TerminalSelectionPoint end{};
};

std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head);
terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button);
// Slices a single terminal row into copyable text between [start, end) columns.
// Skips the trailing spacer cell of a double-width glyph and renders empty
// cells as a single space so that internal spacing is preserved; trailing
// blanks are trimmed only when trim_trailing is set. Shared by whole-line copy
// and selection copy so both paths produce identical text.
std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing);
// Default byte budget for a single terminal selection copy. Generous enough for any
// real selection, but bounded so a drag over the full 100k-line scrollback cap cannot
// materialize an unbounded transcript on the UI thread (TD-2026-07-17A-090).
inline constexpr std::size_t kDefaultTerminalSelectionCopyMaxBytes = 8u * 1024u * 1024u;
// Extract the selected text. If the accumulated bytes reach `max_bytes`, the result is
// truncated on a UTF-8 boundary and a "\n[selection truncated]" marker is appended.
std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection,
                                         std::size_t max_bytes = kDefaultTerminalSelectionCopyMaxBytes);
bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column);

// Double-click word bounds around `column` in `line` (rows are absolute, so the
// caller passes the row the click landed on). Word characters are the editor's
// identifier bytes plus the punctuation that makes a path or URL one token in a
// terminal (`.`/`-`/`/`/`~`/`+`/`:`/`@`), which is what every terminal emulator
// selects and what makes double-click useful on `src/foo/bar.cpp:42`.
// Returns nullopt when the clicked cell is blank or not a word character, so the
// caller can leave the caret-style empty selection alone.
std::optional<TerminalSelectionBounds> TerminalWordBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row,
                                                            std::size_t column);
// Triple-click line bounds: the whole row, trailing blanks excluded. Returns nullopt
// for a blank row.
std::optional<TerminalSelectionBounds> TerminalLineBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row);

// Default line/byte budgets for a "Copy Last Command" transcript. A long-running
// command with large output can retain up to the full scrollback cap (100k lines),
// so the invoke path must not snapshot + join an unbounded transcript on the UI
// thread (TD-2026-07-17A-037). Callers cap the snapshot to `...MaxLines` rows and set
// `source_truncated` when rows were dropped; `BuildLastTerminalCommandTranscript` then
// caps the joined bytes and appends a "\n[output truncated]" marker in either case.
inline constexpr std::size_t kDefaultLastTerminalCommandMaxLines = 20000;
inline constexpr std::size_t kDefaultLastTerminalCommandMaxBytes = 8u * 1024u * 1024u;

// Assembles the last-command transcript from already-rendered `rows` (trailing-blank
// and trailing-prompt rows are stripped, mirroring the interactive shell prompt
// heuristic). Joins the surviving rows with '\n' up to `max_bytes`; if the byte budget
// is hit the text is truncated on a UTF-8 boundary and a "\n[output truncated]" marker
// is appended. When `source_truncated` is set (the caller dropped later rows to honor a
// line cap) the marker is appended even if the byte budget was not reached. Returns an
// empty string when nothing survives, so the caller can fall back to the raw invocation.
std::string BuildLastTerminalCommandTranscript(
    const std::vector<std::string>& rows,
    std::string_view trimmed_prompt_prefix,
    std::string_view invocation_first_line,
    bool source_truncated,
    std::size_t max_bytes = kDefaultLastTerminalCommandMaxBytes);

}  // namespace microide::workspace
