#pragma once

#include <string>
#include <vector>

namespace microide::editor {

struct LanguagePair {
  std::string open;
  std::string close;
};

struct LanguageContractView {
  std::vector<LanguagePair> auto_close_pairs;
  std::vector<LanguagePair> surround_pairs;
  std::vector<std::string> indent_after_open_patterns;
  std::vector<std::string> dedent_on_close_chars;
  std::string line_comment;
  std::string block_comment_open;
  std::string block_comment_close;
  bool auto_close_enabled = false;
  bool surround_enabled = false;
  bool smart_indent_enabled = false;
  bool inhibit_pairs_in_strings = true;
  bool inhibit_pairs_in_comments = true;
};

inline const LanguageContractView kNoLanguageContractView;

}
