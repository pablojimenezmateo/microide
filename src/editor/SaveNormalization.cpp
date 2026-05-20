#include "editor/SaveNormalization.h"

namespace microide::editor {

bool TrimTrailingWhitespace(std::vector<std::string>& lines) {
  bool any = false;
  for (std::string& line : lines) {
    std::size_t end = line.size();
    while (end > 0) {
      char c = line[end - 1];
      if (c != ' ' && c != '\t') break;
      --end;
    }
    if (end != line.size()) {
      line.resize(end);
      any = true;
    }
  }
  return any;
}

bool EnsureSingleFinalNewline(std::vector<std::string>& lines) {
  if (lines.empty()) {
    lines.emplace_back();
    return true;
  }
  bool changed = false;
  while (lines.size() > 1 && lines.back().empty() && lines[lines.size() - 2].empty()) {
    lines.pop_back();
    changed = true;
  }
  if (lines.empty() || !lines.back().empty()) {
    lines.emplace_back();
    changed = true;
  }
  return changed;
}

}  // namespace microide::editor
