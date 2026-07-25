#include "workspace/CommitWorkflowState.h"

namespace microide::workspace {

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport) {
  // The piece tree already stores the body '\n'-joined; taking it directly skips
  // the Snapshot() vector-of-strings (one allocation per line) this used to
  // materialize just to re-join it.
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

}  // namespace microide::workspace
