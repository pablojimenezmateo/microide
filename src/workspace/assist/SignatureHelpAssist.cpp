#include "workspace/assist/SignatureHelpAssist.h"

#include <optional>
#include <utility>

#include "workspace/FileUri.h"
#include "workspace/SettingFlags.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/lsp/LspViewportPositions.h"

namespace microide::workspace {

namespace {

using LoweredSignature = std::pair<std::string, std::string>;

// The popup's documentation block: the active parameter first (so the user sees
// which argument they are typing), then the signature's own documentation.
std::string ComposeSignatureDocumentation(std::string_view parameter_label,
                                          std::string_view parameter_documentation,
                                          std::string_view signature_documentation) {
  std::string documentation(parameter_label);
  if (!parameter_documentation.empty()) {
    if (!documentation.empty()) {
      documentation += " — ";
    }
    documentation += parameter_documentation;
  }
  if (!signature_documentation.empty()) {
    if (!documentation.empty()) {
      documentation += "\n";
    }
    documentation += signature_documentation;
  }
  return documentation;
}

// An index the source reports, or 0 when it is out of range.
std::size_t ActiveIndex(int index, std::size_t count) {
  return index >= 0 && static_cast<std::size_t>(index) < count ? static_cast<std::size_t>(index)
                                                                : 0;
}

// Lower a signature-help result to the display (signature, documentation) pair the
// caret-anchored popup shows: the active overload's label plus the documentation
// block. Returns nullopt when there is nothing to show. Both sources lower through
// one composer so either renders identically.
template <typename Signature>
LoweredSignature LowerSignature(const Signature& signature, int active_parameter) {
  if (active_parameter >= 0 &&
      static_cast<std::size_t>(active_parameter) < signature.parameters.size()) {
    const auto& parameter = signature.parameters[static_cast<std::size_t>(active_parameter)];
    return {signature.label, ComposeSignatureDocumentation(parameter.label, parameter.documentation,
                                                           signature.documentation)};
  }
  return {signature.label, ComposeSignatureDocumentation({}, {}, signature.documentation)};
}

std::optional<LoweredSignature> LowerPluginSignature(
    const plugin::PluginHost::SignatureHelpResult& result) {
  if (result.signatures.empty()) {
    return std::nullopt;
  }
  const auto& signature =
      result.signatures[ActiveIndex(result.active_signature, result.signatures.size())];
  return LowerSignature(signature, signature.active_parameter);
}

std::optional<LoweredSignature> LowerLspSignature(const LspClient::SignatureHelp& help) {
  if (help.signatures.empty()) {
    return std::nullopt;
  }
  const auto& signature = help.signatures[ActiveIndex(help.active_signature, help.signatures.size())];
  // A per-signature activeParameter (LSP 3.16+) overrides the top-level one.
  return LowerSignature(signature, signature.active_parameter >= 0 ? signature.active_parameter
                                                                   : help.active_parameter);
}

}  // namespace

bool SignatureHelpAssist::Show(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  Request(*viewport, /*close_when_empty=*/false);
  return true;
}

bool SignatureHelpAssist::IsTrigger(editor::TextViewport& viewport, char c) const {
  const AssistHost::Operations& ops = host_.ops();
  if (LspFeatureEnabled(ops.get_setting_value, "lsp.signature_help.enabled")) {
    if (LspClient* client = ops.lsp_client_for_viewport(viewport, nullptr);
        client != nullptr && client->IsSignatureHelpTriggerCharacter(c)) {
      return true;
    }
  }
  const plugin::PluginHost::ContributedLanguageProvider* provider =
      host_.plugins().Host().FindLanguageProvider(
          plugin::PluginHost::ContributedLanguageProvider::Kind::SignatureHelp,
          viewport.language_id());
  return provider != nullptr && provider->trigger_characters.find(c) != std::string::npos;
}

void SignatureHelpAssist::AfterTyping(std::string_view typed) {
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  // A paste or a multi-caret edit is not typing into one call: the hints, if up,
  // expire with the caret move.
  if (typed.size() != 1 || viewport == nullptr || viewport->path().empty() ||
      viewport->has_multiple_carets() ||
      !SettingFlagEnabled(ops.get_setting_value("editor.parameterHints"),
                          /*default_value=*/true)) {
    return;
  }
  const bool showing = port_.signature_help_showing && port_.signature_help_showing();
  if (!showing && !IsTrigger(*viewport, typed.front())) {
    return;
  }
  // A language server knows where a call ends and answers the re-ask with the
  // enclosing call or nothing; a plugin provider is asked by position alone and
  // commonly answers the same signature anywhere, so `)` closes its hints.
  if (showing && typed.front() == ')' && ops.lsp_client_for_viewport(*viewport, nullptr) == nullptr) {
    port_.show_signature_help({}, {});
    return;
  }
  if (showing && port_.anchor_signature_help_at_caret) {
    port_.anchor_signature_help_at_caret();
  }
  Request(*viewport, /*close_when_empty=*/showing);
}

void SignatureHelpAssist::Request(editor::TextViewport& viewport, bool close_when_empty) {
  // Query the plugin provider and the language server CONCURRENTLY (mirrors
  // completion / definition). The server is authoritative for its language: the
  // popup waits for its answer and uses the plugin result only if the server
  // comes back empty. The lookups run on the worker / I/O thread so neither
  // blocks the UI; the popup is chosen and shown from the mailbox drain.
  const std::string language_id = viewport.language_id();
  const std::filesystem::path request_path = viewport.path();
  const std::size_t request_line = viewport.cursor_line();
  const std::size_t request_column = viewport.cursor_column();

  auto merge = std::make_shared<Merge>();
  merge->generation = ++request_generation_;
  merge->close_when_empty = close_when_empty;

  const AssistHost::Operations& ops = host_.ops();
  LspClient* client = ops.lsp_client_for_viewport(viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    ops.ensure_lsp_document_open(viewport, *client, language_id);
    ops.begin_tracked_lsp_request();
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    client->RequestSignatureHelpAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(viewport, request_line, request_column, encoding),
        [this, request_path, merge](LspResult<LspClient::SignatureHelp> help) {
          host_.ops().finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (help.has_value()) {
            if (auto lowered = LowerLspSignature(*help)) {
              merge->lsp_has = true;
              merge->lsp_signature = std::move(lowered->first);
              merge->lsp_documentation = std::move(lowered->second);
            }
          }
          Resolve(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  host_.plugins().Host().QuerySignatureHelpAsync(
      language_id, request_path, request_line + 1, request_column + 1,
      [this, request_path, merge](bool resolved, plugin::PluginHost::SignatureHelpResult result,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (resolved) {
          if (auto lowered = LowerPluginSignature(result)) {
            merge->plugin_has = true;
            merge->plugin_signature = std::move(lowered->first);
            merge->plugin_documentation = std::move(lowered->second);
          }
        }
        Resolve(merge, request_path);
      });
}

void SignatureHelpAssist::Resolve(const std::shared_ptr<Merge>& merge,
                                  const std::filesystem::path& request_path) {
  if (merge->acted) {
    return;
  }
  if (merge->generation != request_generation_ || host_.IsStale(request_path)) {
    return;  // buffer switched or a newer signature request superseded this one
  }
  const assist_merge::NavChoice choice = assist_merge::ChooseNavigation(
      merge->sources.lsp_authoritative, merge->sources.lsp_pending, merge->lsp_has,
      merge->sources.plugin_pending, merge->plugin_has);
  if (choice == assist_merge::NavChoice::Pending) {
    return;
  }
  merge->acted = true;
  const auto& show = port_.show_signature_help;
  if (!show) {
    return;
  }
  switch (choice) {
    case assist_merge::NavChoice::Pending:
      return;
    case assist_merge::NavChoice::None:
      // Nothing to show. An explicit request or a fresh trigger leaves any popup
      // untouched (the host lifecycle dismisses it on Escape / caret moves); a
      // re-ask while the hints are up means the caret has left the call.
      if (merge->close_when_empty) {
        show({}, {});
      }
      return;
    case assist_merge::NavChoice::UsePlugin:
      show(merge->plugin_signature, merge->plugin_documentation);
      return;
    case assist_merge::NavChoice::UseLsp:
      show(merge->lsp_signature, merge->lsp_documentation);
      return;
  }
}

}  // namespace microide::workspace
