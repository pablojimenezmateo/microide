#include "workspace/SettingsStore.h"

#include <algorithm>

namespace microide::workspace {

namespace settings_layer {

const std::string* Find(const SettingsLayer& layer, std::string_view id) {
  const auto it = std::find_if(layer.begin(), layer.end(),
                               [id](const auto& entry) { return entry.first == id; });
  return it == layer.end() ? nullptr : &it->second;
}

void Upsert(SettingsLayer& layer, std::string_view id, std::string value) {
  const auto it = std::find_if(layer.begin(), layer.end(),
                               [id](const auto& entry) { return entry.first == id; });
  if (it != layer.end()) {
    it->second = std::move(value);
    return;
  }
  layer.emplace_back(std::string(id), std::move(value));
}

bool Erase(SettingsLayer& layer, std::string_view id) {
  const auto new_end = std::remove_if(layer.begin(), layer.end(),
                                      [id](const auto& entry) { return entry.first == id; });
  if (new_end == layer.end()) {
    return false;
  }
  layer.erase(new_end, layer.end());
  return true;
}

}  // namespace settings_layer

void SettingsStore::BindUserLayer(SettingsLayer* user) {
  user_ = user;
  Reindex();
}

void SettingsStore::BindActiveProject(SettingsLayer* project) {
  project_ = project;
  Reindex();
}

void SettingsStore::Reindex() {
  ++revision_;
  resolved_.clear();
  // User layer first (the cross-project default), then the active project layer
  // overwrites it: a per-project override wins over the user-level default. This
  // matches the "set as default" model where the user layer holds defaults that
  // projects may override (mirrors VS Code's User vs Workspace precedence).
  if (user_ != nullptr) {
    for (const auto& [id, value] : *user_) {
      resolved_[id] = value;
    }
  }
  if (project_ != nullptr) {
    for (const auto& [id, value] : *project_) {
      resolved_[id] = value;
    }
  }
}

const std::string* SettingsStore::Resolve(std::string_view id) const {
  const auto it = resolved_.find(id);
  return it == resolved_.end() ? nullptr : &it->second;
}

// A setting id must be a non-empty token with no whitespace or control bytes.
// An empty id, or one containing a newline / space / pathlike separator, would
// corrupt the persisted layer's text encoding and confuse overlays and LSP
// configuration mapping. Rejecting it at the mutation boundary keeps every
// downstream reader dealing only with well-formed ids.
bool SettingsStore::IsValidSettingId(std::string_view id) {
  if (id.empty()) {
    return false;
  }
  for (const char ch : id) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte <= 0x20 || byte == 0x7F) {  // control chars and space
      return false;
    }
  }
  return true;
}

// Mutations are cold (user actions, config load, control spec) — never per
// frame — so each rebuilds the index wholesale. This keeps precedence trivially
// correct and avoids any chance of the vectors and index drifting apart.
void SettingsStore::SetUser(std::string_view id, std::string value) {
  if (user_ == nullptr || !IsValidSettingId(id)) {
    return;
  }
  settings_layer::Upsert(*user_, id, std::move(value));
  Reindex();
}

void SettingsStore::SetProject(std::string_view id, std::string value) {
  if (project_ == nullptr || !IsValidSettingId(id)) {
    return;
  }
  settings_layer::Upsert(*project_, id, std::move(value));
  Reindex();
}

void SettingsStore::ResetUser(std::string_view id) {
  if (user_ != nullptr && settings_layer::Erase(*user_, id)) {
    Reindex();
  }
}

void SettingsStore::ResetProject(std::string_view id) {
  if (project_ != nullptr && settings_layer::Erase(*project_, id)) {
    Reindex();
  }
}

const std::string* SettingsStore::FindInUserLayer(std::string_view id) const {
  return user_ != nullptr ? settings_layer::Find(*user_, id) : nullptr;
}

const std::string* SettingsStore::FindInProjectLayer(std::string_view id) const {
  return project_ != nullptr ? settings_layer::Find(*project_, id) : nullptr;
}

}  // namespace microide::workspace
