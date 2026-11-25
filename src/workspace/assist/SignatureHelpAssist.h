#pragma once

#include <functional>
#include <utility>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/AssistProviderMerge.h"

namespace microide::workspace {

class SignatureHelpAssist {
 public:
  struct Port {
    std::function<void(std::string, std::string)> show_signature_help;
    std::function<bool()> signature_help_showing;
    std::function<void()> anchor_signature_help_at_caret;
  };

  explicit SignatureHelpAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool Show(std::string* error_message = nullptr);
  void AfterTyping(std::string_view typed);

 private:
  struct Merge {
    assist_merge::TwoSourceState sources;
    bool lsp_has = false;
    std::string lsp_signature;
    std::string lsp_documentation;
    bool plugin_has = false;
    std::string plugin_signature;
    std::string plugin_documentation;
    bool acted = false;
    bool close_when_empty = false;
    std::uint64_t generation = 0;
  };

  void Request(editor::TextViewport& viewport, bool close_when_empty);
  bool IsTrigger(editor::TextViewport& viewport, char c) const;
  void Resolve(const std::shared_ptr<Merge>& merge, const std::filesystem::path& request_path);

  AssistHost& host_;
  Port port_;
  std::uint64_t request_generation_ = 0;
};

}
