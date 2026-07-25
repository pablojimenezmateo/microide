#pragma once

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif

#include <pcre2.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StringUtil.h"

namespace microide::util {

// Caps applied to every match so a user- or plugin-supplied pattern with
// catastrophic backtracking (e.g. `(a+)+$`) cannot spin near PCRE2's ~10M default
// ceiling on every line across every file and worker thread (a real hang/DoS).
// A pathological pattern blows past these exponentially, so it fails fast with
// PCRE2_ERROR_MATCHLIMIT/DEPTHLIMIT; legitimate patterns finish far below them.
// match_limit bounds total match steps (honored by the interpreter and JIT);
// depth_limit bounds backtracking/recursion depth (and thus native stack use).
inline constexpr std::uint32_t kRegexMatchLimit = 1'000'000;
inline constexpr std::uint32_t kRegexDepthLimit = 10'000;

struct RegexMatchRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

// Compile options for a user-facing SEARCH pattern (project-wide, in-file, and
// replace all share this), given whether the search is case sensitive.
//
// The literal search path folds Unicode case — `Δ` finds `δ`, `É` finds `é` (see
// util::Utf8CaseFold) — but the regex path only ever passed PCRE2_CASELESS,
// which without PCRE2_UTF folds ASCII and nothing else. So the SAME query
// matched case-insensitively as a literal and case-sensitively as a regex.
//
// Turning UTF/UCP on unconditionally would slow the dominant path: essentially
// every real query is ASCII, where PCRE2_CASELESS alone is already exactly
// right and byte-oriented matching is faster. So enable Unicode handling only
// when it can change the answer — a case-insensitive search whose query
// actually carries a non-ASCII byte.
//
// PCRE2_MATCH_INVALID_UTF (PCRE2 >= 10.34) comes along for the ride whenever UTF
// is on: search subjects are arbitrary file bytes, and without it a single
// invalid byte makes pcre2_match refuse the whole line, silently hiding matches
// elsewhere in it.
inline std::uint32_t SearchRegexCompileOptions(std::string_view query, bool case_sensitive) {
  if (case_sensitive) {
    return 0u;
  }
  const bool query_has_non_ascii =
      std::any_of(query.begin(), query.end(),
                  [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
  if (!query_has_non_ascii) {
    return PCRE2_CASELESS;
  }
#ifdef PCRE2_MATCH_INVALID_UTF
  return PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
#else
  return PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP;
#endif
}

inline std::string BuildRegexErrorMessage(std::string_view prefix,
                                          int error_code,
                                          PCRE2_SIZE error_offset) {
  PCRE2_UCHAR buffer[256]{};
  const int length = pcre2_get_error_message(error_code, buffer, sizeof(buffer));
  const std::string message =
      length > 0 ? std::string(reinterpret_cast<const char*>(buffer),
                               static_cast<std::size_t>(length))
                 : "invalid regular expression";
  return std::string(prefix) + " at offset " + std::to_string(error_offset) + ": " + message;
}

class RegexMatchData {
 public:
  RegexMatchData() = default;
  explicit RegexMatchData(pcre2_match_data* data) : data_(data) {}

  ~RegexMatchData() {
    if (data_ != nullptr) {
      pcre2_match_data_free(data_);
    }
  }

  RegexMatchData(const RegexMatchData&) = delete;
  RegexMatchData& operator=(const RegexMatchData&) = delete;

  RegexMatchData(RegexMatchData&& other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

  RegexMatchData& operator=(RegexMatchData&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (data_ != nullptr) {
      pcre2_match_data_free(data_);
    }
    data_ = std::exchange(other.data_, nullptr);
    return *this;
  }

  bool valid() const { return data_ != nullptr; }
  pcre2_match_data* get() const { return data_; }

 private:
  pcre2_match_data* data_ = nullptr;
};

class CompiledRegex {
 public:
  CompiledRegex() = default;

  CompiledRegex(std::string_view pattern, uint32_t options, std::string error_prefix = {})
      : utf_mode_((options & PCRE2_UTF) != 0), error_prefix_(std::move(error_prefix)) {
    if (pattern.empty()) {
      return;
    }

    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
                                     options, &error_code, &error_offset, nullptr);
    if (code != nullptr) {
      const int jit_rc = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
      if (jit_rc != 0) {
        static std::atomic<bool> jit_warned{false};
        if (!jit_warned.exchange(true)) {
          std::fprintf(stderr, "RegexUtil: PCRE2 JIT unavailable (rc=%d); using interpreted mode\n",
                       jit_rc);
        }
      }
      code_ = std::shared_ptr<pcre2_code>(code, pcre2_code_free);

      // Match context carrying the backtracking caps. It is not modified by
      // pcre2_match, so a single shared instance is safe to use concurrently from
      // the search worker threads. If creation fails we fall back to nullptr
      // (PCRE2's built-in defaults) rather than refusing to match.
      if (pcre2_match_context* mctx = pcre2_match_context_create(nullptr); mctx != nullptr) {
        pcre2_set_match_limit(mctx, kRegexMatchLimit);
        pcre2_set_depth_limit(mctx, kRegexDepthLimit);
        match_context_ = std::shared_ptr<pcre2_match_context>(mctx, pcre2_match_context_free);
      }
    } else if (!error_prefix_.empty()) {
      error_ = BuildRegexErrorMessage(error_prefix_, error_code, error_offset);
    }
  }

  bool valid() const { return code_ != nullptr; }
  const std::string& error() const { return error_; }
  // True when the pattern was compiled with PCRE2_UTF, i.e. PCRE2 interprets the
  // subject as UTF-8 and a start offset must land on a character boundary.
  // Without UTF, matching is byte-oriented and every byte is a legal offset.
  bool utf_mode() const { return utf_mode_; }

  RegexMatchData CreateMatchData() const {
    return RegexMatchData(valid() ? pcre2_match_data_create_from_pattern(code_.get(), nullptr)
                                  : nullptr);
  }

  int Match(std::string_view text,
            std::size_t offset,
            RegexMatchData& match_data,
            uint32_t options = 0) const {
    if (!valid() || !match_data.valid()) {
      return PCRE2_ERROR_BADOPTION;
    }
    return pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), offset,
                       options, match_data.get(), match_context_.get());
  }

  bool CaptureRange(const RegexMatchData& match_data,
                    std::size_t text_size,
                    RegexMatchRange* range) const {
    if (!match_data.valid() || range == nullptr) {
      return false;
    }
    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
    const std::size_t start = static_cast<std::size_t>(ovector[0]);
    const std::size_t end = static_cast<std::size_t>(ovector[1]);
    if (start > text_size || end > text_size) {
      return false;
    }
    range->start = start;
    range->end = end;
    return true;
  }

  // Substitutes ALL matches in `subject` (global) and APPENDS the result to
  // `out`. The replacement is interpreted with PCRE2's extended syntax: `$1` /
  // `${name}` group references plus `\n \t \r`, `\U \L \u \l \E` case modifiers,
  // and `${name:-default}` conditionals (VSCode-parity). Returns the number of
  // substitutions (>= 0) or a negative PCRE2 error code (e.g.
  // PCRE2_ERROR_BADREPESCAPE for an unsupported `\` escape); `out` is left
  // unchanged on error. Honors the shared match/depth limits.
  int SubstituteInto(std::string_view subject,
                     std::string_view replacement,
                     std::string& out) const {
    if (!valid()) {
      return PCRE2_ERROR_BADOPTION;
    }
    // Reused across calls so a per-line replace-all pass does not allocate a
    // fresh output buffer for every line. PCRE2_SUBSTITUTE_OVERFLOW_LENGTH lets
    // us size exactly on the first overflow and retry once.
    thread_local std::vector<PCRE2_UCHAR> buffer(256);
    const uint32_t options = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED |
                             PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    for (;;) {
      PCRE2_SIZE out_length = buffer.size();
      const int rc = pcre2_substitute(
          code_.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
          /*startoffset=*/0, options, /*match_data=*/nullptr, match_context_.get(),
          reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), buffer.data(),
          &out_length);
      if (rc == PCRE2_ERROR_NOMEMORY) {
        // out_length holds the required size (including the NUL) on overflow.
        buffer.resize(out_length);
        continue;
      }
      if (rc < 0) {
        return rc;
      }
      out.append(reinterpret_cast<const char*>(buffer.data()), out_length);
      return rc;
    }
  }

  // Expands `replacement` for the single match that begins at `offset` in
  // `subject` (matched in full context, so lookarounds resolve), returning ONLY
  // the expanded replacement text — used by "Replace current" to substitute one
  // occurrence. Same replacement syntax as SubstituteInto. Returns std::nullopt
  // on any PCRE2 error (bad escape, no anchored match at `offset`, limit hit).
  std::optional<std::string> ExpandMatchAt(std::string_view subject,
                                           std::size_t offset,
                                           std::string_view replacement) const {
    if (!valid() || offset > subject.size()) {
      return std::nullopt;
    }
    std::vector<PCRE2_UCHAR> buffer(std::max<std::size_t>(64, replacement.size() + 16));
    const uint32_t options = PCRE2_SUBSTITUTE_EXTENDED | PCRE2_SUBSTITUTE_REPLACEMENT_ONLY |
                             PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_ANCHORED;
    for (;;) {
      PCRE2_SIZE out_length = buffer.size();
      const int rc = pcre2_substitute(
          code_.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), offset, options,
          /*match_data=*/nullptr, match_context_.get(),
          reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), buffer.data(),
          &out_length);
      if (rc == PCRE2_ERROR_NOMEMORY) {
        buffer.resize(out_length);
        continue;
      }
      if (rc <= 0) {
        return std::nullopt;  // rc == 0: nothing matched anchored at `offset`.
      }
      return std::string(reinterpret_cast<const char*>(buffer.data()), out_length);
    }
  }

 private:
  bool utf_mode_ = false;
  std::shared_ptr<pcre2_code> code_;
  std::shared_ptr<pcre2_match_context> match_context_;
  std::string error_prefix_;
  std::string error_;
};

// Default cancellation poll interval (advance-loop iterations) for
// FindNextRegexMatchInLine — coarse enough that the modulo/atomic-load cost is
// negligible against the per-offset PCRE2 match, fine enough that a cooperative
// Stop() stays sub-millisecond even on a pathological single-line file.
inline constexpr std::size_t kRegexMatchCancelPollInterval = 4096;

// Finds the next regex match in `line` starting at *search_from, advancing past
// empty matches (recovering a non-empty alternative anchored at the same offset,
// e.g. `x?|foo` on "foo"). Returns true and fills *match_start / *match_end /
// *search_from on a real match; false when the line is exhausted.
//
// `cancel` (optional) is polled every kRegexMatchCancelPollInterval iterations of
// the empty-match advance loop so a pattern that matches empty at every offset
// (e.g. `x?`) on a very large single line can be interrupted; on cancel it returns
// false leaving *search_from short of line.size()+1. Callers with no cancellation
// need (in-memory buffer search) pass nullptr. This is the single shared match
// engine for both project-wide and in-file regex search.
inline bool FindNextRegexMatchInLine(const CompiledRegex& pattern,
                                     std::string_view line,
                                     std::size_t* search_from,
                                     RegexMatchData* match_data,
                                     std::size_t* match_start,
                                     std::size_t* match_end,
                                     const std::atomic<bool>* cancel = nullptr) {
  if (!pattern.valid() || search_from == nullptr || match_data == nullptr ||
      !match_data->valid() || match_start == nullptr || match_end == nullptr) {
    return false;
  }

  std::size_t iterations = 0;
  while (*search_from <= line.size()) {
    if (cancel != nullptr && (++iterations % kRegexMatchCancelPollInterval) == 0 &&
        cancel->load(std::memory_order_relaxed)) {
      return false;
    }
    const int rc = pattern.Match(line, *search_from, *match_data);
    if (rc == PCRE2_ERROR_NOMATCH) {
      return false;
    }
    if (rc < 0) {
      return false;
    }

    RegexMatchRange range;
    if (!pattern.CaptureRange(*match_data, line.size(), &range)) {
      return false;
    }
    if (range.start == range.end) {
      // PCRE2 returned the leftmost match and it is empty. There is no match of
      // any kind before range.start, but a non-empty alternative may begin at the
      // SAME offset (e.g. `x?|foo` on "foo": the leftmost `x?` matches empty at 0,
      // yet `foo` also starts at 0). The naive "advance one byte" idiom loses it.
      // Retry anchored at range.start rejecting an empty match to recover it.
      const int anchored_rc =
          pattern.Match(line, range.start, *match_data, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED);
      RegexMatchRange anchored;
      if (anchored_rc >= 0 && pattern.CaptureRange(*match_data, line.size(), &anchored) &&
          anchored.start != anchored.end) {
        *match_start = anchored.start;
        *match_end = anchored.end;
        *search_from = anchored.end;
        return true;
      }
      // No non-empty match here; skip the pure empty match and advance.
      //
      // The step must be a whole CHARACTER under PCRE2_UTF, not one byte. A byte
      // step lands inside a multi-byte sequence, and pcre2_match then rejects the
      // start offset with PCRE2_ERROR_BADUTFOFFSET — which the `rc < 0` arm above
      // treats as "line exhausted", silently dropping every remaining match on
      // the line. PCRE2_MATCH_INVALID_UTF (>= 10.34) happens to tolerate such an
      // offset and masks this, but the compile-options fallback above deliberately
      // supports older PCRE2 without it, where a case-insensitive non-ASCII query
      // that can match empty returned ZERO matches from the first multi-byte
      // character onward. Stepping by character is also strictly cheaper: it skips
      // the 1-3 continuation-byte offsets that can never begin a match anyway.
      if (range.end >= line.size()) {
        *search_from = line.size() + 1;
      } else if (pattern.utf_mode()) {
        // NextUtf8Boundary advances ONE character from a boundary offset, which
        // range.end is (PCRE2 in UTF mode only reports character boundaries).
        // Passing range.end + 1 would start the scan mid-sequence, where the
        // lead-byte classifier sees a continuation byte and falls back to a
        // 1-byte step — landing mid-character again.
        *search_from = NextUtf8Boundary(line, range.end);
      } else {
        *search_from = range.end + 1;  // byte-oriented: every offset is legal
      }
      continue;
    }

    *match_start = range.start;
    *match_end = range.end;
    *search_from = range.end;
    return true;
  }

  return false;
}

}  // namespace microide::util
