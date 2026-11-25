#include "workspace/persistence/SettingsStore.h"

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

}

void SettingsStore::BindUserLayer(SettingsLayer* user) {
  user_ = user;
  Reindex();
}

void SettingsStore::BindActiveProject(SettingsLayer* project) {
  project_ = project;
  Reindex();
}

void SettingsStore::Reindex() {
  std::unordered_map<std::string, std::string, TransparentHash, std::equal_to<>> next;
  if (user_ != nullptr) {
    for (const auto& [id, value] : *user_) {
      next[id] = value;
    }
  }
  if (project_ != nullptr) {
    for (const auto& [id, value] : *project_) {
      next[id] = value;
    }
  }
  if (next != resolved_) {
    resolved_ = std::move(next);
    ++revision_;
  }
}

const std::string* SettingsStore::Resolve(std::string_view id) const {
  const auto it = resolved_.find(id);
  return it == resolved_.end() ? nullptr : &it->second;
}

bool SettingsStore::IsValidSettingId(std::string_view id) {
  if (id.empty()) {
    return false;
  }
  for (const char ch : id) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte <= 0x20 || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

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

}
