#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/assist/AssistHost.h"

namespace microide::workspace {

class RefactorAssist {
 public:
  explicit RefactorAssist(AssistHost& host) : host_(host) {}

  bool FormatActiveDocument(std::string* error_message = nullptr);
  void PrepareRenameForCursor(std::function<void(bool, std::string)> callback);
  std::string SymbolAtCursor() const;
  bool RenameSymbol(const std::string& new_name, std::string* error_message = nullptr);

 private:
  void ApplyFormattingResult(const std::filesystem::path& request_path, std::uint64_t revision,
                             LspResult<std::vector<LspClient::TextEdit>> edits);
  void ApplyRenameResult(const std::filesystem::path& request_path, const std::string& new_name,
                         const LspClient& client, std::uint64_t revision,
                         LspResult<LspClient::WorkspaceEdit> edit);

  AssistHost& host_;
};

}
