#include "project/GlobMatch.h"

#include "util/StringUtil.h"

#include <cstddef>
#include <utility>

namespace microide::project {

namespace {

bool ExtendSegmentDoubleStar(std::string_view text, std::size_t& consumed_end,
                             bool folded_slash) {
  if (!folded_slash) {
    if (consumed_end >= text.size()) {
      return false;
    }
    ++consumed_end;
    return true;
  }
  const std::size_t separator = text.find('/', consumed_end);
  if (separator == std::string_view::npos) {
    return false;
  }
  consumed_end = separator + 1;
  return true;
}

}

bool GlobMatches(std::string_view pattern, std::string_view text,
                 GlobDoubleStar double_star) {
  const bool double_star_always_crosses = double_star == GlobDoubleStar::Always;
  const std::size_t plen = pattern.size();
  const std::size_t tlen = text.size();
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star_pi = std::string_view::npos;
  std::size_t star_ti = 0;
  bool star_cross_slash = false;
  std::size_t dstar_pi = std::string_view::npos;
  std::size_t dstar_ti = 0;
  bool star_folded_slash = false;
  bool dstar_folded_slash = false;

  while (ti < tlen) {
    if (pi < plen) {
      const char pc = pattern[pi];
      if (pc == '*') {
        const std::size_t star_start = pi;
        std::size_t star_count = 0;
        while (pi < plen && pattern[pi] == '*') {
          ++pi;
          ++star_count;
        }
        const bool before_ok = star_start == 0 || pattern[star_start - 1] == '/';
        const bool after_ok = pi == plen || pattern[pi] == '/';
        const bool segment_doublestar =
            star_count >= 2 && (double_star_always_crosses || (before_ok && after_ok));
        if (segment_doublestar && pi == plen) {
          return true;
        }
        const bool folded_slash = segment_doublestar && pi < plen && pattern[pi] == '/';
        if (folded_slash) {
          ++pi;
        }
        star_pi = pi;
        star_ti = ti;
        star_cross_slash = segment_doublestar;
        star_folded_slash = folded_slash;
        if (segment_doublestar) {
          dstar_pi = pi;
          dstar_ti = ti;
          dstar_folded_slash = folded_slash;
        }
        continue;
      }
      if (pc == '?') {
        if (text[ti] == '/') {
        } else {
          ++pi;
          ++ti;
          continue;
        }
      } else if (pc == '[') {
        const char tc = text[ti];
        if (tc == '/') {
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
    if (star_pi != std::string_view::npos) {
      if (star_cross_slash) {
        if (ExtendSegmentDoubleStar(text, star_ti, star_folded_slash)) {
          pi = star_pi;
          ti = star_ti;
          continue;
        }
      } else if (text[star_ti] != '/') {
        pi = star_pi;
        ti = ++star_ti;
        continue;
      }
    }
    if (dstar_pi != std::string_view::npos &&
        ExtendSegmentDoubleStar(text, dstar_ti, dstar_folded_slash)) {
      pi = dstar_pi;
      ti = dstar_ti;
      star_pi = std::string_view::npos;
      continue;
    }
    return false;
  }
  while (pi < plen && pattern[pi] == '*') {
    const std::size_t star_start = pi;
    while (pi < plen && pattern[pi] == '*') {
      ++pi;
    }
    const bool before_ok = star_start == 0 || pattern[star_start - 1] == '/';
    if (pi - star_start >= 2 && pi < plen && pattern[pi] == '/' &&
        (double_star_always_crosses || before_ok)) {
      ++pi;
    }
  }
  return pi == plen;
}

namespace {

constexpr std::size_t kMaxPatternBytes = 1024;

bool HasWildcard(std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '\\') {
      ++index;
      continue;
    }
    if (c == '*' || c == '?' || c == '[') {
      return true;
    }
  }
  return false;
}

}

void ExpandGlobBraces(std::string pattern, std::vector<std::string>& out) {
  if (out.size() >= kMaxExpandedGlobPatterns || pattern.size() > kMaxBraceExpansionPatternBytes) {
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
    out.push_back(std::move(pattern));
    return;
  }

  const std::string prefix = pattern.substr(0, open);
  const std::string suffix = pattern.substr(close + 1);
  std::size_t alt_start = open + 1;
  commas.push_back(close);
  for (const std::size_t alt_end : commas) {
    if (out.size() >= kMaxExpandedGlobPatterns) {
      return;
    }
    std::string expanded = prefix;
    expanded.append(pattern, alt_start, alt_end - alt_start);
    expanded.append(suffix);
    alt_start = alt_end + 1;
    ExpandGlobBraces(std::move(expanded), out);
  }
}

namespace {

void AppendNormalized(std::string_view entry, std::vector<std::string>& out) {
  entry = util::TrimAsciiWhitespaceView(entry);
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

  const bool subtree = !HasWildcard(entry);

  std::string base(entry);
  if (base.find('/') == std::string::npos && !base.starts_with("**")) {
    base.insert(0, "**/");
  }
  if (out.size() < kMaxExpandedGlobPatterns) {
    ExpandGlobBraces(base, out);
  }
  if (subtree && out.size() < kMaxExpandedGlobPatterns) {
    ExpandGlobBraces(base + "/**", out);
  }
}

}

GlobSet GlobSet::Parse(std::string_view comma_separated) {
  GlobSet set;
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

}
