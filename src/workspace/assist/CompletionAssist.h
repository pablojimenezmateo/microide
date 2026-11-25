#pragma once

#include <functional>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/AssistProviderMerge.h"

namespace microide::workspace {

class CompletionAssist {
 public:
  struct Port {
    std::function<bool(std::string_view)> has_completion_provider;
    std::function<std::string(std::string_view)> plugin_completion_trigger_characters;
  };

  explicit CompletionAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool ShowCompletionOverlay(std::string* error_message = nullptr);
  void FollowCompletionCaret();
  void AfterTyping(std::string_view typed);
  bool ApplySelectedCompletion();
  bool ShowInsertSnippetOverlay(std::string* error_message = nullptr);

 private:
  struct Merge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CompletionSessionItem> lsp_items;
    std::vector<CompletionSessionItem> plugin_items;
    std::uint64_t generation = 0;
    bool lsp_incomplete = false;
    std::size_t request_column = 0;
  };

  CompletionSessionState& ResetSession(const editor::TextViewport& viewport, bool automatic,
                                       const char* source);
  std::vector<CompletionSessionItem> TransformPluginCompletions(
      const std::vector<plugin::PluginHost::CompletionCandidate>& items) const;
  std::vector<CompletionSessionItem> TransformLspCompletions(
      const std::vector<LspClient::CompletionItem>& items,
      lsp_encoding::PositionEncoding encoding) const;
  void RequestCompletions(editor::TextViewport& viewport, bool refresh,
                          LspClient::CompletionTrigger trigger = LspClient::CompletionTrigger::kInvoked,
                          char trigger_character = '\0');
  void StartAutomaticCompletion(editor::TextViewport& viewport, char trigger_character);
  bool RefilterCompletion(CompletionSessionState& session, const editor::TextViewport& viewport,
                          bool keep_selection = false);
  void PublishMerge(const std::shared_ptr<Merge>& merge, const std::filesystem::path& request_path);

  AssistHost& host_;
  Port port_;
  std::uint64_t request_generation_ = 0;
  std::vector<std::pair<int, std::uint32_t>> rank_scratch_;
  std::optional<editor::TextPosition> auto_anchor_;
};

}
