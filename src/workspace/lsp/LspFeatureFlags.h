#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "workspace/SettingFlags.h"

namespace microide::workspace {

template <typename Getter>
[[nodiscard]] inline bool LspMasterEnabled(const Getter& get) {
  return SettingFlagEnabled(get("lsp.enabled"), true);
}

template <typename Getter>
[[nodiscard]] inline bool LspFeatureEnabled(const Getter& get, std::string_view feature_id) {
  return LspMasterEnabled(get) && SettingFlagEnabled(get(feature_id), true);
}

}
