#include "editor/CompletionFilter.h"

namespace microide::editor {

namespace {

bool IsAsciiAlnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool IsAsciiUpper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool IsAsciiLower(unsigned char c) { return c >= 'a' && c <= 'z'; }
unsigned char FoldAscii(unsigned char c) {
  return IsAsciiUpper(c) ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

// A word starts at the text's start, after any non-alphanumeric ASCII byte (`_`,
// `.`, `-`, `:`, a space), and at a camelCase hump (lower or digit, then upper).
// Bytes >= 0x80 count as word characters, so a UTF-8 identifier is one word.
bool IsWordStart(std::string_view text, std::size_t i) {
  if (i == 0) {
    return true;
  }
  const auto prev = static_cast<unsigned char>(text[i - 1]);
  const auto cur = static_cast<unsigned char>(text[i]);
  if (prev < 0x80 && !IsAsciiAlnum(prev)) {
    return true;
  }
  return IsAsciiUpper(cur) && (IsAsciiLower(prev) || (prev >= '0' && prev <= '9'));
}

}  // namespace

int CompletionMatchScore(std::string_view candidate, std::string_view query) {
  if (query.empty()) {
    return 0;
  }
  if (query.size() > candidate.size()) {
    return kNoCompletionMatch;
  }
  const unsigned char first = FoldAscii(static_cast<unsigned char>(query[0]));
  // The first query byte must land on a word start. Try each such position in
  // turn: `ab` against `xAb_ab` must not give up because the first word-start `a`
  // happens to be followed by a poorer tail.
  int best = kNoCompletionMatch;
  for (std::size_t start = 0; start + query.size() <= candidate.size(); ++start) {
    if (FoldAscii(static_cast<unsigned char>(candidate[start])) != first ||
        !IsWordStart(candidate, start)) {
      continue;
    }
    int score = start == 0 ? 8 : 6;
    if (candidate[start] == query[0]) {
      score += 1;
    }
    std::size_t c = start + 1;
    std::size_t q = 1;
    bool previous_matched = true;
    for (; q < query.size() && c < candidate.size(); ++c) {
      const auto cb = static_cast<unsigned char>(candidate[c]);
      const auto qb = static_cast<unsigned char>(query[q]);
      if (FoldAscii(cb) == FoldAscii(qb)) {
        if (previous_matched) {
          score += 5;
        } else if (IsWordStart(candidate, c)) {
          score += 4;
        }
        if (cb == qb) {
          score += 1;
        }
        previous_matched = true;
        ++q;
      } else {
        // A skipped byte costs a little, so the tighter of two matches wins.
        score -= 1;
        previous_matched = false;
      }
    }
    if (q < query.size()) {
      // The tail did not fit from here; a later word start only has less room.
      break;
    }
    // An exact prefix (the common case: the word typed IS the start of the
    // label) outranks any scattered match of the same query.
    if (start == 0 && c == query.size()) {
      score += 10;
    }
    best = std::max(best, score);
  }
  return best;
}

}  // namespace microide::editor
