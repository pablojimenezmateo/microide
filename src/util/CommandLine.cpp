#include "util/CommandLine.h"

namespace microide::util {

namespace {

bool IsAsciiSpace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

}  // namespace

std::vector<std::string> SplitCommandLine(std::string_view text) {
  std::vector<std::string> words;
  std::string word;
  bool in_word = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (IsAsciiSpace(ch)) {
      if (in_word) {
        words.push_back(std::move(word));
        word.clear();
        in_word = false;
      }
      continue;
    }
    in_word = true;
    if (ch == '\'') {
      for (++i; i < text.size() && text[i] != '\''; ++i) {
        word.push_back(text[i]);
      }
      continue;  // an unterminated quote ends the word at end-of-input
    }
    if (ch == '"') {
      for (++i; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i + 1] == '"' || text[i + 1] == '\\' || text[i + 1] == '$')) {
          ++i;
        }
        word.push_back(text[i]);
      }
      continue;
    }
    if (ch == '\\' && i + 1 < text.size()) {
      word.push_back(text[++i]);
      continue;
    }
    word.push_back(ch);
  }
  if (in_word) {
    words.push_back(std::move(word));
  }
  return words;
}

}  // namespace microide::util
