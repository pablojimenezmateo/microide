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

// Shared linear-layer helpers: the single definition that replaces the former
// UpsertSetting / SetStoredSetting / FindStoredSettingValue / FindStoredValue
// near-duplicates. Used for the raw vectors that back persistence and the
// settings overlay; the hot resolution path goes through SettingsStore::Resolve.
namespace settings_layer {
const std::string* Find(const SettingsLayer& layer, std::string_view id);
void Upsert(SettingsLayer& layer, std::string_view id, std::string value);
bool Erase(SettingsLayer& layer, std::string_view id);
}  // namespace settings_layer

// Centralizes resolution and mutation of layered settings: the active project
// layer wins over the user layer (the user layer holds cross-project defaults
// that a project may override). Keeps a resolved index for O(1) reads (the hot
// render path resolves settings 10+ times per frame). The backing vectors stay
// owned by WorkspaceContext / ProjectWorkspaceState so binary persistence and
// the wholesale project-state move keep working; this store binds non-owning
// pointers to them and is the single writer, so the vectors and the index can
// never desync.
class SettingsStore {
 public:
  // Bind the backing vectors. The user layer is stable for the shell's lifetime;
  // the project layer is re-bound on every project switch (the old vector is
  // moved-from and would dangle). Both rebuild the resolved index.
  void BindUserLayer(SettingsLayer* user);
  void BindActiveProject(SettingsLayer* project);

  // Resolved lookup (project over user). Returns nullptr when the id is set in
  // neither layer. The pointer is valid until the next mutation/bind; callers
  // copy the value if it must outlive that.
  const std::string* Resolve(std::string_view id) const;

  // Single mutation choke point. Each updates the backing vector and the index.
  void SetUser(std::string_view id, std::string value);
  void SetProject(std::string_view id, std::string value);
  void ResetUser(std::string_view id);
  void ResetProject(std::string_view id);

  // Rebuild the index after the backing vectors were reloaded in place (the
  // persistence restore paths clear + refill them with canonical side effects).
  void Reindex();

  // Monotonic counter bumped on every mutation, reset, and layer bind (all route
  // through Reindex). Lets per-frame consumers (ApplyLiveSettings) skip redundant
  // work when nothing resolvable changed since the last apply.
  std::uint64_t Revision() const { return revision_; }

  // Raw scope-specific lookups for the settings overlay (user-vs-project label).
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

}  // namespace microide::workspace
