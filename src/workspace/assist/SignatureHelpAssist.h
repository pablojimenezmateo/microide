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

// Parameter hints: the caret-anchored signature popup, asked for explicitly or
// opened and followed while typing a call.
class SignatureHelpAssist {
 public:
  // The shell callbacks only this feature reads (AssistHost::Operations has the
  // shared ones).
  struct Port {
    // Hand a prebuilt (signature, documentation) pair to the host's caret-anchored
    // signature-help popup. Empty signature clears it.
    std::function<void(std::string, std::string)> show_signature_help;
    // Is the signature-help popup up? Typing inside the call re-asks for it.
    std::function<bool()> signature_help_showing;
    // Re-anchor the popup at the caret, so the hint stays on screen across the
    // keystroke instead of expiring until the re-asked answer arrives.
    std::function<void()> anchor_signature_help_at_caret;
  };

  explicit SignatureHelpAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool Show(std::string* error_message = nullptr);
  // Text was typed into the editor. Parameter hints open on a trigger character
  // (the server's signatureHelpProvider.triggerCharacters, a plugin provider's
  // `trigger_characters`, `(` and `,` by default) and, while they are up, are
  // re-asked on every keystroke: the active parameter follows `,`, and they
  // close once the answer is empty (a `)` that ends the call).
  void AfterTyping(std::string_view typed);

 private:
  // Signature help is a single caret-anchored popup, so the two sources are
  // *chosen* between (LSP-primary, like navigation) rather than unioned. Each
  // source lowers its raw result into the display (signature, documentation) pair
  // as it arrives; the resolver shows exactly one, once.
  struct Merge {
    assist_merge::TwoSourceState sources;
    bool lsp_has = false;
    std::string lsp_signature;
    std::string lsp_documentation;
    bool plugin_has = false;
    std::string plugin_signature;
    std::string plugin_documentation;
    bool acted = false;
    // A re-ask while the hints are up: an empty answer closes them (the caret left
    // the call) instead of leaving the old signature on screen.
    bool close_when_empty = false;
    std::uint64_t generation = 0;  // see request_generation_ (TD-2026-07-17A-030)
  };

  // Ask both sources for the signature at the caret.
  void Request(editor::TextViewport& viewport, bool close_when_empty);
  // Does typing `c` into this buffer open parameter hints?
  bool IsTrigger(editor::TextViewport& viewport, char c) const;
  // Show the chosen popup once the preferred source resolves (LSP-primary; plugin
  // used only when the server serves nothing).
  void Resolve(const std::shared_ptr<Merge>& merge, const std::filesystem::path& request_path);

  AssistHost& host_;
  Port port_;
  std::uint64_t request_generation_ = 0;
};

}  // namespace microide::workspace
