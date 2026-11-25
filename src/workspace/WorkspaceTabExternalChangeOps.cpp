#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <filesystem>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

bool TabCoordinator::OverwriteEditorTabsForPath(const std::filesystem::path& path) {
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native()) ? (normalized_storage = path.lexically_normal())
                                                    : path;
  if (!RefuseIfReadOnly(normalized_path, true)) {
    return false;
  }
  bool saved_any = false;
  for (EditorGroup& group : state_.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path) || !viewport.dirty()) {
        continue;
      }
      if (operations_.prepare_editor_view_for_save &&
          !operations_.prepare_editor_view_for_save(viewport.path(), viewport, nullptr,
                                                    SaveMode::Blocking)
               .ok()) {
        continue;
      }
      if (viewport.Save(state_.write_gate())) {
        saved_any = true;
        operations_.invalidate_editor_blame_path(normalized_path);
        operations_.notify_plugin_buffer_save(normalized_path);
      }
    }
  }
  if (saved_any) {
    state_.directory_tree.Refresh();
    util::InvokeIfSet(operations_.request_automatic_git_sidebar_refresh);
  }
  return saved_any;
}

namespace {

struct OpenViewSweep {
  bool matched_any = false;
  bool stopped = false;
};

template <typename Visit>
OpenViewSweep SweepOpenViewsOf(const std::vector<EditorGroup>& groups,
                               const std::filesystem::path& path, Visit&& visit) {
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native())
          ? (normalized_storage = path.lexically_normal())
          : path;
  OpenViewSweep sweep;
  util::FileSignature signature;
  for (const EditorGroup& group : groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path)) {
        continue;
      }
      if (!sweep.matched_any) {
        signature = util::StatFileSignature(normalized_path);
        sweep.matched_any = true;
      }
      if (!visit(viewport, signature, normalized_path)) {
        sweep.stopped = true;
        return sweep;
      }
    }
  }
  return sweep;
}

}

bool TabCoordinator::DiskSignatureMatchesOpenView(const std::filesystem::path& path) const {
  std::optional<std::uint64_t> disk_content_hash;
  bool disk_read_failed = false;
  const OpenViewSweep sweep = SweepOpenViewsOf(
      state_.editor_groups, path,
      [&](const editor::TextViewport& viewport, const util::FileSignature& signature,
          const std::filesystem::path& normalized_path) {
        if (signature.SameContentAs(viewport.disk_signature())) {
          return true;
        }
        if (!viewport.CouldConfirmDiskContent(signature)) {
          return signature.SameStatAs(viewport.disk_signature());
        }
        if (!disk_content_hash.has_value() && !disk_read_failed) {
          util::AddPerformanceCounter(util::PerfCounterId::ExternalChangeConfirmInlineReads);
          if (const std::optional<std::string> bytes = util::ReadTextFile(normalized_path);
              bytes.has_value()) {
            disk_content_hash = util::QuickContentHash(*bytes);
          } else {
            disk_read_failed = true;
          }
        }
        return disk_content_hash.has_value() &&
               viewport.ConfirmDiskContentUnchanged(signature, *disk_content_hash);
      });
  return sweep.matched_any && !sweep.stopped;
}

TabCoordinator::ExternalChangeVerdict TabCoordinator::ClassifyExternalChange(
    const std::filesystem::path& path) const {
  bool needs_confirm = false;
  const OpenViewSweep sweep = SweepOpenViewsOf(
      state_.editor_groups, path,
      [&](const editor::TextViewport& viewport, const util::FileSignature& signature,
          const std::filesystem::path&) {
        if (signature.SameContentAs(viewport.disk_signature())) {
          return true;
        }
        if (!viewport.CouldConfirmDiskContent(signature)) {
          return signature.SameStatAs(viewport.disk_signature());
        }
        needs_confirm = true;
        return true;
      });
  if (sweep.stopped) {
    return ExternalChangeVerdict::Changed;
  }
  if (!sweep.matched_any) {
    return ExternalChangeVerdict::Changed;
  }
  return needs_confirm ? ExternalChangeVerdict::NeedsContentConfirm
                       : ExternalChangeVerdict::OwnEcho;
}

bool TabCoordinator::ExternalChangeIsOwnEcho(const std::filesystem::path& path,
                                             std::uint64_t disk_content_hash,
                                             const util::FileSignature& read_signature) const {
  const OpenViewSweep sweep = SweepOpenViewsOf(
      state_.editor_groups, path,
      [&](const editor::TextViewport& viewport, const util::FileSignature& signature,
          const std::filesystem::path&) {
        return signature.SameContentAs(viewport.disk_signature()) ||
               (signature.SameStatAs(read_signature) &&
                viewport.ConfirmDiskContentUnchanged(signature, disk_content_hash));
      });
  return sweep.matched_any && !sweep.stopped;
}

void TabCoordinator::RefreshRealPathsOf(const std::filesystem::path& normalized_path) {
  const editor::TextViewport* refreshed = nullptr;
  for (EditorGroup& group : state_.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (LoadedBufferViewOf(tab) == nullptr) {
        continue;
      }
      editor::TextViewport& viewport =
          tab.kind == TabEntry::Kind::Compare ? tab.compare->right_viewport : tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path)) {
        continue;
      }
      if (refreshed == nullptr || !viewport.SharesDocumentWith(*refreshed)) {
        viewport.RefreshRealPath();
        refreshed = &viewport;
      }
    }
  }
}

std::vector<std::filesystem::path> TabCoordinator::OpenSymlinkAliasesOf(
    const std::filesystem::path& normalized_target) const {
  std::vector<std::filesystem::path> aliases;
  std::optional<std::filesystem::path> real_target;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      const editor::TextViewport* loaded = LoadedBufferViewOf(tab);
      if (loaded == nullptr) {
        continue;
      }
      const editor::TextViewport& viewport = *loaded;
      if (viewport.real_path().empty() ||
          util::SameAsNormalizedPath(viewport.path(), normalized_target)) {
        continue;
      }
      if (!real_target.has_value()) {
        real_target = util::ResolveRealPath(normalized_target);
      }
      if (!util::SameAsNormalizedPath(util::ResolveRealPath(viewport.path()), *real_target)) {
        continue;
      }
      const bool seen = std::any_of(aliases.begin(), aliases.end(), [&](const auto& alias) {
        return util::SameAsNormalizedPath(viewport.path(), alias);
      });
      if (!seen) {
        aliases.push_back(viewport.path());
      }
    }
  }
  return aliases;
}

}
