#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/RuntimeSyntaxData.h"
#include "editor/SyntaxHighlighter.h"

namespace microide::editor::runtime_syntax {

// Above this byte length a line is not tokenized: running the syntax rules over
// the whole line is O(line) work on the UI thread on every token-cache miss, so a
// single enormous line (a minified bundle with no newline) would stall the shell.
// Such lines render unhighlighted (all Plain) — the same threshold behavior
// mature editors use to disable tokenization on very long lines.
//
// Exported because it is the editor's one "this line does not get whole-line
// treatment" threshold, and folding has to agree with it: past this length a line
// has no syntax tokens, so nothing can tell a brace in a string literal from a
// real one (see FoldingModel::kMaxBracketScanLineBytes).
inline constexpr std::size_t kMaxHighlightLineBytes = 100000;

struct RuntimeSyntaxRuleData {
  GeneratedRuleKind kind = GeneratedRuleKind::Pattern;
  std::string group_name;
  std::string limit_group_name;
  std::string pattern;
  std::string start_regex;
  std::string end_regex;
  std::string skip_regex;
  std::vector<RuntimeSyntaxRuleData> children;
};

struct RuntimeSyntaxDefinitionData {
  std::string filetype;
  std::vector<std::string> filename_patterns;
  std::vector<std::string> header_patterns;
  std::vector<std::string> signature_patterns;
  std::vector<RuntimeSyntaxRuleData> rules;
  std::filesystem::path source_path;
};

struct RuntimeSyntaxReloadResult {
  std::size_t built_in_definition_count = 0;
  std::size_t plugin_definition_count = 0;
  std::size_t error_count = 0;
};

RuntimeSyntaxReloadResult ReloadDefinitions(
    const std::vector<RuntimeSyntaxDefinitionData>& definitions,
    std::vector<std::string>* errors = nullptr);
void EnsureInitialized();
std::size_t RegistryRevision();
// Initial highlight state for a document. Like DetectFiletype, this only
// inspects a bounded head (signature/shebang scan), so passing a LineSpan over
// the live buffer never materializes the whole file.
SyntaxState DetectState(const std::filesystem::path& path, LineSpan lines);
// Same, for a caller holding the document as one text blob rather than as lines.
// It splits only the bounded head into views instead of making the caller
// materialize the whole document into owned lines to hand over sixty-four of them
// (TD-2026-08-06-159).
SyntaxState DetectState(const std::filesystem::path& path, std::string_view text);
// Filetype detection only inspects the path and a bounded head of the document
// (signature/shebang scan). Pass a LineSpan over the live buffer; only the head
// is read, so no whole-document materialization happens on per-frame callers.
std::string DetectFiletype(const std::filesystem::path& path, LineSpan lines);
// Path-only detection for callers with no content available.
std::string DetectFiletype(const std::filesystem::path& path);

// Eagerly compile a definition's lazily-built rule regexes. Safe to call from a
// background worker: it is idempotent (std::call_once) and a no-op for an
// unknown id or an already-compiled/eager definition. Used to prewarm a
// cold filetype off the UI thread so the first visible-line highlight does not
// pay the regex-compile cost on the render path.
void CompileDefinition(std::uint32_t definition_id);

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line);
SyntaxState AdvanceState(std::string_view line,
                         const std::filesystem::path& path,
                         const SyntaxState& state,
                         std::string_view first_line = {});

}  // namespace microide::editor::runtime_syntax
