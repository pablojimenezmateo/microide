#include "workspace/CommitWorkflowState.h"

namespace microide::workspace {

std::string CommitWorkflowBodyText(const editor::TextViewport& viewport) {
  std::string body;
  const auto& lines = viewport.lines();
  for (std::size_t line = 0; line < lines.size(); ++line) {
    if (line > 0) {
      body.push_back('\n');
    }
    body += lines[line];
  }
  return body;
}

}  // namespace microide::workspace
