#include "TestSupport.h"

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelVisibleRowsForHeight;
using microide::workspace::BuildLastTerminalCommandTranscript;
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

  std::vector<microide::terminal::TerminalLine> wrapped_lines = {
      MakeTerminalLine("foobar"),
      MakeTerminalLine("baz"),
      MakeTerminalLine("next"),
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
  const std::string capped = ExtractTerminalSelectionText(lines, selection, 32);
  const std::string marker = "\n[selection truncated]";

  Expect(capped.size() < full.size(), "a small byte budget truncates the selection copy");
  Expect(capped.size() <= 32 + marker.size(),
         "the truncated copy is bounded by the byte budget plus the marker");
  Expect(capped.size() >= marker.size() &&
             capped.compare(capped.size() - marker.size(), marker.size(), marker) == 0,
         "a truncated selection copy ends with the truncation marker");
  const std::string small = ExtractTerminalSelectionText(lines, selection, 1u << 20);
  Expect(small == full && small.find("[selection truncated]") == std::string::npos,
         "a selection within the budget is copied verbatim without a marker");
}

void TestWorkspaceSharedLastCommandTranscript() {
  {
    const std::vector<std::string> rows = {
        "user@host:~/repo$ ll", "file-a.txt", "file-b.txt", "user@host:~/repo$ ", ""};
    const std::string transcript = BuildLastTerminalCommandTranscript(
        rows, "user@host:~/repo$",
        "ll", false);
    Expect(transcript == "user@host:~/repo$ ll\nfile-a.txt\nfile-b.txt",
           "the transcript keeps the command + output and drops the redrawn prompt row");
  }

  {
    const std::vector<std::string> rows = {"$ build", "step-1", "step-2"};
    const std::string transcript = BuildLastTerminalCommandTranscript(
        rows, "$", "build",
        true);
    Expect(transcript == "$ build\nstep-1\nstep-2\n[output truncated]",
           "a line-capped snapshot appends the output-truncated marker");
  }

  {
    std::vector<std::string> rows;
    for (int i = 0; i < 40; ++i) {
      rows.push_back("output-row-" + std::to_string(i) + "-payload");
    }
    const std::string full = BuildLastTerminalCommandTranscript(
        rows, "", "",
        false);
    const std::string marker = "\n[output truncated]";
    const std::string capped = BuildLastTerminalCommandTranscript(
        rows, "", "",
        false, 48);
    Expect(capped.size() < full.size(), "a small byte budget truncates the transcript");
    Expect(capped.size() <= 48 + marker.size(),
           "the truncated transcript is bounded by the byte budget plus the marker");
    Expect(capped.size() >= marker.size() &&
               capped.compare(capped.size() - marker.size(), marker.size(), marker) == 0,
           "a byte-truncated transcript ends with the output-truncated marker");
    const std::string small = BuildLastTerminalCommandTranscript(
        rows, "", "",
        false, 1u << 20);
    Expect(small == full && small.find("[output truncated]") == std::string::npos,
           "a transcript within the budget is returned verbatim without a marker");
  }

  {
    const std::vector<std::string> rows = {"h\xC3\xA9llo-h\xC3\xA9llo-h\xC3\xA9llo"};
    const std::string capped = BuildLastTerminalCommandTranscript(
        rows, "", "",
        false, 2);
    const std::string marker = "\n[output truncated]";
    Expect(capped.size() >= marker.size(), "a truncated multibyte transcript still emits the marker");
    const std::string body = capped.substr(0, capped.size() - marker.size());
    Expect(body == "h", "the byte cut backs off past the split multibyte sequence to a UTF-8 boundary");
    Expect(body.empty() ||
               (static_cast<unsigned char>(body.back()) & 0xC0) != 0x80,
           "the truncated body ends on a UTF-8 boundary");
  }

  {
    const std::vector<std::string> rows = {"$ ", ""};
    const std::string transcript = BuildLastTerminalCommandTranscript(
        rows, "$", "anything",
        false);
    Expect(transcript.empty(), "an all-prompt/blank snapshot yields an empty transcript");
  }
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

}

void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/TerminalPanelHelpers", TestWorkspaceSharedTerminalPanelHelpers);
  AddTest(tests, "WorkspaceShared/TerminalSelectionHelpers",
          TestWorkspaceSharedTerminalSelectionHelpers);
  AddTest(tests, "WorkspaceShared/TerminalSelectionCopyByteBudget",
          TestWorkspaceSharedTerminalSelectionCopyByteBudget);
  AddTest(tests, "WorkspaceShared/LastCommandTranscript",
          TestWorkspaceSharedLastCommandTranscript);
  AddTest(tests, "WorkspaceShared/TerminalMouseHelpers", TestWorkspaceSharedTerminalMouseHelpers);
}

}
