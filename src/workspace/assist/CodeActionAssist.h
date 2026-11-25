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

class CodeActionAssist {
 public:
  struct Port {
    std::function<bool(std::string_view, const std::vector<std::string>&, std::string*)>
        execute_command_name;
    std::function<std::vector<LspClient::Diagnostic>(const editor::TextViewport&,
                                                     const editor::SelectionRange&,
                                                     lsp_encoding::PositionEncoding)>
        collect_lsp_context_diagnostics;
  };

  explicit CodeActionAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  static std::vector<CodeActionSessionItem> TransformLspCodeActions(
      const LspResult<std::vector<LspClient::CodeAction>>& actions);

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
  bool Finish(bool applied, std::string error);

  AssistHost& host_;
  Port port_;
  std::uint64_t request_generation_ = 0;
};

}
