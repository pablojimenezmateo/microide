// The review controls on a diff (render/CompareReviewControls,
// services/CompareReviewService): a hunk's Mark Reviewed / Comment buttons, its
// comment row's Edit / Delete, the header's Previous / Next Change and Mark File
// Reviewed, the comments handed to an agent from the Git menu and the git view's
// right-click, and the status bar's worktree count.
//
// Each flow clicks what the `ui` query names, as a user or the QA monkey does, and
// every transition paints as the app loop paints it and must equal a full
// repaint (support/FlowFixture.h).

#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "WorkspaceShellEventHelpers.h"
#include "support/FlowFixture.h"
#include "support/GitSidebarWait.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;
using compare::BranchReviewMarkerStatus;

std::string Lines(int count, int changed_a = -1, int changed_b = -1) {
  std::string text;
  for (int i = 1; i <= count; ++i) {
    text += (i == changed_a || i == changed_b) ? "changed " + std::to_string(i) : "line " + std::to_string(i);
    text += '\n';
  }
  return text;
}

// A review of calc.txt against `main`: two changes far enough apart
// that the unchanged run between them collapses (lines 3 and 25 of 30).
struct ReviewScene {
  ReviewScene() : ReviewScene(Lines(30), Lines(30, 3, 25)) {}

  // A review of calc.txt going from `base` to `changed`, in a window of the
  // given size.
  ReviewScene(const std::string& base, const std::string& changed, int width = FlowFixture::kWidth,
              int height = FlowFixture::kHeight)
      : fixture("calc.txt", base, width, height, [&changed](const std::filesystem::path& root) {
          // As an agent leaves it: its work committed on a branch of its own.
          InitializeGitRepo(root);
          CommitAll(root, "base", "the review's base");
          RequireGitCommandSuccess(root, {"branch", "-M", "main"}, "the base branch");
          RequireGitCommandSuccess(root, {"checkout", "-q", "-b", "agent"}, "the agent's branch");
          WriteFile(root / "calc.txt", changed);
          CommitAll(root, "agent: clamp", "the agent's commit");
        }) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "review-branch main"), "the review opens");
    TestAccess::FlushPendingFileReads(fixture.shell);
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the review's first frame");
    TestAccess::FlushPendingFileReads(fixture.shell);  // the frame that shows a tab loads it
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the loaded review");
  }

  workspace::CompareTabState& Tab() {
    workspace::CompareTabState* tab = TestAccess::ActiveCompareOrNull(fixture.shell);
    Expect(tab != nullptr, "a diff is in front");
    return *tab;
  }

  std::optional<SDL_FRect> Button(std::string_view name) { return fixture.ElementRect("button:" + std::string(name)); }

  // Clicks the control and expects the frame it owes to be fresh.
  void Click(std::string_view name, const std::string& what) {
    const std::optional<SDL_FRect> rect = Button(name);
    Expect(rect.has_value(), std::string(name) + " should be on screen to " + what);
    if (rect.has_value()) {
      fixture.ExpectFresh(fixture.Click(*rect), what);
    }
  }

  // The first row of hunk `hunk`, on screen.
  std::optional<SDL_FRect> HunkRow(int hunk) {
    const workspace::CompareTabState& tab = Tab();
    const std::size_t row = compare::ComparePresentationRowForModelRow(
        tab.presentation, static_cast<std::size_t>(tab.model.hunks[static_cast<std::size_t>(hunk)].start_row));
    return TestAccess::ActiveCompareRowRangeRect(fixture.shell, row, row + 1);
  }

  std::size_t NoteRows() {
    std::size_t count = 0;
    for (const compare::ComparePresentationRow& row : Tab().presentation.rows) {
      count += row.kind == compare::ComparePresentationRowKind::ReviewNote ? 1 : 0;
    }
    return count;
  }

  // Types a comment into the open prompt and saves it, as the keyboard does.
  void TypeComment(const char* text, const std::string& what) {
    Expect(TestAccess::PromptSurfaceVisible(fixture.shell) &&
               TestAccess::CurrentPromptSurface(fixture.shell).action ==
                   workspace::PromptSurfaceState::Action::ReviewComment,
           "the comment prompt is open to " + what);
    TestAccess::CurrentPromptSurface(fixture.shell).input.SetText({});
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(text), KeyDownEvent(SDLK_RETURN)}), what);
  }

  FlowFixture fixture;
};

// The selected change shows Mark Reviewed and Comment; a click marks it, its
// badge says so and the header counts it, and Unmark takes it back. Another
// change shows its buttons while the pointer is over it.
void TestHunkButtonsMarkAndUnmarkAChange() {
  ReviewScene scene;
  Expect(scene.Button("compare/hunk-0/toggle-reviewed").has_value() && scene.Button("compare/hunk-0/comment"),
         "the change the review opened on shows its buttons");
  Expect(!scene.Button("compare/hunk-1/toggle-reviewed").has_value(),
         "a change that is neither selected nor hovered shows none");

  scene.Click("compare/hunk-0/toggle-reviewed", "marking the change reviewed");
  Expect(workspace::CompareHunkReviewStatus(scene.Tab(), 0) == BranchReviewMarkerStatus::Reviewed,
         "the change is marked reviewed");
  Expect(scene.Tab().review_header.summary_line.find("2 changes, 1 reviewed") != std::string::npos,
         "the header counts it: " + scene.Tab().review_header.summary_line);
  Expect(TestAccess::IsActionEnabled(scene.fixture.shell, WorkspaceShell::ActionId::UnmarkBranchHunkReviewed),
         "and the palette can take it back");

  scene.Click("compare/hunk-0/toggle-reviewed", "unmarking it");
  Expect(workspace::CompareHunkReviewStatus(scene.Tab(), 0) == BranchReviewMarkerStatus::Unreviewed,
         "Unmark takes the mark back");

  const std::optional<SDL_FRect> row = scene.HunkRow(1);
  Expect(row.has_value(), "the second change is on screen");
  if (!row.has_value()) {
    return;
  }
  scene.fixture.ExpectFresh(
      DispatchEvents(scene.fixture.shell, {MouseMotionEvent(row->x + 40.0f, row->y + row->h / 2.0f)}),
      "the pointer over the second change");
  Expect(scene.Button("compare/hunk-1/toggle-reviewed").has_value(), "the hovered change shows its buttons");
  scene.Click("compare/hunk-1/toggle-reviewed", "marking the hovered change");
  Expect(workspace::CompareHunkReviewStatus(scene.Tab(), 1) == BranchReviewMarkerStatus::Reviewed &&
             workspace::CompareHunkReviewStatus(scene.Tab(), 0) == BranchReviewMarkerStatus::Unreviewed,
         "the click marks the change it is on, not the selected one");
  SDL_Event leave{};
  leave.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
  scene.fixture.ExpectFresh(DispatchEvents(scene.fixture.shell, {leave}), "the pointer leaving the window");
  Expect(!scene.Button("compare/hunk-1/toggle-reviewed").has_value(),
         "leaving the window takes the hovered change's buttons away");
  scene.fixture.ExpectFresh(
      DispatchEvents(scene.fixture.shell, {MouseMotionEvent(row->x + 40.0f, row->y + row->h / 2.0f)}),
      "the pointer back over the second change");
  scene.fixture.ExpectFresh(DispatchEvents(scene.fixture.shell, {MouseMotionEvent(4.0f, 4.0f)}),
                            "the pointer leaving the diff");
  Expect(!scene.Button("compare/hunk-1/toggle-reviewed").has_value(),
         "off the change its buttons go; its badge stays (it is painted, not a control)");
}

// Comment opens the prompt; what is typed shows under the change with Edit and
// Delete; Edit opens it seeded with the comment; Delete removes it.
void TestCommentsShowUnderTheirChange() {
  ReviewScene scene;
  scene.Click("compare/hunk-0/comment", "opening the comment prompt");
  Expect(TestAccess::CurrentPromptSurface(scene.fixture.shell).detail == "Comment on calc.txt, line 3.",
         "the prompt says where the comment goes: " + TestAccess::CurrentPromptSurface(scene.fixture.shell).detail);
  scene.TypeComment("is this right?", "saving the comment");
  Expect(scene.NoteRows() == 1, "the comment shows as a row under its change");
  const std::optional<SDL_FRect> edit = scene.Button("compare/note-0/edit");
  Expect(edit.has_value() && !scene.Button("compare/hunk-0/comment").has_value(),
         "the row edits it; the change's own Comment button goes");

  scene.Click("compare/note-0/edit", "editing the comment");
  Expect(TestAccess::CurrentPromptSurface(scene.fixture.shell).input.text() == "is this right?",
         "Edit opens the prompt with the comment in it");
  scene.TypeComment("clamp the low end too", "saving the edit");
  Expect(scene.Tab().review_hunk_markers[0].note_text == "clamp the low end too", "the edit replaced the comment");

  scene.Click("compare/note-0/delete", "deleting the comment");
  Expect(scene.NoteRows() == 0 && !scene.Tab().review_hunk_markers[0].has_note, "Delete removes it");
  Expect(scene.Button("compare/hunk-0/comment").has_value(), "and the change can take a comment again");
}

// The prompt pins the change it was opened on: whatever moves the selection
// while it is up (an agent rewriting the file reloads the diff), the comment
// lands where the reader opened it.
void TestCommentPromptPinsItsChange() {
  ReviewScene scene;
  scene.Click("compare/hunk-0/comment", "opening the comment prompt on the first change");
  workspace::CompareTabState& tab = scene.Tab();
  tab.selected_row = compare::ComparePresentationRowForModelRow(
      tab.presentation, static_cast<std::size_t>(tab.model.hunks[1].start_row));
  scene.TypeComment("first change", "saving with the selection moved");
  Expect(scene.Tab().review_hunk_markers.size() == 2 && scene.Tab().review_hunk_markers[0].note_text == "first change" &&
             !scene.Tab().review_hunk_markers[1].has_note,
         "the comment is on the change the prompt was opened on");
}

// Previous / Next Change step through the diff's changes; Mark File Reviewed
// marks every change and flips to Unmark File, which takes them all back.
void TestHeaderStepsChangesAndMarksTheFile() {
  ReviewScene scene;
  Expect(workspace::CompareTabSelectedHunkIndex(scene.Tab()) == 0, "the review opens on its first change");
  scene.Click("compare/next-change", "Next Change");
  Expect(workspace::CompareTabSelectedHunkIndex(scene.Tab()) == 1, "Next Change selects the second");
  scene.Click("compare/previous-change", "Previous Change");
  Expect(workspace::CompareTabSelectedHunkIndex(scene.Tab()) == 0, "Previous Change goes back");

  scene.Click("compare/file-reviewed", "Mark File Reviewed");
  Expect(scene.Tab().review_reviewed_hunk_count == 2 && workspace::CompareFileFullyReviewed(scene.Tab()),
         "every change is marked");
  Expect(scene.fixture.ElementRect("button:compare/file-reviewed").has_value() &&
             workspace::CompareFileReviewedLabel(true) == "Unmark File",
         "the button now unmarks");
  scene.Click("compare/file-reviewed", "Unmark File");
  Expect(scene.Tab().review_reviewed_hunk_count == 0, "Unmark File takes every mark back");
}

// A working-tree diff from the git view (the working tree against HEAD) is for
// staging, not review: it steps through its changes but takes no marks.
void TestAStagingDiffStepsButTakesNoMarks() {
  FlowFixture fixture("calc.txt", Lines(30), [](const std::filesystem::path& root) {
    InitializeGitRepo(root);
    CommitAll(root, "base", "base");
    WriteFile(root / "calc.txt", Lines(30, 3, 25));
  });
  TestAccess::OpenTreeContextMenuForPath(fixture.shell, workspace::TreeContextTargetKind::File, fixture.file);
  Expect(TestAccess::ExecuteContextMenuAction(fixture.shell, WorkspaceShell::ActionId::CompareHead),
         "the file's Compare with HEAD opens the working tree against HEAD");
  TestAccess::FlushPendingFileReads(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the staging diff");
  TestAccess::FlushPendingFileReads(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the loaded staging diff");
  Expect(fixture.ElementRect("button:compare/next-change").has_value(), "it steps through its changes");
  Expect(!fixture.ElementRect("button:compare/file-reviewed").has_value() &&
             !fixture.ElementRect("button:compare/hunk-0/toggle-reviewed").has_value(),
         "and has no review controls");
}

// The review's comments go to an agent from anywhere: the Git menu counts them
// with no diff in front, and the git view's right-click on a folder hands over
// the ones under it.
void TestCommentsAreHandedOverFromTheGitMenuAndTheGitView() {
  ReviewScene scene;
  WorkspaceShell& shell = scene.fixture.shell;
  std::string copied;
  TestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    copied = std::string(text);
    return true;
  });
  scene.Click("compare/hunk-0/comment", "commenting on the first change");
  scene.TypeComment("first", "the first comment");
  scene.Click("compare/next-change", "to the second change");
  scene.Click("compare/hunk-1/comment", "commenting on the second change");
  scene.TypeComment("second", "the second comment");

  // Away from the diff: the editor tab the fixture opened.
  for (std::size_t i = 0; i < TestAccess::OpenTabs(shell).size(); ++i) {
    if (TestAccess::OpenTabs(shell)[i].kind == workspace::TabEntry::Kind::Editor) {
      TestAccess::ActivateTab(shell, i);
    }
  }
  Expect(TestAccess::ActiveCompareOrNull(shell) == nullptr, "no diff is in front");
  const std::vector<std::string> git_menu = TestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::Git);
  Expect(std::find(git_menu.begin(), git_menu.end(), "Copy Review Comments (2)") != git_menu.end(),
         "the Git menu offers the review's two comments");
  Expect(TestAccess::ExecuteCommandLine(shell, "copy-review-comments") &&
             copied == "calc.txt:3: first\ncalc.txt:25: second",
         "the review's comments, as references: " + copied);

  copied.clear();
  Expect(!TestAccess::ExecuteCommandLine(shell, "copy-review-comments docs"),
         "a path with no comments under it copies nothing, and says so");
  Expect(TestAccess::ExecuteCommandLine(shell, "copy-review-comments calc.txt") && copied.find("calc.txt:3") == 0,
         "a file's path narrows it to that file");

  // The git view: right-click on the Outgoing header is the whole review.
  TestAccess::ShowGitSidebar(shell);
  Expect(SettleGitSidebarRefresh(shell), "the git view settles");
  scene.fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the git view");
  std::optional<SDL_FRect> header;
  const util::JsonValue ui = TestAccess::ControlUi(shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString().starts_with("row:git/Outgoing")) {
      const auto& r = entry["rect"].AsArray();
      header = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(header.has_value(), "the Outgoing header is a row");
  if (!header.has_value()) {
    return;
  }
  SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, header->x + 20.0f, header->y + header->h / 2.0f);
  down.button.button = SDL_BUTTON_RIGHT;
  SDL_Event up = down;
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  (void)DispatchEvents(shell, {down, up});
  scene.fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the review's context menu");
  const std::optional<SDL_FRect> item = scene.fixture.ElementRect("menu-item:context/Copy Review Comments (2)");
  Expect(item.has_value(), "the right-click offers the review's comments");
  copied.clear();
  if (item.has_value()) {
    (void)scene.fixture.Click(*item);
  }
  Expect(copied == "calc.txt:3: first\ncalc.txt:25: second", "and copies them: " + copied);
}

std::string LinesWhere(int count, const std::function<bool(int)>& changed) {
  std::string text;
  for (int i = 1; i <= count; ++i) {
    text += (changed(i) ? "changed " : "line ") + std::to_string(i) + '\n';
  }
  return text;
}

// A change longer than the view keeps its buttons: once its first row scrolls
// off above, they ride the view's top row for as long as that is inside the
// change. They sat on the first row only, so deep in a long change there was
// nothing to click.
void TestALongChangeKeepsItsButtonsOnScreen() {
  ReviewScene scene(Lines(300), LinesWhere(300, [](int line) { return line >= 5 && line <= 250; }));
  const std::optional<SDL_FRect> first = scene.Button("compare/hunk-0/toggle-reviewed");
  Expect(first.has_value(), "the change shows Mark Reviewed on its first row");
  const std::optional<SDL_FRect> row = scene.HunkRow(0);
  if (!first.has_value() || !row.has_value()) {
    return;
  }
  const int scroll_before = scene.Tab().scroll_row;
  scene.fixture.ExpectFresh(
      DispatchEvents(scene.fixture.shell, {MouseWheelEvent(row->x + 40.0f, row->y + 100.0f, -10.0f)}),
      "scrolling into the change");
  Expect(scene.Tab().scroll_row > scroll_before + 8, "the first row scrolled off above");
  const std::optional<SDL_FRect> sticky = scene.Button("compare/hunk-0/toggle-reviewed");
  const workspace::WorkspaceShell::CompareSurfaceLayout layout =
      TestAccess::ActiveCompareSurfaceLayout(scene.fixture.shell);
  Expect(sticky.has_value() && sticky->y >= layout.rows_y - 2.0f && sticky->y < layout.rows_y + layout.line_height,
         "its buttons are on the view's top row");
  scene.Click("compare/hunk-0/toggle-reviewed", "marking the change from inside it");
  Expect(workspace::CompareHunkReviewStatus(scene.Tab(), 0) == BranchReviewMarkerStatus::Reviewed,
         "and the click marks it");
}

// Rows scrolled under a still pointer move the hover with them: the change that
// was under the pointer kept its buttons until the mouse moved again.
void TestScrollingMovesTheHoveredChange() {
  ReviewScene scene(Lines(400), LinesWhere(400, [](int line) {
    return line == 3 || line == 9 || (line >= 30 && line % 12 == 0);
  }));
  const std::optional<SDL_FRect> row = scene.HunkRow(1);
  Expect(row.has_value(), "the second change is on screen");
  if (!row.has_value()) {
    return;
  }
  const float x = row->x + 40.0f;
  const float y = row->y + row->h / 2.0f;
  scene.fixture.ExpectFresh(DispatchEvents(scene.fixture.shell, {MouseMotionEvent(x, y)}),
                            "the pointer over the second change");
  Expect(scene.Button("compare/hunk-1/toggle-reviewed").has_value(), "the hovered change shows its buttons");
  const int scroll_before = scene.Tab().scroll_row;
  scene.fixture.ExpectFresh(DispatchEvents(scene.fixture.shell, {MouseWheelEvent(x, y, -1.0f)}),
                            "one wheel notch with the pointer still");
  Expect(scene.Tab().scroll_row > scroll_before && scene.Tab().scroll_row < scroll_before + 8,
         "the rows moved a few lines: an unchanged line is under the pointer now");
  Expect(scene.HunkRow(1).has_value() && !scene.Button("compare/hunk-1/toggle-reviewed").has_value(),
         "the change still on screen but no longer under the pointer drops its buttons");
}

// A comment whose change was edited since stays under it, flagged, and saving
// it re-pins it (one comment, not two); one whose change is gone shows at the
// top and can still be deleted. Both were out of sight while the export went on
// handing them over, and only clearing the whole review removed them.
void TestACommentOutlivesItsChange() {
  ReviewScene scene;
  compare::BranchReviewStateService& review = TestAccess::CurrentProjectState(scene.fixture.shell).branch_review;
  const compare::BranchReviewTargetIdentity target = scene.Tab().branch_target;
  compare::BranchReviewHunkIdentity edited_since =
      compare::ComputeBranchReviewHunkIdentity(scene.Tab().model, 1, scene.Tab().path);
  edited_since.content_hash ^= 1;  // written on other text at the same place
  review.SetNote(target, compare::BranchReviewNoteScope::Hunk, scene.Tab().path, edited_since, "before the edit");
  compare::BranchReviewHunkIdentity gone = edited_since;
  gone.old_start = gone.new_start = 900;
  gone.content_hash = 7;
  review.SetNote(target, compare::BranchReviewNoteScope::Hunk, scene.Tab().path, gone, "on a change that is gone");
  workspace::ApplyBranchReviewPresentationMarkers(scene.Tab(), review);
  scene.fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the two comments");

  Expect(scene.NoteRows() == 2 && review.NoteCount(target) == 2, "both comments show");
  Expect(scene.Tab().review_hunk_markers[1].has_note && scene.Tab().review_hunk_markers[1].note_outdated &&
             workspace::CompareNoteCaption(scene.Tab(), 1).find("edited since") != std::string_view::npos,
         "the one whose change was edited is under it, and says so");
  Expect(scene.Button("compare/note-detached-0/edit").has_value(), "the one whose change is gone has a row of its own");

  scene.Click("compare/note-1/edit", "editing the outdated comment");
  Expect(TestAccess::CurrentPromptSurface(scene.fixture.shell).input.text() == "before the edit",
         "Edit opens it");
  scene.TypeComment("still wrong", "saving it");
  Expect(review.NoteCount(target) == 2 && scene.Tab().review_hunk_markers[1].note_text == "still wrong" &&
             !scene.Tab().review_hunk_markers[1].note_outdated,
         "saved, it is the change's comment as it stands: re-pinned, not doubled");

  // A comment too long for its row shows whole in a tooltip over the row.
  scene.Click("compare/note-1/edit", "editing it again");
  const std::string long_comment(400, 'w');
  scene.TypeComment(long_comment.c_str(), "saving a comment wider than the diff");
  if (const std::optional<SDL_FRect> edit = scene.Button("compare/note-1/edit"); edit.has_value()) {
    scene.fixture.ExpectFresh(
        DispatchEvents(scene.fixture.shell, {MouseMotionEvent(edit->x - 200.0f, edit->y + edit->h / 2.0f)}),
        "the pointer over the long comment");
    Expect(TestAccess::HoveredTooltipLabel(scene.fixture.shell).find("www") != std::string::npos,
           "the row's tooltip carries the comment: " + TestAccess::HoveredTooltipLabel(scene.fixture.shell));
    scene.fixture.ExpectFresh(DispatchEvents(scene.fixture.shell, {MouseMotionEvent(4.0f, 4.0f)}),
                              "the pointer away again");
  }

  scene.Click("compare/note-detached-0/delete", "deleting the detached comment");
  Expect(scene.NoteRows() == 1 && review.NoteCount(target) == 1, "Delete removes it from the review");
}

// A narrow window keeps what matters: the change's Mark Reviewed (Comment goes
// first), and the header's progress count ahead of its buttons. It kept Comment
// and dropped the mark, and cut the summary to "Bra..." under three buttons.
void TestANarrowDiffKeepsTheMarkAndTheProgress() {
  ReviewScene scene(Lines(30), Lines(30, 3, 25), 700, 560);
  Expect(scene.Button("compare/hunk-0/toggle-reviewed").has_value(),
         "the change keeps Mark Reviewed");
  const std::optional<SDL_FRect> toggle = scene.Button("compare/hunk-0/toggle-reviewed");
  const std::optional<SDL_FRect> comment = scene.Button("compare/hunk-0/comment");
  Expect(!comment.has_value() || (toggle.has_value() && comment->x > toggle->x),
         "Comment shows only beside it");
  const workspace::WorkspaceShell::CompareSurfaceLayout layout =
      TestAccess::ActiveCompareSurfaceLayout(scene.fixture.shell);
  const std::optional<SDL_FRect> leftmost = scene.Button("compare/file-reviewed").has_value()
                                                ? scene.Button("compare/file-reviewed")
                                                : scene.Button("compare/previous-change");
  if (leftmost.has_value()) {
    // The summary is wider than half this header, so half is what it keeps.
    const float pane_width = layout.right_x + layout.gutter_width + layout.right_width - layout.left_x;
    Expect(leftmost->x - layout.left_x >= pane_width * 0.4f, "the header's buttons leave the summary its room");
  }
}

// A repository with linked worktrees says how many in the status bar, and a
// click on it switches between them; with just the one tree it says nothing.
void TestStatusBarCountsTheWorktrees() {
  FlowFixture fixture("calc.txt", Lines(3), [](const std::filesystem::path& root) {
    InitializeGitRepo(root);
    CommitAll(root, "base", "base");
  });
  WorkspaceShell& shell = fixture.shell;
  TestAccess::ShowGitSidebar(shell);
  Expect(SettleGitSidebarRefresh(shell), "the git view settles");
  TestAccess::RefreshStatusBar(shell);
  Expect(TestAccess::StatusBarSegmentText(shell, workspace::StatusBarSegmentId::Worktrees).empty(),
         "one working tree: no segment");

  const std::filesystem::path agent = fixture.temp_dir.path() / "agent-tree";
  RequireGitCommandSuccess(fixture.root, {"worktree", "add", "-b", "agent", agent.string()}, "a linked tree");
  Expect(TestAccess::ExecuteCommandLine(shell, "git-refresh"), "a refresh");
  Expect(SettleGitSidebarRefresh(shell), "the refresh lands");
  TestAccess::RefreshStatusBar(shell);
  Expect(TestAccess::StatusBarSegmentText(shell, workspace::StatusBarSegmentId::Worktrees) == "2 worktrees",
         "two trees: " + TestAccess::StatusBarSegmentText(shell, workspace::StatusBarSegmentId::Worktrees));
  Expect(TestAccess::StatusBarSegmentTooltip(shell, workspace::StatusBarSegmentId::Worktrees) == "Switch Worktree…",
         "a click switches worktree");

  // A tree whose directory is gone is prunable, not one to switch to.
  std::filesystem::remove_all(agent);
  Expect(TestAccess::ExecuteCommandLine(shell, "git-refresh"), "another refresh");
  Expect(SettleGitSidebarRefresh(shell), "it lands");
  TestAccess::RefreshStatusBar(shell);
  Expect(TestAccess::StatusBarSegmentText(shell, workspace::StatusBarSegmentId::Worktrees).empty(),
         "the prunable tree does not count");
}

}  // namespace

void RegisterReviewControlsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ReviewControls/HunkButtonsMarkAndUnmarkAChange", TestHunkButtonsMarkAndUnmarkAChange);
  AddTest(tests, "ReviewControls/CommentsShowUnderTheirChange", TestCommentsShowUnderTheirChange);
  AddTest(tests, "ReviewControls/CommentPromptPinsItsChange", TestCommentPromptPinsItsChange);
  AddTest(tests, "ReviewControls/HeaderStepsChangesAndMarksTheFile", TestHeaderStepsChangesAndMarksTheFile);
  AddTest(tests, "ReviewControls/AStagingDiffStepsButTakesNoMarks", TestAStagingDiffStepsButTakesNoMarks);
  AddTest(tests, "ReviewControls/ALongChangeKeepsItsButtonsOnScreen", TestALongChangeKeepsItsButtonsOnScreen);
  AddTest(tests, "ReviewControls/ScrollingMovesTheHoveredChange", TestScrollingMovesTheHoveredChange);
  AddTest(tests, "ReviewControls/ACommentOutlivesItsChange", TestACommentOutlivesItsChange);
  AddTest(tests, "ReviewControls/ANarrowDiffKeepsTheMarkAndTheProgress", TestANarrowDiffKeepsTheMarkAndTheProgress);
  AddTest(tests, "ReviewControls/CommentsAreHandedOverFromTheGitMenuAndTheGitView",
          TestCommentsAreHandedOverFromTheGitMenuAndTheGitView);
  AddTest(tests, "ReviewControls/StatusBarCountsTheWorktrees", TestStatusBarCountsTheWorktrees);
}

}  // namespace microide::tests
