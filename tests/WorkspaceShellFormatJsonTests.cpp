#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::NotificationService;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Scope the app config/state homes so persistence is test-local (mirrors the
// other WorkspaceShell fixtures).

std::string ViewportText(const editor::TextViewport& viewport) {
  const editor::TextBuffer& lines = viewport.lines();
  std::string text;
  for (std::size_t i = 0; i < lines.LineCount(); ++i) {
    if (i != 0) text.push_back('\n');
    text.append(lines.LineView(i));
  }
  return text;
}

bool AnyNotificationContains(const WorkspaceShell& shell, std::string_view needle) {
  for (const auto& note : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    if (note.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

// The command reindents the active buffer, sorts object keys human-alphabetically,
// keeps the edit in memory (disk untouched), and is one undoable step.
void TestFormatJsonCommandFormatsActiveBufferInMemory() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root);
  const std::filesystem::path file = root / "data.json";
  const std::string original = R"({"zebra":1,"apple":{"y":2,"x":3}})";
  WriteFile(file, original);

  WorkspaceShell shell;
  shell.Initialize({});
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open project");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file), "open data.json");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-json"),
         "format-json should be handled");

  const std::string formatted = ViewportText(WorkspaceShellTestAccess::ActiveEditor(shell));
  Expect(formatted.find('\n') != std::string::npos, "buffer should be reflowed onto many lines");
  const std::size_t apple = formatted.find("\"apple\"");
  const std::size_t zebra = formatted.find("\"zebra\"");
  Expect(apple != std::string::npos && zebra != std::string::npos, "both keys present");
  Expect(apple < zebra, "keys sorted: apple before zebra");
  Expect(formatted.find("\"x\"") < formatted.find("\"y\""), "subkeys sorted: x before y");

  // The edit is in memory only — disk still holds the original minified bytes.
  Expect(ReadFile(file) == original, "format-json must not write to disk");

  // One undo restores the original single-line content.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "undo"), "undo should be handled");
  Expect(ViewportText(WorkspaceShellTestAccess::ActiveEditor(shell)) == original,
         "a single undo restores the pre-format buffer");

  // The caret keeps its line (clamped) instead of snapping to the top of the
  // buffer with the whole-document replace.
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.LoadContent("{\n\"zebra\": 1,\n\"apple\": 2\n}\n", file);
  viewport.MoveCursorTo(2, 3);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-json"), "format again");
  Expect(viewport.line_count() > 3 && viewport.cursor_line() == 2,
         "the caret stays on its line after formatting (got line " +
             std::to_string(viewport.cursor_line()) + ")");
}

// Invalid JSON is reported via a toast and leaves the buffer untouched.
void TestFormatJsonCommandTogglesToastOnInvalidJson() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root);
  const std::filesystem::path file = root / "bad.json";
  const std::string original = "{not valid json}";
  WriteFile(file, original);

  WorkspaceShell shell;
  shell.Initialize({});
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open project");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file), "open bad.json");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-json"),
         "format-json should be handled even for invalid input");
  Expect(AnyNotificationContains(shell, "Invalid JSON"), "invalid JSON should toast");
  Expect(ViewportText(WorkspaceShellTestAccess::ActiveEditor(shell)) == original,
         "invalid JSON leaves the buffer unchanged");
}

// With no editable buffer, the command surfaces a toast rather than doing nothing.
void TestFormatJsonCommandToastsWhenNoActiveBuffer() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-json"),
         "format-json is handled (it posts a toast) when there is no buffer");
  Expect(AnyNotificationContains(shell, "No active buffer"),
         "no active buffer should surface a toast");
}

}  // namespace

void RegisterWorkspaceShellFormatJsonTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/FormatJson/FormatsActiveBufferInMemory",
          TestFormatJsonCommandFormatsActiveBufferInMemory);
  AddTest(tests, "WorkspaceShell/FormatJson/ToastOnInvalidJson",
          TestFormatJsonCommandTogglesToastOnInvalidJson);
  AddTest(tests, "WorkspaceShell/FormatJson/ToastWhenNoActiveBuffer",
          TestFormatJsonCommandToastsWhenNoActiveBuffer);
}

}  // namespace microide::tests
