// What a remote project had open on one machine, for the next (RoamingSession):
// what is captured from a session, the text kept on the host, and how little of
// that text is trusted.

#include "TestSupport.h"
#include "compare/BranchReviewStateService.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/persistence/RoamingReview.h"

#include <chrono>
#include <thread>

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
  // The list of files alone does not move with the caret: it is what decides
  // whether a save is worth sending at once.
  using microide::workspace::RoamingSessionFileList;
  Expect(RoamingSessionFileList(later) == RoamingSessionFileList(roaming), "the same files are the same list");
  later.files[0].active = true;
  later.files[1].active = false;
  Expect(RoamingSessionFileList(later) != RoamingSessionFileList(roaming), "another tab in front is not");
  later.files.pop_back();
  Expect(RoamingSessionFileList(later) != RoamingSessionFileList(roaming), "nor is a closed tab");

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

// Review marks and notes follow the user between machines the same way: the
// targets of the repository at the project's root are left as text, without the
// root (each machine's mirror is its own), and merged target by target on the
// machine that reads them -- the copy changed last wins, so a review cleared on
// one machine stays cleared on the other.
void TestRoamingReviewMergesTheNewerCopyOfEachTarget() {
  using microide::compare::BranchReviewNoteScope;
  using microide::compare::BranchReviewStateService;
  using microide::compare::MakeBranchReviewTargetIdentity;
  using microide::workspace::DecodeRoamingReview;
  using microide::workspace::EncodeRoamingReview;
  using microide::workspace::MergeRoamingReview;
  using microide::workspace::PersistedBranchReviewState;
  using microide::workspace::PersistedBranchReviewTarget;
  using microide::workspace::ToPersistedBranchReviewState;
  const std::filesystem::path work_mirror = "/home/work/.local/share/microide/remote/build/src/repo";
  const std::filesystem::path home_mirror = "/home/me/.local/share/microide/remote/build/src/repo";

  BranchReviewStateService work;
  const auto main_review = MakeBranchReviewTargetIdentity(work_mirror, "origin/main", "HEAD", "origin/main", 3);
  const auto checkpoint = MakeBranchReviewTargetIdentity(work_mirror, "refs/worktree/microide/review-checkpoint",
                                                         "WORKTREE", "", 9);
  const auto elsewhere = MakeBranchReviewTargetIdentity("/somewhere/else", "main", "HEAD", "main", 1);
  work.MarkFileReviewed(main_review, work_mirror / "src/a.cpp", 0xfeedfacecafebeefULL);
  work.MarkHunkReviewed(main_review, {.path = "src/b.cpp", .old_start = 4, .old_count = 2, .new_start = 4,
                                      .new_count = 5, .content_hash = 0xffffffffffffffffULL});
  work.SetNote(main_review, BranchReviewNoteScope::Hunk, "src/b.cpp",
               microide::compare::BranchReviewHunkIdentity{.path = "src/b.cpp", .old_start = 4, .old_count = 2,
                                                          .new_start = 4, .new_count = 5, .content_hash = 77},
               "this leaks \"the\" handle");
  work.SetNote(checkpoint, BranchReviewNoteScope::File, "docs/plan.md", std::nullopt, "tighten this");
  work.MarkFileReviewed(elsewhere, "x.cpp");

  const std::string left = EncodeRoamingReview(ToPersistedBranchReviewState(work), work_mirror);
  Expect(!left.empty() && left.find("/home/work") == std::string::npos && left.find("x.cpp") == std::string::npos,
         "the repository's own targets, and no path of the machine that left them: " + left);
  Expect(EncodeRoamingReview(ToPersistedBranchReviewState(work), "/no/such/project").empty(),
         "a project with no review leaves nothing");

  // The other machine: its own mirror, no marks yet.
  std::optional<std::vector<PersistedBranchReviewTarget>> theirs = DecodeRoamingReview(left, home_mirror);
  Expect(theirs.has_value() && theirs->size() == 2, "both targets arrive");
  PersistedBranchReviewState home_state;
  Expect(theirs.has_value() && MergeRoamingReview(*theirs, &home_state), "a machine with none takes them");
  BranchReviewStateService home;
  microide::workspace::LoadBranchReviewStateFromPersisted(home_state, &home);
  const auto home_main = MakeBranchReviewTargetIdentity(home_mirror, "origin/main", "HEAD", "origin/main", 0);
  const auto home_checkpoint = MakeBranchReviewTargetIdentity(home_mirror, "refs/worktree/microide/review-checkpoint",
                                                              "WORKTREE", "", 0);
  Expect(home.NotesAsReferences(home_main) == "src/b.cpp:4-8: this leaks \"the\" handle" &&
             home.NotesAsReferences(home_checkpoint) == "docs/plan.md: tighten this",
         "as targets of its own mirror, notes and all: " + home.NotesAsReferences(home_main));
  const auto* taken = home.FindTarget(home_main);
  Expect(taken != nullptr && taken->reviewed_files.size() == 1 &&
             taken->reviewed_files[0].path == "src/a.cpp" &&
             taken->reviewed_files[0].reviewed_diff_fingerprint == 0xfeedfacecafebeefULL &&
             taken->reviewed_hunks.size() == 1 && taken->reviewed_hunks[0].identity.content_hash == 0xffffffffffffffffULL,
         "marks keep their 64-bit fingerprints");
  Expect(EncodeRoamingReview(ToPersistedBranchReviewState(home), home_mirror) == left,
         "the same marks are the same text on either machine");
  Expect(!MergeRoamingReview(*DecodeRoamingReview(left, home_mirror), &home_state), "and merging them again changes nothing");

  // At home the review is cleared: an emptied target, changed later, which the
  // work machine takes instead of bringing its marks back.
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  home.ClearTarget(home_main);
  const std::string cleared = EncodeRoamingReview(ToPersistedBranchReviewState(home), home_mirror);
  PersistedBranchReviewState work_state = ToPersistedBranchReviewState(work);
  Expect(MergeRoamingReview(*DecodeRoamingReview(cleared, work_mirror), &work_state), "the newer copy is taken");
  microide::workspace::LoadBranchReviewStateFromPersisted(work_state, &work);
  Expect(work.NotesAsReferences(main_review).empty() && work.FindTarget(main_review) != nullptr &&
             work.FindTarget(main_review)->reviewed_files.empty(),
         "a review cleared on one machine is cleared on the other");
  Expect(work.NotesAsReferences(checkpoint) == "docs/plan.md: tighten this" && work.FindTarget(elsewhere) != nullptr,
         "the target that did not change, and another repository's, are as they were");
  // ...and the older copy does not come back over the newer one.
  Expect(!MergeRoamingReview(*DecodeRoamingReview(left, work_mirror), &work_state), "an older copy changes nothing");

  // Untrusted: anything on the host could have written it.
  Expect(!DecodeRoamingReview("", home_mirror).has_value() && !DecodeRoamingReview("[]", home_mirror).has_value() &&
             !DecodeRoamingReview(R"({"version": 2, "targets": []})", home_mirror).has_value(),
         "anything but that form is no review");
  const auto hostile = DecodeRoamingReview(R"({"version": 1, "targets": [
      {"base": "main", "head": "HEAD", "merge_base": "", "changed_at": -5,
       "files": [{"path": "../../etc/passwd", "diff": "1"}, {"path": "/etc/passwd", "diff": "1"},
                 {"path": "ok.cpp", "diff": "18446744073709551616"}, {"path": "ok.cpp", "diff": -3},
                 {"path": "kept.cpp", "diff": "12", "at": 4}, "nonsense"],
       "hunks": [{"path": "kept.cpp", "hunk": {"lines": [1, 2, 3], "hash": "5"}},
                 {"path": "kept.cpp", "hunk": {"lines": [1, 2, 3, -4], "hash": "5"}},
                 {"path": "kept.cpp", "hunk": {"lines": [1, 2, 3, 4], "hash": "5"}}],
       "notes": [{"path": "a/../../b", "text": "x"}, {"path": "kept.cpp", "text": ""},
                 {"path": "kept.cpp", "text": "a hunk's", "hunk": {"lines": "no"}},
                 {"path": "kept.cpp", "text": "a file's"}]},
      {"base": "", "head": "HEAD", "merge_base": ""},
      {"base": "main\n--upload-pack=x", "head": "HEAD", "merge_base": ""},
      {"base": "main", "head": "HEAD", "merge_base": "", "files": [{"path": "second.cpp", "diff": "1"}]},
      7]})", home_mirror);
  Expect(hostile.has_value() && hostile->size() == 1, "one target survives: the nameless, the hostile and the repeated do not");
  if (hostile.has_value() && hostile->size() == 1) {
    const PersistedBranchReviewTarget& target = hostile->front();
    Expect(target.repository_root == home_mirror && target.last_accessed_unix_ms == 0,
           "it is this machine's repository's, and a time that is none reads as the oldest");
    Expect(target.reviewed_files.size() == 1 && target.reviewed_files[0].path == "kept.cpp" &&
               target.reviewed_files[0].reviewed_diff_fingerprint == 12,
           "only a path inside the repository with a real fingerprint is a mark");
    Expect(target.reviewed_hunks.size() == 1 && target.reviewed_hunks[0].identity.new_count == 4,
           "a hunk is four counts and a hash");
    Expect(target.notes.size() == 1 && target.notes[0].text == "a file's" && target.notes[0].scope == "file",
           "a note needs a place and a text, and a hunk's note its hunk");
  }
  // Past the budget the oldest reviews stay behind, whole.
  const std::string one = EncodeRoamingReview(ToPersistedBranchReviewState(home), home_mirror, cleared.size() - 1);
  Expect(!one.empty() && one.size() < cleared.size() && DecodeRoamingReview(one, home_mirror).value().size() == 1,
         "a budget too small for both targets carries the one changed last");
}

}  // namespace

void RegisterRoamingSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RoamingSession/ReviewMergesTheNewerCopyOfEachTarget", TestRoamingReviewMergesTheNewerCopyOfEachTarget);
  AddTest(tests, "RoamingSession/CapturesTheProjectsOpenFiles", TestRoamingSessionCapturesTheProjectsOpenFiles);
  AddTest(tests, "RoamingSession/TextRoundTripsAndIsNotTrusted", TestRoamingSessionTextRoundTripsAndIsNotTrusted);
}

}  // namespace microide::tests
