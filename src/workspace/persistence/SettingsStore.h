#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace microide::workspace {

using SettingsLayer = std::vector<std::pair<std::string, std::string>>;

namespace settings_layer {
const std::string* Find(const SettingsLayer& layer, std::string_view id);
void Upsert(SettingsLayer& layer, std::string_view id, std::string value);
bool Erase(SettingsLayer& layer, std::string_view id);
}

class SettingsStore {
 public:
  void BindUserLayer(SettingsLayer* user);
  void BindActiveProject(SettingsLayer* project);

  const std::string* Resolve(std::string_view id) const;

  static bool IsValidSettingId(std::string_view id);

  void SetUser(std::string_view id, std::string value);
  void SetProject(std::string_view id, std::string value);
  void ResetUser(std::string_view id);
  void ResetProject(std::string_view id);

  void Reindex();

  std::uint64_t Revision() const { return revision_; }

  const std::string* FindInUserLayer(std::string_view id) const;
  const std::string* FindInProjectLayer(std::string_view id) const;

 private:
  struct TransparentHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };

  SettingsLayer* user_ = nullptr;
  SettingsLayer* project_ = nullptr;
  std::unordered_map<std::string, std::string, TransparentHash, std::equal_to<>> resolved_;
  std::uint64_t revision_ = 0;
};

}
