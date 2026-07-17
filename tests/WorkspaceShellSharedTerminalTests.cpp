#include "TestSupport.h"

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelVisibleRowsForHeight;
using microide::workspace::ClampScrollRowToContent;
using microide::workspace::ExtractTerminalSelectionText;
using microide::workspace::NormalizeTerminalSelection;
using microide::workspace::TailScrollRowForContent;
using microide::workspace::TerminalMouseButtonFromSdl;
using microide::workspace::TerminalSelectionBounds;
using microide::workspace::TerminalSelectionContainsCell;
using microide::workspace::TerminalSelectionPoint;

microide::terminal::TerminalLine MakeTerminalLine(std::string_view text) {
  microide::terminal::TerminalLine line;
  line.cells.reserve(text.size());
  for (char c : text) {
    microide::terminal::TerminalCell cell;
    cell.SetAscii(c);
    line.cells.push_back(cell);
  }
  return line;
}

void TestWorkspaceSharedTerminalPanelHelpers() {
  Expect(BottomPanelVisibleRowsForHeight(220.0f, 16.0f) == 10,
         "panel visible rows should use header and footer padding");
  Expect(TailScrollRowForContent(25, 8) == 17,
         "tail scroll should leave the last visible page aligned");
  Expect(TailScrollRowForContent(4, 8) == 0, "tail scroll should clamp when content fits");
  Expect(ClampScrollRowToContent(-3, 25, 8) == 0, "scroll clamp should bound negative values");
  Expect(ClampScrollRowToContent(40, 25, 8) == 17,
         "scroll clamp should bound past-end values");
}

void TestWorkspaceSharedTerminalSelectionHelpers() {
  const auto normalized = NormalizeTerminalSelection(TerminalSelectionPoint{.row = 3, .column = 8},
                                                     TerminalSelectionPoint{.row = 1, .column = 2});
  Expect(normalized.has_value(), "terminal selection should normalize present endpoints");
  Expect(normalized->start.row == 1 && normalized->start.column == 2,
         "terminal selection should sort the start position");
  Expect(normalized->end.row == 3 && normalized->end.column == 8,
         "terminal selection should sort the end position");
  Expect(!NormalizeTerminalSelection(std::nullopt, TerminalSelectionPoint{.row = 0, .column = 0})
              .has_value(),
         "terminal selection should require both endpoints");

  const std::vector<microide::terminal::TerminalLine> lines = {
      MakeTerminalLine("alpha"),
      MakeTerminalLine("bravo"),
      MakeTerminalLine("charlie"),
  };
  const TerminalSelectionBounds selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 2},
      .end = TerminalSelectionPoint{.row = 2, .column = 4},
  };
  Expect(ExtractTerminalSelectionText(lines, selection) == "pha\nbravo\nchar",
         "terminal selection text should span multiple rows");
  Expect(TerminalSelectionContainsCell(selection, 0, 2),
         "terminal selection should include its start cell");
  Expect(!TerminalSelectionContainsCell(selection, 0, 1),
         "terminal selection should exclude cells before the start");
  Expect(TerminalSelectionContainsCell(selection, 1, 3),
         "terminal selection should include full interior rows");
  Expect(!TerminalSelectionContainsCell(selection, 2, 4),
         "terminal selection should exclude the end column");

  // Soft-wrapped rows (a long line the terminal auto-wrapped) must be joined
  // WITHOUT a newline when copied, because the terminal stream never contained
  // one there. A real (hard) line break still emits '\n'.
  std::vector<microide::terminal::TerminalLine> wrapped_lines = {
      MakeTerminalLine("foobar"),
      MakeTerminalLine("baz"),   // soft continuation of "foobar"
      MakeTerminalLine("next"),  // a real new line
  };
  wrapped_lines[1].wrapped_from_previous = true;
  wrapped_lines[2].wrapped_from_previous = false;
  const TerminalSelectionBounds wrapped_selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 2, .column = 4},
  };
  Expect(ExtractTerminalSelectionText(wrapped_lines, wrapped_selection) == "foobarbaz\nnext",
         "soft-wrapped rows must join without an injected newline; hard breaks keep it");
}

// A terminal selection copy is bounded by a byte budget: a drag over a huge scrollback
// truncates on a UTF-8 boundary and appends a marker instead of materializing an
// unbounded transcript on the UI thread (TD-2026-07-17A-090).
void TestWorkspaceSharedTerminalSelectionCopyByteBudget() {
  std::vector<microide::terminal::TerminalLine> lines;
  for (int i = 0; i < 50; ++i) {
    lines.push_back(MakeTerminalLine("row-" + std::to_string(i) + "-payload"));
  }
  const TerminalSelectionBounds selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = lines.size() - 1, .column = 8},
  };

  const std::string full = ExtractTerminalSelectionText(lines, selection);
  const std::string capped = ExtractTerminalSelectionText(lines, selection, /*max_bytes=*/32);
  const std::string marker = "\n[selection truncated]";

  Expect(capped.size() < full.size(), "a small byte budget truncates the selection copy");
  Expect(capped.size() <= 32 + marker.size(),
         "the truncated copy is bounded by the byte budget plus the marker");
  Expect(capped.size() >= marker.size() &&
             capped.compare(capped.size() - marker.size(), marker.size(), marker) == 0,
         "a truncated selection copy ends with the truncation marker");
  // A selection that fits within the budget is returned verbatim (no marker).
  const std::string small = ExtractTerminalSelectionText(lines, selection, /*max_bytes=*/1u << 20);
  Expect(small == full && small.find("[selection truncated]") == std::string::npos,
         "a selection within the budget is copied verbatim without a marker");
}

void TestWorkspaceSharedTerminalMouseHelpers() {
  Expect(TerminalMouseButtonFromSdl(SDL_BUTTON_LEFT) ==
             microide::terminal::TerminalSession::MouseButton::Left,
         "left button should map to terminal left button");
  Expect(TerminalMouseButtonFromSdl(SDL_BUTTON_MIDDLE) ==
             microide::terminal::TerminalSession::MouseButton::Middle,
         "middle button should map to terminal middle button");
  Expect(TerminalMouseButtonFromSdl(SDL_BUTTON_RIGHT) ==
             microide::terminal::TerminalSession::MouseButton::Right,
         "right button should map to terminal right button");
  Expect(TerminalMouseButtonFromSdl(SDL_BUTTON_X1) ==
             microide::terminal::TerminalSession::MouseButton::None,
         "unsupported buttons should map to none");
}

}  // namespace

void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/TerminalPanelHelpers", TestWorkspaceSharedTerminalPanelHelpers);
  AddTest(tests, "WorkspaceShared/TerminalSelectionHelpers",
          TestWorkspaceSharedTerminalSelectionHelpers);
  AddTest(tests, "WorkspaceShared/TerminalSelectionCopyByteBudget",
          TestWorkspaceSharedTerminalSelectionCopyByteBudget);
  AddTest(tests, "WorkspaceShared/TerminalMouseHelpers", TestWorkspaceSharedTerminalMouseHelpers);
}

}  // namespace microide::tests
