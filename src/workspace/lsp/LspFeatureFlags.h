#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "workspace/SettingFlags.h"

namespace microide::workspace {

// Central interpretation of the LSP feature toggles registered in
// WorkspaceSettingsRegistry ("lsp.enabled" master + per-feature "lsp.*.enabled").
// A feature is live only when BOTH the master switch and its own toggle are on;
// this helper dedups that "master AND feature" check across every gating call site
// (action availability, hover, outline, diagnostics, semantic tokens, menu hiding).
//
// `Getter` is any callable of shape (std::string_view) -> std::optional<std::string>
// (e.g. WorkspaceShell::GetSettingValue via a lambda, or LspService's Operations op),
// so the check stays allocation-free and header-only like SettingFlags.h.

template <typename Getter>
[[nodiscard]] inline bool LspMasterEnabled(const Getter& get) {
  return SettingFlagEnabled(get("lsp.enabled"), /*default_value=*/true);
}

template <typename Getter>
[[nodiscard]] inline bool LspFeatureEnabled(const Getter& get, std::string_view feature_id) {
  return LspMasterEnabled(get) && SettingFlagEnabled(get(feature_id), /*default_value=*/true);
}

}  // namespace microide::workspace
