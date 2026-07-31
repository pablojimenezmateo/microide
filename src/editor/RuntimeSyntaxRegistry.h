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
// Filetype detection only inspects the path and a bounded head of the document
// (signature/shebang scan). Pass a LineSpan over the live buffer; only the head
// is read, so no whole-document materialization happens on per-frame callers.
std::string DetectFiletype(const std::filesystem::path& path, LineSpan lines);
// Path-only detection for callers with no content available.
std::string DetectFiletype(const std::filesystem::path& path);

// One-entry memo for DetectFiletype, for callers that ask repeatedly about the
// same buffer.
//
// Detection is cheap per call but not free: it materializes the signature-scan
// head of the buffer into a vector<std::string> and runs the filename / header /
// signature regexes, then returns an owned std::string. Two per-frame callers
// existed. One (the status-bar language segment) had grown its own four-field
// cache inline; the other (the folding-model refresh, which runs for every editor
// group on every prepared frame) had none, and cost ~0.85 ms per frame on a
// 50k-line buffer -- more than the entire render path in that scenario -- to
// re-derive a string that had not changed.
//
// The answer is a pure function of the buffer's identity, its content, its path,
// and the syntax registry, so the key is exactly those. `owner` is any stable
// per-buffer pointer (the viewport); it is compared, never dereferenced, so a
// recycled address is still safe -- the other three key parts have to match too.
//
// Not thread-safe: one memo belongs to one thread's call site. Background
// detection (the highlight prefetch prewarm) calls DetectState directly and does
// not go through here.
class FiletypeMemo {
 public:
  const std::string& Resolve(const void* owner,
                             const std::filesystem::path& path,
                             std::uint64_t content_revision,
                             LineSpan lines);

  // Drop the memo. Only needed when a caller cannot supply a distinguishing key
  // (e.g. it is about to reuse the same owner for different content).
  void Invalidate() { owner_ = nullptr; }

 private:
  const void* owner_ = nullptr;
  std::filesystem::path path_;
  std::uint64_t content_revision_ = 0;
  std::size_t registry_revision_ = 0;
  std::string filetype_;
};

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
