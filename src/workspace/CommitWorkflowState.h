#pragma once

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
  SDL_FRect caret_rect{};
  int body_visible_rows = 0;
};

}  // namespace microide::workspace
