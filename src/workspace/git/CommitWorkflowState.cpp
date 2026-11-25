#include "workspace/git/CommitWorkflowState.h"

#include "workspace/git/CommitWorkflowService.h"

namespace microide::workspace {

void CommitOperationClaim::Cancel() {
  if (service_ != nullptr) {
    service_->AbandonOperation(generation_);
  }
  service_ = nullptr;
  generation_ = 0;
}

CommitOperationClaim::~CommitOperationClaim() { Cancel(); }

CommitOperationClaim& CommitOperationClaim::operator=(CommitOperationClaim&& other) noexcept {
  if (this != &other) {
    Cancel();
    service_ = other.service_;
    generation_ = other.generation_;
    other.service_ = nullptr;
    other.generation_ = 0;
  }
  return *this;
}

CommitOperationClaim& CommitOperationClaim::operator=(const CommitOperationClaim&) noexcept {
  Cancel();
  return *this;
}

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport) {
  if (viewport.is_placeholder()) {
    return {};
  }
  std::string body;
  viewport.lines().AppendWholeText(body);
  return body;
}

const std::string& CommitWorkflowState::BodyText() const {
  const std::uint64_t revision = body.content_revision();
  if (body_text_cache_revision_ != revision) {
    body_text_cache_ = CommitWorkflowBodyText(body);
    body_text_cache_revision_ = revision;
  }
  return body_text_cache_;
}

PersistedCommitDraftState MakePersistedCommitDraft(const CommitWorkflowState& state) {
  return PersistedCommitDraftState{
      .head_oid = state.draft_context.head_oid,
      .branch_name = state.draft_context.branch_name,
      .subject = state.subject.text(),
      .body = state.BodyText(),
  };
}

}
