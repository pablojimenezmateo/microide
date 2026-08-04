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

// RAII claim on an in-flight background commit.
//
// A dispatched commit runs on a worker thread and posts a completion that resolves
// back to the CommitWorkflowState that started it. That state lives inside a
// ProjectWorkspaceState, which is not stable: closing a project tab destroys it,
// resetting to the welcome screen move-assigns a fresh one over it, and erasing a
// catalog entry shifts every later entry down by one move-assignment. All three
// leave the completion pointing at storage that is gone or now belongs to a
// different project — a use-after-free in the first case and a result published
// into the wrong project's commit panel in the others.
//
// Holding the claim as a member means the compiler routes every one of those paths
// through this type's destructor or move, so none of them can be forgotten. The
// commit itself still runs to completion on disk; only the UI publication is
// dropped, which is the same outcome as being superseded by a newer dispatch.
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

  // A copy is a different object and therefore claims nothing: the operation is
  // identified by the address the completion resolves to, and only one object can
  // be at that address.
  CommitOperationClaim(const CommitOperationClaim&) noexcept {}
  CommitOperationClaim& operator=(const CommitOperationClaim&) noexcept;

  // Gives up the claim without cancelling — used when the completion has already
  // published, so the operation is finished rather than abandoned.
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
  // Non-blocking pre-check warnings are pending acknowledgement. Unlike Amend /
  // NoVerify the confirmation does not itself name the operation, so the requested
  // one is parked in CommitWorkflowState::pending_operation.
  Warnings,
};

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport);

struct CommitWorkflowState {
  bool open = false;
  std::optional<PersistedCommitDraftState> loaded_persisted_draft;
  bool draft_restored = false;
  bool operation_in_flight = false;
  CommitWorkflowFocusField focus_field = CommitWorkflowFocusField::Subject;
  CommitWorkflowPendingConfirmation pending_confirmation = CommitWorkflowPendingConfirmation::None;
  // The operation to run once a Warnings confirmation is accepted. Meaningless for
  // the other pending_confirmation values, which encode their own operation.
  project::CommitOperationKind pending_operation = project::CommitOperationKind::Create;
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

  // Held while a commit dispatched by this state is in flight. See
  // CommitOperationClaim: destroying, moving over, or copying this state gives the
  // claim up, so the queued completion is dropped rather than publishing into
  // storage that is gone or now belongs to a different project.
  //
  // Public rather than private-with-a-friend: workspace code does not use `friend`
  // (AGENTS.md § Do-Not-Regress Patterns), and there is no invariant for privacy to
  // protect here — the claim type enforces its own, and holding one is not a secret.
  CommitOperationClaim in_flight_claim_;

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
