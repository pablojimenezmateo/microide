#pragma once

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif

#include <pcre2.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace microide::util {

struct RegexMatchRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

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
      : error_prefix_(std::move(error_prefix)) {
    if (pattern.empty()) {
      return;
    }

    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
                                     options, &error_code, &error_offset, nullptr);
    if (code != nullptr) {
      code_ = std::shared_ptr<pcre2_code>(code, pcre2_code_free);
    } else if (!error_prefix_.empty()) {
      error_ = BuildRegexErrorMessage(error_prefix_, error_code, error_offset);
    }
  }

  bool valid() const { return code_ != nullptr; }
  const std::string& error() const { return error_; }

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
                       options, match_data.get(), nullptr);
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

 private:
  std::shared_ptr<pcre2_code> code_;
  std::string error_prefix_;
  std::string error_;
};

}  // namespace microide::util
