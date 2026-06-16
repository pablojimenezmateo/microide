#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::tests::architecture {

std::filesystem::path RepoRoot();
std::string ReadText(const std::filesystem::path& path);
std::size_t LineNumberAt(std::string_view text, std::size_t offset);
std::vector<bool> BuildCodeMask(std::string_view text);
// Counts source lines of code: lines holding at least one real code byte.
// Blank lines and comment-only lines are excluded (per BuildCodeMask); a line
// with code plus a trailing comment still counts. This is the single counter
// behind every architecture file-size cap.
std::size_t CountCodeLines(std::string_view text);
std::size_t CountCodeLinesInFile(const std::filesystem::path& path);
bool MatchesCodeAt(std::string_view text,
                   const std::vector<bool>& is_code,
                   std::size_t pos,
                   std::string_view needle);
std::vector<std::size_t> FindCodeLiteralOccurrences(std::string_view text,
                                                    std::string_view literal);
std::vector<std::size_t> FindTryCatchStoViolations(std::string_view text);
std::vector<bool> BuildTestingGuardMask(const std::string& text);
std::optional<std::string> ExtractBraceDelimitedBody(const std::string& text,
                                                     std::size_t open_brace_index);
std::optional<std::pair<std::string, std::size_t>> ExtractMemberFunctionBodyWithOffset(
    const std::string& text, std::string_view signature_needle);

}  // namespace microide::tests::architecture
