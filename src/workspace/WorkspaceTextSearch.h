#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::workspace {

bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWith(std::string_view text, std::string_view suffix);
std::string ToLower(std::string_view text);

std::vector<std::string> SplitSyntaxLines(std::string_view text);
std::string CollapseWhitespace(std::string_view text);

bool QuerySupportsLiteralReplace(std::string_view query);
bool UsesCaseSensitiveLiteralMatch(std::string_view query);
std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive);
std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query);

std::optional<std::size_t> FindLiteralNeedleInLine(std::string_view haystack,
                                                   std::size_t start_from,
                                                   std::string_view needle,
                                                   bool case_sensitive);

}  // namespace microide::workspace
