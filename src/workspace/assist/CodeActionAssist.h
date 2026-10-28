#pragma once

#include <functional>
#include <utility>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/AssistProviderMerge.h"

namespace microide::workspace {

// The code-actions list (Ctrl+.): both sources queried at once, LSP-first, and
// the selected action applied as an inline WorkspaceEdit or run as a command.
class CodeActionAssist {
 public:
  // The shell callbacks only this feature reads (AssistHost::Operations has the
  // shared ones).
  struct Port {
    std::function<bool(std::string_view, const std::vector<std::string>&, std::string*)>
        execute_command_name;
    // Collect the diagnostics for the viewport's file that overlap `range`,
    // converted to LSP wire form (columns in the server's position encoding), to
    // populate a codeAction request `context`. The encoding must match the request
    // Range's, or clangd (UTF-16) fails to match diagnostics on non-ASCII lines.
    std::function<std::vector<LspClient::Diagnostic>(const editor::TextViewport&,
                                                     const editor::SelectionRange&,
                                                     lsp_encoding::PositionEncoding)>
        collect_lsp_context_diagnostics;
  };

  explicit CodeActionAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  // Convert LSP code actions into overlay session items, materializing each
  // action's inline WorkspaceEdit under a SHARED aggregate edit/byte budget so a
  // server returning many large (but individually capped) fixes cannot make the
  // overlay hold the sum of every action's edit payload before the user selects
  // one. Past the budget an action's inline fix is dropped (edits_truncated set).
  // Static + free of member state so the budget is unit-testable. TD-2026-07-17A-057.
  static std::vector<CodeActionSessionItem> TransformLspCodeActions(
      const LspResult<std::vector<LspClient::CodeAction>>& actions);

  // `explicit_range`, when set, targets code actions at that document range
  // (e.g. a diagnostic's range from the hover "Quick Fix" affordance) instead of
  // the current cursor/selection.
  bool ShowOverlay(std::string* error_message = nullptr,
                   const editor::SelectionRange* explicit_range = nullptr);
  bool ExecuteSelected();

 private:
  struct Merge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CodeActionSessionItem> lsp_items;
    std::vector<CodeActionSessionItem> plugin_items;
    std::uint64_t generation = 0;
  };

  static std::vector<CodeActionSessionItem> TransformPluginCodeActions(
      const std::vector<plugin::PluginHost::CodeActionCandidate>& items);
  void PublishMerge(const std::shared_ptr<Merge>& merge, const std::filesystem::path& request_path);
  // Close the list when `applied`, else show `error` in it.
  bool Finish(bool applied, std::string error);

  AssistHost& host_;
  Port port_;
  std::uint64_t request_generation_ = 0;
};

}  // namespace microide::workspace
