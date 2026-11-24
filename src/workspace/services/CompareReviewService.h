#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "compare/CompareReviewTypes.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

// A review's marks and comments, as the diff's controls, the palette commands and
// the Git menu act on them: marking a hunk or a file reviewed, writing, editing
// and deleting a comment (a review note), and handing every comment of a review
// on as `path:line: comment` lines.
//
// The review is the active diff's when it takes review marks
// (CompareTabTakesReviewMarks); with none in front, the repository's review that
// was touched last -- so the Git menu's export works from anywhere.
class CompareReviewService {
 public:
  struct Operations {
    std::function<CompareTabState*()> active_compare_tab;
    // Saves the review state (and, for a remote project, tells the host).
    std::function<void()> persist;
    // The diff's marks and notes are its rows' and header's keys: a frame.
    std::function<void()> request_frame;
    // The git view's row markers follow the marks; it is not derived yet.
    std::function<void()> request_sidebar_redraw;
    std::function<void(PromptSurfaceService::ReviewCommentPrompt)> open_comment_prompt;
    // Moves the active diff's selection to its previous (-1) or next (+1) change.
    std::function<void(int)> jump_compare_hunk;
  };

  CompareReviewService(ProjectWorkspaceState& state, const PromptState& prompts, Operations operations);

  // ---- the palette commands, on the active diff's selection ----
  void SetSelectedHunkReviewed(bool reviewed);
  void SetActiveFileReviewed(bool reviewed);
  // Replaces the note of the selected hunk (or the file's, off any change); empty
  // deletes it.
  void EditSelectedNote(const std::string& note_text);
  void ClearReview();

  // ---- the diff's controls, on `compare_tab` ----
  void SetHunkReviewed(CompareTabState& compare_tab, int hunk, bool reviewed);
  void SetFileReviewed(CompareTabState& compare_tab, bool reviewed);
  // `note` below names a note's place as a ReviewNote row does: a hunk, the
  // file (compare::kCompareFileNoteHunk), or a detached note
  // (compare::CompareDetachedNoteHunk) -- one whose change is gone.
  //
  // Opens the comment prompt on `note`, seeded with the note there.
  void OpenCommentPrompt(CompareTabState& compare_tab, int note);
  // The prompt's Save: the typed text becomes the pinned place's note.
  void CommitCommentPrompt();
  void DeleteComment(CompareTabState& compare_tab, int note);
  // Runs the control a click landed on. False: not a review or header control.
  bool RunControl(CompareTabState& compare_tab, const CompareControlHit& hit);

  // ---- export ----
  // (ReviewExportTarget / ReviewCommentCount below answer the same from const
  // state, for a menu label.)
  // The review the export acts on, or nullopt when there is none.
  std::optional<compare::BranchReviewTargetIdentity> ExportTarget() const;
  // The comments on `scope` (a file or directory, absolute or repository-
  // relative; empty: all) of that review.
  std::size_t CommentCount(const std::filesystem::path& scope = {}) const;
  std::string CommentsAsReferences(const std::filesystem::path& scope = {}) const;

 private:
  // The marks moved: every diff of the project re-resolves its markers (a file
  // can be open in two), then the state is saved and repainted.
  void MarksChanged();

  CompareTabState* ActiveTab() const;
  // The active diff, when it takes review marks.
  CompareTabState* ActiveReviewTab() const;

  ProjectWorkspaceState& state_;
  const PromptState& prompts_;
  Operations operations_;
};

// The review an export acts on: `active_compare_tab`'s when it takes review
// marks, else the repository's review touched last; nullopt when there is none.
std::optional<compare::BranchReviewTargetIdentity> ReviewExportTarget(const ProjectWorkspaceState& state,
                                                                      const CompareTabState* active_compare_tab);
// How many comments that review has under `scope` (empty: all of them).
std::size_t ReviewCommentCount(const ProjectWorkspaceState& state,
                               const CompareTabState* active_compare_tab,
                               const std::filesystem::path& scope);

}  // namespace microide::workspace
