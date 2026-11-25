#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

#include <SDL3/SDL.h>

#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "project/CommitWorkflowTypes.h"
#include "workspace/git/CommitWorkflowPersistence.h"

namespace microide::workspace {

class CommitWorkflowService;

class CommitOperationClaim {
 public:
  CommitOperationClaim() = default;
  CommitOperationClaim(CommitWorkflowService* service, std::uint64_t generation)
      : service_(service), generation_(generation) {}
  ~CommitOperationClaim();

  CommitOperationClaim(CommitOperationClaim&& other) noexcept
      : service_(other.service_), generation_(other.generation_) {
    other.service_ = nullptr;
    other.generation_ = 0;
  }
  CommitOperationClaim& operator=(CommitOperationClaim&& other) noexcept;

  CommitOperationClaim(const CommitOperationClaim&) noexcept {}
  CommitOperationClaim& operator=(const CommitOperationClaim&) noexcept;

  bool Holds(const CommitWorkflowService* service, std::uint64_t generation) const {
    return service != nullptr && service_ == service && generation != 0 && generation_ == generation;
  }

  void Release() {
    service_ = nullptr;
    generation_ = 0;
  }

 private:
  void Cancel();

  CommitWorkflowService* service_ = nullptr;
  std::uint64_t generation_ = 0;
};

enum class CommitWorkflowFocusField {
  Subject,
  Body,
};

enum class CommitWorkflowPendingConfirmation {
  None,
  Amend,
  NoVerify,
  Warnings,
};

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport);
inline void ClearCommitBodyHint(editor::TextViewport& body) {
  if (body.is_placeholder()) {
    body.LoadContent({});
  }
}

struct CommitWorkflowState {
  bool open = false;
  std::optional<PersistedCommitDraftState> loaded_persisted_draft;
  bool draft_restored = false;
  bool operation_in_flight = false;
  CommitWorkflowFocusField focus_field = CommitWorkflowFocusField::Subject;
  CommitWorkflowPendingConfirmation pending_confirmation = CommitWorkflowPendingConfirmation::None;
  project::CommitOperationKind pending_operation = project::CommitOperationKind::Create;
  editor::SingleLineEditor subject;
  editor::TextViewport body;
  project::CommitDraftContext draft_context;
  project::CommitStagedSummary staged_summary;
  std::string staged_summary_line;
  std::uint64_t staged_summary_generation = std::numeric_limits<std::uint64_t>::max();
  std::vector<project::CommitPreCheck> checks;
  std::unordered_set<std::string> acknowledged_warning_ids;
  std::string status_message;
  std::string last_hook_output;
  project::CommitOperationResultCategory last_result_category =
      project::CommitOperationResultCategory::Success;
  SDL_FRect subject_field_rect{};
  SDL_FRect body_field_rect{};
  SDL_FRect commit_button_rect{};
  SDL_FRect caret_rect{};
  int body_visible_rows = 0;

  const std::string& BodyText() const;

  CommitOperationClaim in_flight_claim_;

 private:
  mutable std::string body_text_cache_;
  mutable std::uint64_t body_text_cache_revision_ = std::numeric_limits<std::uint64_t>::max();
};

PersistedCommitDraftState MakePersistedCommitDraft(const CommitWorkflowState& state);

}
