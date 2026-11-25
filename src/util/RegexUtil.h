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

inline constexpr std::uint32_t kRegexMatchLimit = 1'000'000;
inline constexpr std::uint32_t kRegexDepthLimit = 10'000;

#ifdef PCRE2_MATCH_INVALID_UTF
inline constexpr std::uint32_t kMatchInvalidUtf = PCRE2_MATCH_INVALID_UTF;
#else
inline constexpr std::uint32_t kMatchInvalidUtf = 0;
#endif

inline constexpr std::uint32_t kJitMatchOptions = PCRE2_NOTBOL | PCRE2_NOTEOL | PCRE2_NOTEMPTY |
                                                  PCRE2_NOTEMPTY_ATSTART | PCRE2_PARTIAL_SOFT |
                                                  PCRE2_PARTIAL_HARD;

struct RegexMatchRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

inline std::uint32_t SearchRegexCompileOptions(std::string_view query, bool case_sensitive) {
  const std::uint32_t caseless = case_sensitive ? 0u : static_cast<std::uint32_t>(PCRE2_CASELESS);
#ifdef PCRE2_MATCH_INVALID_UTF
  (void)query;
  return caseless | PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
#else
  const bool query_has_non_ascii =
      std::any_of(query.begin(), query.end(),
                  [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
  return case_sensitive || !query_has_non_ascii ? caseless
                                                : caseless | PCRE2_UTF | PCRE2_UCP;
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

enum class RegexSubject : std::uint8_t { Unknown, Ascii };

enum class AsciiTwin : std::uint8_t { None, Build };

class CompiledRegex {
 public:
  CompiledRegex() = default;

  CompiledRegex(std::string_view pattern,
                uint32_t options,
                std::string error_prefix = {},
                AsciiTwin twin = AsciiTwin::None)
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
      jit_fast_path_ = jit_rc == 0 && (!utf_mode_ || (options & kMatchInvalidUtf) != 0);
      code_ = std::shared_ptr<pcre2_code>(code, pcre2_code_free);

      if (pcre2_match_context* mctx = pcre2_match_context_create(nullptr); mctx != nullptr) {
        pcre2_set_match_limit(mctx, kRegexMatchLimit);
        pcre2_set_depth_limit(mctx, kRegexDepthLimit);
        match_context_ = std::shared_ptr<pcre2_match_context>(mctx, pcre2_match_context_free);
      }
    } else if (!error_prefix_.empty()) {
      error_ = BuildRegexErrorMessage(error_prefix_, error_code, error_offset);
    }
    if (code_ != nullptr && utf_mode_ && twin == AsciiTwin::Build && IsAllAscii(pattern)) {
      const uint32_t byte_options = options & ~(PCRE2_UTF | PCRE2_UCP | kMatchInvalidUtf);
      pcre2_code* ascii = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                        pattern.size(), byte_options, &error_code, &error_offset,
                                        nullptr);
      if (ascii != nullptr) {
        ascii_jit_ = pcre2_jit_compile(ascii, PCRE2_JIT_COMPLETE) == 0;
        ascii_code_ = std::shared_ptr<pcre2_code>(ascii, pcre2_code_free);
      }
    }
  }

  bool valid() const { return code_ != nullptr; }
  const std::string& error() const { return error_; }
  bool utf_mode() const { return utf_mode_; }

  RegexMatchData CreateMatchData() const {
    return RegexMatchData(valid() ? pcre2_match_data_create_from_pattern(code_.get(), nullptr)
                                  : nullptr);
  }

  bool has_ascii_twin() const { return ascii_code_ != nullptr; }

  int Match(std::string_view text,
            std::size_t offset,
            RegexMatchData& match_data,
            uint32_t options = 0,
            RegexSubject subject = RegexSubject::Unknown) const {
    if (!valid() || !match_data.valid()) {
      return PCRE2_ERROR_BADOPTION;
    }
    if (subject == RegexSubject::Ascii && ascii_code_ != nullptr) {
      if (ascii_jit_ && (options & ~kJitMatchOptions) == 0) {
        return pcre2_jit_match(ascii_code_.get(), reinterpret_cast<PCRE2_SPTR>(text.data()),
                               text.size(), offset, options, match_data.get(),
                               match_context_.get());
      }
      return pcre2_match(ascii_code_.get(), reinterpret_cast<PCRE2_SPTR>(text.data()),
                         text.size(), offset, options, match_data.get(), match_context_.get());
    }
    if (jit_fast_path_ && (options & ~kJitMatchOptions) == 0) {
      return pcre2_jit_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(),
                             offset, options, match_data.get(), match_context_.get());
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

  int SubstituteInto(std::string_view subject,
                     std::string_view replacement,
                     std::string& out) const {
    if (!valid()) {
      return PCRE2_ERROR_BADOPTION;
    }
    thread_local std::vector<PCRE2_UCHAR> buffer(256);
    const uint32_t options = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED |
                             PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    pcre2_code* code =
        ascii_code_ != nullptr && IsAllAscii(subject) ? ascii_code_.get() : code_.get();
    for (;;) {
      PCRE2_SIZE out_length = buffer.size();
      const int rc = pcre2_substitute(
          code, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
          0, options, nullptr, match_context_.get(),
          reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), buffer.data(),
          &out_length);
      if (rc == PCRE2_ERROR_NOMEMORY) {
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
          nullptr, match_context_.get(),
          reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), buffer.data(),
          &out_length);
      if (rc == PCRE2_ERROR_NOMEMORY) {
        buffer.resize(out_length);
        continue;
      }
      if (rc <= 0) {
        return std::nullopt;
      }
      return std::string(reinterpret_cast<const char*>(buffer.data()), out_length);
    }
  }

 private:
  bool utf_mode_ = false;
  bool jit_fast_path_ = false;
  bool ascii_jit_ = false;
  std::shared_ptr<pcre2_code> code_;
  std::shared_ptr<pcre2_code> ascii_code_;
  std::shared_ptr<pcre2_match_context> match_context_;
  std::string error_prefix_;
  std::string error_;
};

inline constexpr std::size_t kRegexMatchCancelPollInterval = 4096;

inline bool FindNextRegexMatchInLine(const CompiledRegex& pattern,
                                     std::string_view line,
                                     std::size_t* search_from,
                                     RegexMatchData* match_data,
                                     std::size_t* match_start,
                                     std::size_t* match_end,
                                     const std::atomic<bool>* cancel = nullptr,
                                     RegexSubject subject = RegexSubject::Unknown) {
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
    const int rc = pattern.Match(line, *search_from, *match_data, 0, subject);
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
      const int anchored_rc =
          pattern.Match(line, range.start, *match_data, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED,
                        subject);
      RegexMatchRange anchored;
      if (anchored_rc >= 0 && pattern.CaptureRange(*match_data, line.size(), &anchored) &&
          anchored.start != anchored.end) {
        *match_start = anchored.start;
        *match_end = anchored.end;
        *search_from = anchored.end;
        return true;
      }
      if (range.end >= line.size()) {
        *search_from = line.size() + 1;
      } else if (pattern.utf_mode()) {
        *search_from = NextUtf8Boundary(line, range.end);
      } else {
        *search_from = range.end + 1;
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

}
