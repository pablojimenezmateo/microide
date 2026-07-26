#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

#include <SDL3/SDL.h>

#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "project/CommitWorkflowTypes.h"
#include "workspace/CommitWorkflowPersistence.h"

namespace microide::workspace {

enum class CommitWorkflowFocusField {
  Subject,
  Body,
};

enum class CommitWorkflowPendingConfirmation {
  None,
  Amend,
  NoVerify,
};

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport);

struct CommitWorkflowState {
  bool open = false;
  std::optional<PersistedCommitDraftState> loaded_persisted_draft;
  bool draft_restored = false;
  bool operation_in_flight = false;
  CommitWorkflowFocusField focus_field = CommitWorkflowFocusField::Subject;
  CommitWorkflowPendingConfirmation pending_confirmation = CommitWorkflowPendingConfirmation::None;
  editor::SingleLineEditor subject;
  editor::TextViewport body;
  project::CommitDraftContext draft_context;
  project::CommitStagedSummary staged_summary;
  std::string staged_summary_line;
  // Git generation the cached staged_summary was built against. The staged summary
  // depends only on the git index, so RefreshDerivedState rebuilds it (a
  // `git diff --cached --numstat` subprocess) only when the generation changes, not
  // on every field switch / warning-ack refresh. SIZE_MAX-sentinel forces a rebuild.
  std::uint64_t staged_summary_generation = std::numeric_limits<std::uint64_t>::max();
  std::vector<project::CommitPreCheck> checks;
  std::unordered_set<std::string> acknowledged_warning_ids;
  std::string status_message;
  std::string last_hook_output;
  project::CommitOperationResultCategory last_result_category =
      project::CommitOperationResultCategory::Success;
  // On-screen geometry of the subject/body edit fields, recomputed by the sidebar render
  // each frame. The commit panel sits below a content-dynamic git summary, so its field
  // positions can't be a pure layout function; mouse hit-testing and caret placement read
  // these cached rects (kept divergence-free by being written from the single render path).
  SDL_FRect subject_field_rect{};
  SDL_FRect body_field_rect{};
  SDL_FRect commit_button_rect{};
  SDL_FRect caret_rect{};
  int body_visible_rows = 0;

  // Serialized commit body, memoized against the body viewport's content
  // revision. Typing in the body runs a precheck (RefreshDerivedState) and can
  // then persist the draft; both need the full body string, so without this
  // cache each keystroke re-snapshots and re-concatenates the whole (possibly
  // large) body (A-075). content_revision advances on every content edit, so a
  // matching revision guarantees the cached string is current.
  const std::string& BodyText() const;

 private:
  mutable std::string body_text_cache_;
  mutable std::uint64_t body_text_cache_revision_ = std::numeric_limits<std::uint64_t>::max();
};

// The ONE place a CommitWorkflowState is mapped onto its persisted form. Both the
// service and the project-config writer used to spell these field assignments out
// separately; designated initializers silently value-initialize a field nobody
// assigns, so a new member on PersistedCommitDraftState would have been dropped by
// whichever copy was not updated, with no compiler error.
PersistedCommitDraftState MakePersistedCommitDraft(const CommitWorkflowState& state);

}  // namespace microide::workspace
