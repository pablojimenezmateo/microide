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
// Every `*.cpp` under `root`, EXCLUDING any `fixtures/` subtree, collected
// without throwing.
//
// Two rules walk `tests/perf` recursively, which contains the generated fixture
// trees — tens of thousands of files that no rule can match, and that the ctest
// fixture-setup tests rewrite while the shards run. A `recursive_directory_iterator`
// over them is both wasted work and a race: increment on an entry another process
// just unlinked throws, and the whole architecture shard died with
// "cannot increment recursive directory iterator: No such file or directory"
// (TD-2026-08-10-170).
std::vector<std::filesystem::path> SourceFilesUnder(const std::filesystem::path& root);
std::vector<bool> BuildTestingGuardMask(const std::string& text);
std::optional<std::string> ExtractBraceDelimitedBody(const std::string& text,
                                                     std::size_t open_brace_index);
std::optional<std::pair<std::string, std::size_t>> ExtractMemberFunctionBodyWithOffset(
    const std::string& text, std::string_view signature_needle);

}  // namespace microide::tests::architecture
