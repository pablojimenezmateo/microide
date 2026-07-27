#include "project/GlobMatch.h"

#include <cstddef>
#include <utility>

namespace microide::project {

bool GlobMatches(std::string_view pattern, std::string_view text) {
  const std::size_t plen = pattern.size();
  const std::size_t tlen = text.size();
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star_pi = std::string_view::npos;
  std::size_t star_ti = 0;
  bool star_cross_slash = false;  // the pending '*' backtrack may consume '/'
  // A cross-slash '**' restart kept separately from `star_pi` so that a later
  // non-crossing '*' in the same pattern (e.g. the '*' in '**/*.txt') does not
  // clobber the only way to consume the intervening '/' separators. Without this
  // fallback, '**/' degrades to matching at the top level only.
  std::size_t dstar_pi = std::string_view::npos;
  std::size_t dstar_ti = 0;

  while (ti < tlen) {
    if (pi < plen) {
      const char pc = pattern[pi];
      if (pc == '*') {
        // Consume the whole run of '*'. A run of two or more that forms a complete
        // path segment ('**') matches across '/'; anything else behaves as '*'.
        const std::size_t star_start = pi;
        std::size_t star_count = 0;
        while (pi < plen && pattern[pi] == '*') {
          ++pi;
          ++star_count;
        }
        const bool before_ok = star_start == 0 || pattern[star_start - 1] == '/';
        const bool after_ok = pi == plen || pattern[pi] == '/';
        const bool segment_doublestar = star_count >= 2 && before_ok && after_ok;
        if (segment_doublestar && pi < plen && pattern[pi] == '/') {
          // '**/' can match zero directories: fold the following '/' into the
          // wildcard so the pattern can resume with nothing consumed from text.
          ++pi;
        }
        star_pi = pi;
        star_ti = ti;
        star_cross_slash = segment_doublestar;
        if (segment_doublestar) {
          // Remember this cross-slash '**' restart independently so a later
          // non-crossing '*' cannot clobber our only way back across a '/'.
          dstar_pi = pi;
          dstar_ti = ti;
        }
        continue;
      }
      if (pc == '?') {
        if (text[ti] == '/') {
          // fall through to backtrack
        } else {
          ++pi;
          ++ti;
          continue;
        }
      } else if (pc == '[') {
        const char tc = text[ti];
        if (tc == '/') {
          // fall through to backtrack
        } else {
          std::size_t scan = pi + 1;
          bool negate = false;
          if (scan < plen && (pattern[scan] == '!' || pattern[scan] == '^')) {
            negate = true;
            ++scan;
          }
          bool matched = false;
          bool closed = false;
          while (scan < plen) {
            if (pattern[scan] == ']' && scan != pi + 1 + (negate ? 1 : 0)) {
              closed = true;
              ++scan;
              break;
            }
            char lo = pattern[scan++];
            if (lo == '\\' && scan < plen) {
              lo = pattern[scan++];
            }
            char hi = lo;
            if (scan + 1 < plen && pattern[scan] == '-' && pattern[scan + 1] != ']') {
              ++scan;
              hi = pattern[scan++];
              if (hi == '\\' && scan < plen) {
                hi = pattern[scan++];
              }
            }
            if (tc >= lo && tc <= hi) {
              matched = true;
            }
          }
          if (closed) {
            if (matched != negate) {
              pi = scan;
              ++ti;
              continue;
            }
          } else if (tc == '[') {
            // POSIX fnmatch / gitignore(5): a '[' with no closing ']' is not a
            // character class but an ordinary literal '['. Match it as such so a
            // pattern like "weird[name" ignores the file literally named
            // "weird[name" instead of failing to match. (A closed-but-unmatched
            // class still falls through to backtrack above.)
            ++pi;
            ++ti;
            continue;
          }
        }
      } else {
        char lit = pc;
        std::size_t advance = 1;
        if (lit == '\\' && pi + 1 < plen) {
          lit = pattern[pi + 1];
          advance = 2;
        }
        if (text[ti] == lit) {
          pi += advance;
          ++ti;
          continue;
        }
      }
    }
    if (star_pi != std::string_view::npos && (star_cross_slash || text[star_ti] != '/')) {
      pi = star_pi;
      ti = ++star_ti;
      continue;
    }
    // The most-recent '*' is exhausted or blocked by a '/'. Fall back to the
    // most-recent cross-slash '**', which is allowed to consume the '/' — this is
    // what lets '**/<wildcard>' patterns descend through directory boundaries. The
    // stale '*' backtrack (which sits AFTER '**' in the pattern) is invalidated so a
    // later literal mismatch cannot wrongly jump forward into it while we are
    // re-extending the '**'; it is freshly re-armed when the '*' is reached again.
    if (dstar_pi != std::string_view::npos && dstar_ti < tlen) {
      pi = dstar_pi;
      ti = ++dstar_ti;
      star_pi = std::string_view::npos;
      continue;
    }
    return false;
  }
  while (pi < plen && pattern[pi] == '*') {
    ++pi;
  }
  return pi == plen;
}

namespace {

// Upper bound on how many patterns one scope box may expand to. Brace alternation
// is multiplicative ("{a,b}/{c,d}/{e,f}" is 8), so a pasted pathological entry
// could otherwise expand without bound. 256 is far above any hand-written filter;
// past it the remaining alternatives are dropped rather than allocated.
constexpr std::size_t kMaxExpandedPatterns = 256;

// Longest single pattern we keep. Scope globs are short by nature; anything longer
// is paste noise that would only slow the per-file match loop.
constexpr std::size_t kMaxPatternBytes = 1024;

std::string_view TrimAscii(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

bool HasWildcard(std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '\\') {
      ++index;  // the escaped character is literal, wildcard or not
      continue;
    }
    if (c == '*' || c == '?' || c == '[') {
      return true;
    }
  }
  return false;
}

// Expand `{a,b}` alternation into concrete patterns. Braces are matched with
// nesting awareness and '\\' escapes are respected, so "a\\{b" is a literal brace.
// Appends to `out`, stopping once kMaxExpandedPatterns is reached.
void ExpandBraces(std::string pattern, std::vector<std::string>& out) {
  if (out.size() >= kMaxExpandedPatterns) {
    return;
  }

  std::size_t open = std::string::npos;
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (pattern[index] == '\\') {
      ++index;
      continue;
    }
    if (pattern[index] == '{') {
      open = index;
      break;
    }
  }
  if (open == std::string::npos) {
    out.push_back(std::move(pattern));
    return;
  }

  // Locate the matching '}' and the top-level ',' split points inside it.
  std::size_t depth = 0;
  std::size_t close = std::string::npos;
  std::vector<std::size_t> commas;
  for (std::size_t index = open; index < pattern.size(); ++index) {
    const char c = pattern[index];
    if (c == '\\') {
      ++index;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        close = index;
        break;
      }
    } else if (c == ',' && depth == 1) {
      commas.push_back(index);
    }
  }
  if (close == std::string::npos) {
    // Unbalanced '{' — treat the rest as literal text rather than dropping the
    // pattern (mirrors how an unterminated '[' stays a literal bracket).
    out.push_back(std::move(pattern));
    return;
  }

  const std::string prefix = pattern.substr(0, open);
  const std::string suffix = pattern.substr(close + 1);
  std::size_t alt_start = open + 1;
  commas.push_back(close);
  for (const std::size_t alt_end : commas) {
    if (out.size() >= kMaxExpandedPatterns) {
      return;
    }
    std::string expanded = prefix;
    expanded.append(pattern, alt_start, alt_end - alt_start);
    expanded.append(suffix);
    alt_start = alt_end + 1;
    ExpandBraces(std::move(expanded), out);
  }
}

// Turn one user-typed entry into the zero, one, or two patterns it means. A
// wildcard-free entry names a file OR a subtree, so it yields both "p" and "p/**".
void AppendNormalized(std::string_view entry, std::vector<std::string>& out) {
  entry = TrimAscii(entry);
  while (entry.starts_with("./")) {
    entry.remove_prefix(2);
  }
  while (entry.starts_with('/')) {
    entry.remove_prefix(1);
  }
  while (entry.ends_with('/')) {
    entry.remove_suffix(1);
  }
  if (entry.empty() || entry.size() > kMaxPatternBytes) {
    return;
  }

  // Decide "is this a plain path?" from what the user actually typed — the "**/"
  // prefix added below is ours, and testing it would make every bare name look
  // wildcarded and lose its subtree expansion.
  const bool subtree = !HasWildcard(entry);

  std::string base(entry);
  // A '/'-free entry floats to any depth, exactly as VSCode's search box treats
  // "*.ts" as "**/*.ts". An entry that already spells its own "**/" prefix is left
  // alone so it is not doubled.
  if (base.find('/') == std::string::npos && !base.starts_with("**")) {
    base.insert(0, "**/");
  }
  if (out.size() < kMaxExpandedPatterns) {
    ExpandBraces(base, out);
  }
  if (subtree && out.size() < kMaxExpandedPatterns) {
    // "src/util" also means everything under src/util, matching how selecting a
    // folder in VSCode's explorer fills the include box with that folder path.
    ExpandBraces(base + "/**", out);
  }
}

}  // namespace

GlobSet GlobSet::Parse(std::string_view comma_separated) {
  GlobSet set;
  // Split on top-level ',' only: a comma inside '{...}' is alternation, not an
  // entry separator, so "*.{ts,js}" stays one entry (VSCode parses it the same
  // way). Escapes are honored so "a\\,b" is a literal comma.
  std::size_t start = 0;
  std::size_t depth = 0;
  for (std::size_t index = 0; index <= comma_separated.size(); ++index) {
    if (index == comma_separated.size()) {
      AppendNormalized(comma_separated.substr(start), set.patterns_);
      break;
    }
    const char c = comma_separated[index];
    if (c == '\\') {
      ++index;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth > 0) {
        --depth;
      }
    } else if (c == ',' && depth == 0) {
      AppendNormalized(comma_separated.substr(start, index - start), set.patterns_);
      start = index + 1;
    }
  }
  return set;
}

bool GlobSet::Matches(std::string_view normalized_relative_path) const {
  for (const std::string& pattern : patterns_) {
    if (GlobMatches(pattern, normalized_relative_path)) {
      return true;
    }
  }
  return false;
}

}  // namespace microide::project
