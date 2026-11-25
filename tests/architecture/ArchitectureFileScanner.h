#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
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
std::size_t CountCodeLines(std::string_view text);
std::size_t CountCodeLinesInFile(const std::filesystem::path& path);
bool MatchesCodeAt(std::string_view text,
                   const std::vector<bool>& is_code,
                   std::size_t pos,
                   std::string_view needle);
std::vector<std::size_t> FindCodeLiteralOccurrences(std::string_view text,
                                                    std::string_view literal);
std::vector<std::size_t> FindTryCatchStoViolations(std::string_view text);
inline constexpr std::array<std::string_view, 1> kDefaultSourceExtensions = {".cpp"};
std::vector<std::filesystem::path> SourceFilesUnder(
    const std::filesystem::path& root,
    std::span<const std::string_view> extensions = kDefaultSourceExtensions);
std::vector<bool> BuildTestingGuardMask(const std::string& text);
std::optional<std::string> ExtractBraceDelimitedBody(const std::string& text,
                                                     std::size_t open_brace_index);
std::optional<std::pair<std::string, std::size_t>> ExtractMemberFunctionBodyWithOffset(
    const std::string& text, std::string_view signature_needle);

}
