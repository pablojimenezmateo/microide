#pragma once

#include <string>
#include <vector>

namespace microide::editor {

struct LanguagePair {
  std::string open;
  std::string close;
};

// Owning POD-like value type that carries the per-language data the editor
// needs for auto-close, surround, smart indent, dedent-on-close, and split
// braces. Workspace builds the view from `WorkspaceLanguageContract` (plus
// user/project setting overrides) and pushes it into the active tab via
// `TextViewport::SetLanguageContractView(...)`.
//
// Default-constructed view leaves every behavior off; the editor falls back to
// its existing literal-insertion path. Tests opt in by constructing a view
// with the relevant flags set.
struct LanguageContractView {
  std::vector<LanguagePair> auto_close_pairs;
  std::vector<LanguagePair> surround_pairs;
  // Each pattern is matched against the trimmed end of the previous line; if
  // any matches, the auto-indent path appends one indent unit. Patterns are
  // simple suffix tokens like "{", "(", "[", or ":".
  std::vector<std::string> indent_after_open_patterns;
  // Single-character close tokens that, when typed on an indent-only line with
  // a positive leading indent, drop one indent unit before the character is
  // inserted.
  std::vector<std::string> dedent_on_close_chars;
  std::string line_comment;
  std::string block_comment_open;
  std::string block_comment_close;
  bool auto_close_enabled = false;
  bool surround_enabled = false;
  bool smart_indent_enabled = false;
  // Reserved for future inhibit logic; not consulted in v1.
  bool inhibit_pairs_in_strings = true;
  bool inhibit_pairs_in_comments = true;
};

}  // namespace microide::editor
