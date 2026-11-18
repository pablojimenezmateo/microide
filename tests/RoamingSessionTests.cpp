// What a remote project had open on one machine, for the next (RoamingSession):
// what is captured from a session, the text kept on the host, and how little of
// that text is trusted.

#include "TestSupport.h"

#include "workspace/persistence/RoamingSession.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::CaptureRoamingSession;
using microide::workspace::DecodeRoamingSession;
using microide::workspace::EncodeRoamingSession;
using microide::workspace::PersistedEditorGroupState;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::RoamingSession;
using microide::workspace::RoamingSessionFingerprint;

PersistedEditorTabState Tab(std::filesystem::path path, std::size_t line = 0, std::size_t column = 0) {
  PersistedEditorTabState tab;
  tab.path = std::move(path);
  tab.cursor_line = line;
  tab.cursor_column = column;
  return tab;
}

void TestRoamingSessionCapturesTheProjectsOpenFiles() {
  const std::filesystem::path root = "/mirrors/app/tree";
  PersistedProjectSessionState session;
  PersistedEditorGroupState left;
  left.tabs.push_back(Tab(root / "src/a.cpp", 4, 2));
  left.tabs.push_back(Tab("/elsewhere/host-file.h"));  // outside the project
  PersistedEditorTabState compare = Tab(root / "src/a.cpp");
  compare.kind = "compare";
  left.tabs.push_back(compare);
  left.tabs.push_back(Tab({}));  // an untitled buffer
  PersistedEditorGroupState right;
  right.tabs.push_back(Tab(root / "docs/notes.md", 10, 0));
  right.tabs.push_back(Tab(root / "src/a.cpp", 99, 0));  // the same file in a second group
  right.tabs.push_back(Tab(root / "src/b.cpp", 7, 3));
  right.active_tab_index = 2;
  session.groups = {left, right};
  session.focused_group_index = 1;

  const RoamingSession roaming = CaptureRoamingSession(session, root, "laptop-a", 1700000000);
  Expect(roaming.client == "laptop-a" && roaming.saved_at == 1700000000, "who left it and when");
  Expect(roaming.files.size() == 3, "the project's files, each once: " + std::to_string(roaming.files.size()));
  if (roaming.files.size() != 3) {
    return;
  }
  Expect(roaming.files[0].path == "docs/notes.md" && roaming.files[1].path == "src/a.cpp" &&
             roaming.files[2].path == "src/b.cpp",
         "the focused group's tabs first, paths relative to the root");
  Expect(roaming.files[1].line == 99, "a file open twice keeps the caret of the tab met first");
  Expect(roaming.files[2].active && roaming.files[2].line == 7 && roaming.files[2].column == 3 &&
             !roaming.files[0].active,
         "the tab that was in front, at its caret");

  PersistedProjectSessionState nothing;
  nothing.groups.push_back(PersistedEditorGroupState{});
  Expect(CaptureRoamingSession(nothing, root, "laptop-a", 1).empty(), "nothing open is nothing to pick up");
}

void TestRoamingSessionTextRoundTripsAndIsNotTrusted() {
  RoamingSession roaming;
  roaming.client = "laptop-a";
  roaming.saved_at = 1700000000;
  roaming.files = {RoamingSession::File{.path = "src/a.cpp", .line = 4, .column = 2, .active = false},
                   RoamingSession::File{.path = "docs/a b.md", .line = 0, .column = 0, .active = true}};
  const std::string text = EncodeRoamingSession(roaming);
  const std::optional<RoamingSession> decoded = DecodeRoamingSession(text);
  Expect(decoded.has_value() && decoded->client == "laptop-a" && decoded->saved_at == 1700000000 &&
             decoded->files == roaming.files,
         "what was left is what is read: " + text);

  RoamingSession later = roaming;
  later.client = "laptop-b";
  later.saved_at = 1800000000;
  Expect(RoamingSessionFingerprint(later) == RoamingSessionFingerprint(roaming),
         "the same files and carets are the same session, whoever left it and whenever");
  later.files[0].line = 5;
  Expect(RoamingSessionFingerprint(later) != RoamingSessionFingerprint(roaming), "a moved caret is not");

  Expect(!DecodeRoamingSession("").has_value() && !DecodeRoamingSession("not json").has_value() &&
             !DecodeRoamingSession("[]").has_value() &&
             !DecodeRoamingSession(R"({"version": 2, "client": "x", "saved_at": 1, "files": []})").has_value() &&
             !DecodeRoamingSession(R"({"version": 1, "client": 5, "saved_at": 1})").has_value(),
         "anything but that form is no session");
  // Another machine -- or anything on the host -- wrote it: a path that leaves the
  // root, or names nothing, is dropped; a caret that is no position reads as 0.
  const std::optional<RoamingSession> hostile = DecodeRoamingSession(R"({
    "version": 1, "client": "laptop-a", "saved_at": 1,
    "files": [
      {"path": "../../etc/passwd"}, {"path": "/etc/passwd"}, {"path": "src/../../x"}, {"path": "./a"},
      {"path": ""}, {"path": "dir/"}, {"path": 7}, "nonsense",
      {"path": "ok.txt", "line": -4, "column": 99999999999, "active": true},
      {"path": "second.txt", "active": true}
    ]})");
  Expect(hostile.has_value() && hostile->files.size() == 2, "only the paths under the root survive");
  if (hostile.has_value() && hostile->files.size() == 2) {
    Expect(hostile->files[0].path == "ok.txt" && hostile->files[0].line == 0 && hostile->files[0].column == 0 &&
               hostile->files[0].active && !hostile->files[1].active,
           "a hostile caret is the top of the file, and one tab at most was in front");
  }
  std::string many = R"({"version": 1, "client": "a", "saved_at": 1, "files": [)";
  for (int i = 0; i < 1000; ++i) {
    many += (i == 0 ? "" : ",") + std::string(R"({"path": "f)") + std::to_string(i) + R"("})";
  }
  many += "]}";
  const std::optional<RoamingSession> capped = DecodeRoamingSession(many);
  Expect(capped.has_value() && capped->files.size() == microide::workspace::kMaxRoamingFiles,
         "a list past the cap is cut at it");
}

}  // namespace

void RegisterRoamingSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RoamingSession/CapturesTheProjectsOpenFiles", TestRoamingSessionCapturesTheProjectsOpenFiles);
  AddTest(tests, "RoamingSession/TextRoundTripsAndIsNotTrusted", TestRoamingSessionTextRoundTripsAndIsNotTrusted);
}

}  // namespace microide::tests
