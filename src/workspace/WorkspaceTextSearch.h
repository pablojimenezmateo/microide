#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextBuffer.h"
#include "editor/TextViewport.h"
#include "util/RegexUtil.h"

namespace microide::workspace {

bool StartsWith(std::string_view text, std::string_view prefix);
std::string ToLower(std::string_view text);

std::string CollapseWhitespace(std::string_view text);

bool QuerySupportsLiteralReplace(std::string_view query);
bool UsesCaseSensitiveLiteralMatch(std::string_view query);
std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive);

std::optional<std::size_t> ReplaceRegexMatchesInText(std::string& content,
                                                     const util::CompiledRegex& pattern,
                                                     std::string_view replacement);

struct BufferSearchOptions {
  bool case_sensitive = false;
  bool whole_word = false;
  std::optional<editor::SelectionRange> scope;

  friend bool operator==(const BufferSearchOptions&, const BufferSearchOptions&) = default;
};

std::vector<editor::SelectionRange> FindRegexSearchMatches(const editor::TextBuffer& buffer,
                                                           const util::CompiledRegex& pattern,
                                                           BufferSearchOptions options = {},
                                                           bool* truncated = nullptr);

std::vector<editor::SelectionRange> SplitRegexMatchHighlightFragments(
    const editor::TextBuffer& buffer, const std::vector<editor::SelectionRange>& matches);
inline constexpr std::size_t kMaxBufferSearchMatches = 100000;

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

std::vector<editor::SelectionRange> RefineLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    const std::vector<editor::SelectionRange>& previous,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

bool QueryExtendsCaseInsensitive(std::string_view prefix, std::string_view query);

std::optional<std::size_t> FindLiteralNeedleInLine(std::string_view haystack,
                                                   std::size_t start_from,
                                                   std::string_view needle,
                                                   bool case_sensitive);

inline constexpr std::size_t kMaxAddCursorAtAllMatches = 10000;

struct AddCursorMatchScan {
  std::vector<editor::SelectionRange> ranges;
  bool truncated = false;
};

AddCursorMatchScan CollectAddCursorMatchRanges(const editor::TextBuffer& buffer,
                                               std::size_t seed_line,
                                               std::size_t seed_column,
                                               std::string_view needle,
                                               bool case_sensitive,
                                               std::size_t max_matches =
                                                   kMaxAddCursorAtAllMatches);

std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const std::vector<std::string>& lines,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive);

std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const editor::TextBuffer& buffer,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive);

}
