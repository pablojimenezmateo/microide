#include "workspace/CommitWorkflowState.h"

namespace microide::workspace {

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport) {
  std::string body;
  const auto& lines = viewport.lines().Snapshot();
  for (std::size_t line = 0; line < lines.size(); ++line) {
    if (line > 0) {
      body.push_back('\n');
    }
    body += lines[line];
  }
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
