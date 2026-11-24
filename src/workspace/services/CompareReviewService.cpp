#include "workspace/services/CompareReviewService.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "compare/BranchReviewStateTypes.h"
#include "util/StringUtil.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ShellGlueCache.h"
#include "workspace/shell/WorkspaceShell.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

bool IsBlank(std::string_view text) {
  return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// "src/calc.cpp, lines 19-20" -- where a comment prompt's note goes.
std::string DescribeCommentPlace(const std::filesystem::path& repository_root, const std::filesystem::path& path,
                                 const std::optional<compare::BranchReviewHunkIdentity>& hunk) {
  std::string place = compare::NormalizeReviewPath(repository_root, path).generic_string();
  if (!hunk.has_value()) {
    return place;
  }
  const bool removal_only = hunk->new_count == 0;
  const int start = removal_only ? hunk->old_start : hunk->new_start;
  const int count = removal_only ? hunk->old_count : hunk->new_count;
  place += removal_only ? ", removed line" : ", line";
  if (count > 1) {
    place += "s ";
    util::AppendUnsigned(place, static_cast<std::size_t>(std::max(0, start)));
    place += '-';
    util::AppendUnsigned(place, static_cast<std::size_t>(std::max(0, start + count - 1)));
  } else {
    place += ' ';
    util::AppendUnsigned(place, static_cast<std::size_t>(std::max(0, start)));
  }
  return place;
}

// Where a note named as a ReviewNote row names it is kept, and what is there.
struct NotePlace {
  // The identity a note written here is stored under; none for the file's.
  std::optional<compare::BranchReviewHunkIdentity> identity;
  // The identity the note shown here is stored under now, when that differs (its
  // change moved, or was edited since): writing here replaces it.
  std::optional<compare::BranchReviewHunkIdentity> replaced;
  std::string text;
};

// Nullopt when `note` names nothing the tab shows.
std::optional<NotePlace> ResolveNotePlace(const compare::BranchReviewStateService& review,
                                          const CompareTabState& compare_tab, int note) {
  if (note == compare::kCompareFileNoteHunk) {
    return NotePlace{.identity = std::nullopt, .replaced = std::nullopt, .text = compare_tab.review_file_note};
  }
  if (compare::IsCompareDetachedNoteHunk(note)) {
    // A detached note keeps the identity it has: there is no hunk to move it to.
    const std::size_t index = compare::CompareDetachedNoteIndex(note);
    if (index >= compare_tab.review_detached_notes.size()) {
      return std::nullopt;
    }
    const compare::BranchReviewNote& detached = compare_tab.review_detached_notes[index];
    return NotePlace{.identity = detached.hunk_identity, .replaced = std::nullopt, .text = detached.text};
  }
  if (static_cast<std::size_t>(note) >= compare_tab.model.hunks.size()) {
    return std::nullopt;
  }
  NotePlace place{.identity = compare::ComputeBranchReviewHunkIdentity(compare_tab.model, note, compare_tab.path),
                  .replaced = std::nullopt,
                  .text = {}};
  if (const compare::BranchReviewNote* shown = review.NoteOnHunk(
          {.target = compare_tab.branch_target, .path = compare_tab.path, .model = &compare_tab.model,
           .selected_hunk_index = note})) {
    place.text = shown->text;
    const compare::BranchReviewHunkIdentity& stored = *shown->hunk_identity;
    if (stored.old_start != place.identity->old_start || stored.new_start != place.identity->new_start ||
        stored.old_count != place.identity->old_count || stored.new_count != place.identity->new_count ||
        stored.content_hash != place.identity->content_hash) {
      place.replaced = stored;
    }
  }
  return place;
}

// Every way a note is written ends here: the note `replaced` names goes, and
// blank text deletes the one at `hunk` instead of writing it.
void WriteNote(compare::BranchReviewStateService& review, const compare::BranchReviewTargetIdentity& target,
               const std::filesystem::path& path, const std::optional<compare::BranchReviewHunkIdentity>& hunk,
               const std::optional<compare::BranchReviewHunkIdentity>& replaced, std::string_view text) {
  const compare::BranchReviewNoteScope scope =
      hunk.has_value() ? compare::BranchReviewNoteScope::Hunk : compare::BranchReviewNoteScope::File;
  if (replaced.has_value()) {
    review.DeleteNote(target, compare::BranchReviewNoteScope::Hunk, path, replaced);
  }
  if (IsBlank(text)) {
    review.DeleteNote(target, scope, path, hunk);
  } else {
    review.SetNote(target, scope, path, hunk, text);
  }
}

}  // namespace

CompareReviewService::CompareReviewService(ProjectWorkspaceState& state, const PromptState& prompts,
                                           Operations operations)
    : state_(state), prompts_(prompts), operations_(std::move(operations)) {}

CompareTabState* CompareReviewService::ActiveTab() const {
  return operations_.active_compare_tab ? operations_.active_compare_tab() : nullptr;
}

CompareTabState* CompareReviewService::ActiveReviewTab() const {
  CompareTabState* compare_tab = ActiveTab();
  return compare_tab != nullptr && CompareTabTakesReviewMarks(*compare_tab) ? compare_tab : nullptr;
}

void CompareReviewService::MarksChanged() {
  for (EditorGroup& group : state_.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
        ApplyBranchReviewPresentationMarkers(*tab.compare, state_.branch_review);
      }
    }
  }
  util::InvokeIfSet(operations_.persist);
  util::InvokeIfSet(operations_.request_frame);
  util::InvokeIfSet(operations_.request_sidebar_redraw);
}

void CompareReviewService::SetHunkReviewed(CompareTabState& compare_tab, int hunk, bool reviewed) {
  if (!CompareTabTakesReviewMarks(compare_tab) || hunk < 0 ||
      static_cast<std::size_t>(hunk) >= compare_tab.model.hunks.size()) {
    return;
  }
  state_.branch_review.SetHunkReviewed(compare_tab.branch_target, compare_tab.path, compare_tab.model, hunk,
                                       reviewed);
  MarksChanged();
}

void CompareReviewService::SetFileReviewed(CompareTabState& compare_tab, bool reviewed) {
  if (!CompareTabTakesReviewMarks(compare_tab)) {
    return;
  }
  // Every hunk follows the file (BranchReviewStateService::SetFileReviewed): a
  // hunk marked on its own, or changed since, would otherwise keep the file from
  // reading reviewed, or keep it reviewed after "Unmark File".
  state_.branch_review.SetFileReviewed(compare_tab.branch_target, compare_tab.path, compare_tab.model, reviewed);
  MarksChanged();
}

void CompareReviewService::OpenCommentPrompt(CompareTabState& compare_tab, int note) {
  if (!CompareTabTakesReviewMarks(compare_tab) || !operations_.open_comment_prompt) {
    return;
  }
  std::optional<NotePlace> place = ResolveNotePlace(state_.branch_review, compare_tab, note);
  if (!place.has_value()) {
    return;
  }
  const bool editing = !place->text.empty();
  std::string detail = (editing ? "Edit the comment on " : "Comment on ") +
                       DescribeCommentPlace(state_.root, compare_tab.path, place->identity) +
                       (editing ? ". Empty deletes it." : ".");
  operations_.open_comment_prompt({
      .path = compare_tab.path,
      .text = std::move(place->text),
      .target = compare_tab.branch_target,
      .hunk = std::move(place->identity),
      .replaced_hunk = std::move(place->replaced),
      .detail = std::move(detail),
  });
}

void CompareReviewService::CommitCommentPrompt() {
  const PromptSurfaceState& prompt = prompts_.surface;
  if (prompt.action != PromptSurfaceState::Action::ReviewComment) {
    return;
  }
  WriteNote(state_.branch_review, prompt.review_target, prompt.path, prompt.review_hunk, prompt.review_replaced_hunk,
            prompt.input.text());
  MarksChanged();
}

void CompareReviewService::DeleteComment(CompareTabState& compare_tab, int note) {
  if (!CompareTabTakesReviewMarks(compare_tab)) {
    return;
  }
  const std::optional<NotePlace> place = ResolveNotePlace(state_.branch_review, compare_tab, note);
  if (!place.has_value()) {
    return;
  }
  WriteNote(state_.branch_review, compare_tab.branch_target, compare_tab.path, place->identity, place->replaced, {});
  MarksChanged();
}

bool CompareReviewService::RunControl(CompareTabState& compare_tab, const CompareControlHit& hit) {
  switch (hit.kind) {
    case CompareHoverKind::HunkToggleReviewed:
      SetHunkReviewed(compare_tab, hit.hunk,
                      CompareHunkReviewStatus(compare_tab, hit.hunk) != compare::BranchReviewMarkerStatus::Reviewed);
      return true;
    case CompareHoverKind::HunkComment:
    case CompareHoverKind::NoteEdit:
      OpenCommentPrompt(compare_tab, hit.hunk);
      return true;
    case CompareHoverKind::NoteDelete:
      DeleteComment(compare_tab, hit.hunk);
      return true;
    case CompareHoverKind::HeaderPreviousChange:
    case CompareHoverKind::HeaderNextChange:
      if (operations_.jump_compare_hunk) {
        operations_.jump_compare_hunk(hit.kind == CompareHoverKind::HeaderNextChange ? 1 : -1);
      }
      return true;
    case CompareHoverKind::HeaderToggleFileReviewed:
      SetFileReviewed(compare_tab, !CompareFileFullyReviewed(compare_tab));
      return true;
    case CompareHoverKind::CollapsedContextPreviousAction:
    case CompareHoverKind::CollapsedContextAllAction:
    case CompareHoverKind::CollapsedContextNextAction:
      return false;
  }
  return false;
}

void CompareReviewService::SetSelectedHunkReviewed(bool reviewed) {
  if (CompareTabState* compare_tab = ActiveTab()) {
    SetHunkReviewed(*compare_tab, CompareTabSelectedHunkIndex(*compare_tab), reviewed);
  }
}

void CompareReviewService::SetActiveFileReviewed(bool reviewed) {
  if (CompareTabState* compare_tab = ActiveTab()) {
    SetFileReviewed(*compare_tab, reviewed);
  }
}

void CompareReviewService::EditSelectedNote(const std::string& note_text) {
  CompareTabState* compare_tab = ActiveReviewTab();
  if (compare_tab == nullptr) {
    return;
  }
  // The selected hunk's note, or the file's off any change.
  const std::optional<NotePlace> place =
      ResolveNotePlace(state_.branch_review, *compare_tab,
                       std::max(compare::kCompareFileNoteHunk, CompareTabSelectedHunkIndex(*compare_tab)));
  if (!place.has_value()) {
    return;
  }
  WriteNote(state_.branch_review, compare_tab->branch_target, compare_tab->path, place->identity, place->replaced,
            note_text);
  MarksChanged();
}

void CompareReviewService::ClearReview() {
  if (const CompareTabState* compare_tab = ActiveReviewTab()) {
    state_.branch_review.ClearTarget(compare_tab->branch_target);
  } else if (const std::optional<compare::BranchReviewTargetIdentity> target =
                 OutgoingBranchReviewTarget(state_.sidebar.git, state_.root);
             target.has_value()) {
    state_.branch_review.ClearTarget(*target);
  }
  MarksChanged();
}

std::optional<compare::BranchReviewTargetIdentity> ReviewExportTarget(const ProjectWorkspaceState& state,
                                                                      const CompareTabState* active_compare_tab) {
  if (active_compare_tab != nullptr && CompareTabTakesReviewMarks(*active_compare_tab)) {
    return active_compare_tab->branch_target;
  }
  if (state.root.empty()) {
    return std::nullopt;
  }
  if (const compare::BranchReviewTargetState* recent = state.branch_review.MostRecentTarget(state.root)) {
    return recent->target;
  }
  return std::nullopt;
}

std::size_t ReviewCommentCount(const ProjectWorkspaceState& state, const CompareTabState* active_compare_tab,
                               const std::filesystem::path& scope) {
  const std::optional<compare::BranchReviewTargetIdentity> target = ReviewExportTarget(state, active_compare_tab);
  return target.has_value() ? state.branch_review.NoteCount(*target, scope) : 0;
}

std::optional<compare::BranchReviewTargetIdentity> CompareReviewService::ExportTarget() const {
  return ReviewExportTarget(state_, ActiveTab());
}

std::size_t CompareReviewService::CommentCount(const std::filesystem::path& scope) const {
  return ReviewCommentCount(state_, ActiveTab(), scope);
}

std::string CompareReviewService::CommentsAsReferences(const std::filesystem::path& scope) const {
  const std::optional<compare::BranchReviewTargetIdentity> target = ExportTarget();
  return target.has_value() ? state_.branch_review.NotesAsReferences(*target, scope) : std::string();
}

CompareReviewService& WorkspaceShell::MakeCompareReviewService() {
  if (glue_->compare_review_service != nullptr) {
    return *glue_->compare_review_service;
  }
  glue_->compare_review_service = std::make_unique<CompareReviewService>(
      context_.current_project_state, context_.prompts,
      CompareReviewService::Operations{
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .persist =
              [this]() {
                MakePersistenceCoordinator().SaveConfigState();
                // A remote project's marks follow the user to their other machine.
                if (context_.current_project_state.id.IsRemote()) {
                  MakeRemoteHostService().ReviewChanged(context_.current_project_state.root);
                }
              },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
          .open_comment_prompt =
              [this](PromptSurfaceService::ReviewCommentPrompt prompt) {
                MakePromptSurfaceService().OpenReviewCommentPrompt(std::move(prompt));
              },
          .jump_compare_hunk = [this](int delta) { JumpCompareHunk(delta); },
      });
  return *glue_->compare_review_service;
}

}  // namespace microide::workspace
