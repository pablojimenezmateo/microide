#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/assist/AssistHost.h"

namespace microide::workspace {

// The language server's whole-symbol and whole-document edits: rename and format.
class RefactorAssist {
 public:
  explicit RefactorAssist(AssistHost& host) : host_(host) {}

  // Format the active editable buffer via the language server's
  // textDocument/formatting (rangeFormatting with a selection) and apply the
  // returned edits to the open buffer.
  bool FormatActiveDocument(std::string* error_message = nullptr);
  // Prepare-rename probe for the symbol under the cursor: resolves the LSP client,
  // fires textDocument/prepareRename, and delivers the result to `callback` on the
  // main-thread drain. `callback(can_rename, placeholder)` — placeholder is the
  // server's suggested seed (may be empty). Never invoked when no server serves the
  // buffer (the caller keeps its heuristic seed and validates on rename).
  void PrepareRenameForCursor(std::function<void(bool, std::string)> callback);
  // The identifier under the cursor in the active editable buffer (word chars:
  // alnum + '_'), or empty when the cursor is not on one. Used to prefill the
  // rename prompt.
  std::string SymbolAtCursor() const;
  // Rename the symbol under the cursor to `new_name` via the language server's
  // textDocument/rename, applying the returned workspace edit across open buffers.
  bool RenameSymbol(const std::string& new_name, std::string* error_message = nullptr);

 private:
  // Apply a textDocument/formatting or rangeFormatting result to the active buffer.
  // Drops superseded results.
  void ApplyFormattingResult(const std::filesystem::path& request_path,
                             LspResult<std::vector<LspClient::TextEdit>> edits);
  void ApplyRenameResult(const std::filesystem::path& request_path, const std::string& new_name,
                         const LspClient& client, LspResult<LspClient::WorkspaceEdit> edit);

  AssistHost& host_;
};

}  // namespace microide::workspace
