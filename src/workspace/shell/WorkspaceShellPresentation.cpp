#include "workspace/services/TerminalPanelService.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "project/GitRepositoryMetadataTracker.h"
#include "project/GitMetadataSource.h"
#include "util/PathMatch.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

const std::string& WorkspaceShell::BreadcrumbLabel() const {
  return BreadcrumbLabel(context_.current_project_state.clamped_focused_group_index());
}

const std::string& WorkspaceShell::BreadcrumbLabel(std::size_t group_index) const {
  static const std::filesystem::path kEmptyPath;
  const std::vector<EditorGroup>& groups = context_.current_project_state.editor_groups;
  const std::size_t slot = group_index < groups.size() && group_index < kMaxEditorGroups
                               ? group_index
                               : context_.current_project_state.clamped_focused_group_index();
  BreadcrumbLabelCache& cache = breadcrumb_label_cache_[std::min(slot, kMaxEditorGroups - 1)];
  int mode = 0;
  bool placeholder = false;
  const std::filesystem::path* path = &kEmptyPath;
  std::string_view left;
  std::string_view right;
  const EditorGroup& group = groups[std::min(slot, groups.size() - 1)];
  const TabEntry* active = group.active_tab_index < group.open_tabs.size()
                               ? &group.active_tab()
                               : nullptr;
  if (active != nullptr && active->kind == TabEntry::Kind::Compare) {
    if (!active->compare.has_value()) {
      cache.valid = false;
      cache.label = "compare";
      return cache.label;
    }
    mode = 1;
    path = &active->compare->path;
    left = active->compare->left_label;
    right = active->compare->right_label;
  } else if (active != nullptr && active->kind == TabEntry::Kind::Merge) {
    if (!active->merge.has_value()) {
      cache.valid = false;
      cache.label = "merge";
      return cache.label;
    }
    mode = 2;
    path = &active->merge->output_path;
    left = active->merge->incoming_label;
    right = active->merge->current_label;
  } else {
    const editor::TextViewport* viewport = GroupActiveViewport(group);
    mode = 0;
    if (viewport != nullptr) {
      path = &viewport->path();
      placeholder = viewport->is_placeholder();
    }
  }

  const std::filesystem::path& root = context_.current_project_state.root;
  if (cache.valid && cache.mode == mode && cache.placeholder == placeholder &&
      cache.root == root && cache.path == *path && cache.left_label == left &&
      cache.right_label == right) {
    return cache.label;
  }

  switch (mode) {
    case 1:
      cache.label = BuildCompareBreadcrumbLabel(root, *path, left, right);
      break;
    case 2:
      cache.label = BuildMergeBreadcrumbLabel(root, *path, left, right);
      break;
    default:
      cache.label = context_.current_project_state.launcher().DisplayPathOf(*path);
      if (cache.label.empty()) {
        cache.label = BuildEditorBreadcrumbLabel(root, *path, placeholder);
      }
      break;
  }
  cache.valid = true;
  cache.mode = mode;
  cache.placeholder = placeholder;
  cache.root = root;
  cache.path = *path;
  cache.left_label = left;
  cache.right_label = right;
  return cache.label;
}

std::string WorkspaceShell::ProjectLabel() const {
  const project::ProjectId& id = context_.current_project_state.id;
  return id.empty() ? "microide" : id.Label();
}

namespace {

const ProjectWorkspaceState* BackgroundProjectEntry(const WorkspaceContext& context, std::size_t index) {
  const bool front = !context.current_project_state.root.empty() && index == context.project_catalog.active_index;
  return front || index >= context.project_catalog.entries.size() ? nullptr
                                                                  : context.project_catalog.entries[index].get();
}

}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const project::ProjectId& id = context_.ProjectCatalogId(index);
  const std::string label = id.empty() ? "Welcome" : id.Label();
  std::string title = HasDirtyEditorTabForProject(index) ? "*" + label : label;
  if (const ProjectWorkspaceState* project = BackgroundProjectEntry(context_, index);
      project != nullptr && TerminalPanelService::HasUnseenTerminalActivity(*project)) {
    title += " \xe2\x80\xa2";
  }
  return title;
}

std::string WorkspaceShell::ProjectTabTooltipLabel(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const project::ProjectId& id = context_.ProjectCatalogId(index);
  std::string label = id.empty() ? "Welcome" : id.Location();
  if (const ProjectWorkspaceState* project = BackgroundProjectEntry(context_, index);
      project != nullptr && TerminalPanelService::HasUnseenTerminalActivity(*project)) {
    label += "\nA terminal printed or rang since you left";
  }
  return label;
}

SDL_FRect WorkspaceShell::ComputeCaretAnchoredOverlayRect(const SDL_FRect& editor_area,
                                                          const SDL_FRect& caret_anchor) const {
  constexpr float kRowStep = 22.0f;
  constexpr float kListBottomPadding = 16.0f;
  constexpr float kMaxVisibleRows = 9.0f;
  constexpr float kMargin = 4.0f;

  const int rows = std::clamp(static_cast<int>(OverlayItemCount()), 1,
                              static_cast<int>(kMaxVisibleRows));
  const float height =
      OverlayListStartOffset() + kListBottomPadding + static_cast<float>(rows) * kRowStep + 6.0f;
  const float width =
      std::min(editor_area.w - 2.0f * kMargin, std::max(260.0f, editor_area.w * 0.42f));

  float x = caret_anchor.x;
  if (x + width > editor_area.x + editor_area.w - kMargin) {
    x = editor_area.x + editor_area.w - width - kMargin;
  }
  x = std::max(x, editor_area.x + kMargin);

  float y = caret_anchor.y + caret_anchor.h + 2.0f;
  if (y + height > editor_area.y + editor_area.h - kMargin) {
    const float above_y = caret_anchor.y - height - 2.0f;
    y = above_y >= editor_area.y + kMargin
            ? above_y
            : std::max(editor_area.y + kMargin, editor_area.y + editor_area.h - height - kMargin);
  }

  return SDL_FRect{std::floor(x), std::floor(y), std::floor(width), std::floor(height)};
}

SDL_FRect WorkspaceShell::ComputeCenteredMenuOverlayRect(const SDL_FRect& editor_area) const {
  constexpr float kRowStep = 22.0f;
  constexpr float kListBottomPadding = 16.0f;
  constexpr float kMaxVisibleRows = 9.0f;
  constexpr float kVerticalFrac = 0.30f;

  const int rows = std::clamp(static_cast<int>(OverlayItemCount()), 1,
                              static_cast<int>(kMaxVisibleRows));
  const float height =
      OverlayListStartOffset() + kListBottomPadding + static_cast<float>(rows) * kRowStep + 6.0f;
  const float width =
      std::min(editor_area.w - 32.0f, std::max(320.0f, editor_area.w * 0.42f));

  const float x = editor_area.x + (editor_area.w - width) * 0.5f;
  const float y = editor_area.y + std::max(0.0f, (editor_area.h - height) * kVerticalFrac);
  return SDL_FRect{std::floor(x), std::floor(y), std::floor(width), std::floor(height)};
}

SDL_FRect WorkspaceShell::FindWidgetAnchorRect(const SDL_FRect& fallback) const {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    if (const SDL_FRect surface = ActiveTabSurface(*layout); surface.w > 0.0f && surface.h > 0.0f) {
      return surface;
    }
  }
  return fallback;
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  const OverlayState& overlay = context_.current_project_state.overlay;
  if (overlay.mode == OverlayMode::Completion && overlay.caret_anchor.has_value()) {
    return ComputeCaretAnchoredOverlayRect(editor_area, *overlay.caret_anchor);
  }
  if (overlay.mode == OverlayMode::CodeActions) {
    return ComputeCenteredMenuOverlayRect(editor_area);
  }
  if (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace) {
    return ComputeFindWidgetRect(FindWidgetAnchorRect(editor_area),
                                 overlay.mode == OverlayMode::BufferReplace);
  }
  const auto layout = CurrentWorkspaceLayout();
  return ComputeQuickOpenOverlaySurfaceRect(editor_area, layout.has_value() ? layout->full : editor_area);
}

void WorkspaceShell::RefreshStatusBar() {
  std::string_view startup_mode_text;
  std::string_view startup_mode_tooltip;
  if (startup_options_.safe_mode) {
    startup_mode_text = "Safe mode";
    startup_mode_tooltip =
        "Plugins disabled; workspace/session restore skipped at startup";
  } else if (startup_options_.disable_plugins) {
    startup_mode_text = "Plugins off";
    startup_mode_tooltip = "User-scope plugins and plugin syntax are disabled";
  }

  status_bar_model_service_.Refresh(
      status_bar_service_,
      StatusBarModelService::Operations{
          .git_availability =
              [this](const std::filesystem::path& project_root) {
                return project::GitMetadataFor(context_.current_project_state.launcher())
                    .Availability(project_root);
              },
          .read_head_branch =
              [this](const std::filesystem::path& project_root) {
                return project::ReadHeadBranchName(
                    project_root,
                    project::GitMetadataFor(context_.current_project_state.launcher()));
              },
          .active_lsp_status_strings =
              [this](bool ensure_started, std::string& text, std::string& tooltip,
                     StatusTone& tone) {
                ActiveLspStatusStrings(ensure_started, text, tooltip, tone);
              },
          .startup_mode_text = startup_mode_text,
          .startup_mode_tooltip = startup_mode_tooltip,
      },
      context_.current_project_state, StatusBarSourceViewport());
}

const editor::TextViewport* WorkspaceShell::StatusBarSourceViewport() const {
  if (const editor::TextViewport* navigable = ActiveNavigableViewport(); navigable != nullptr) {
    return navigable->is_placeholder() ? nullptr : navigable;
  }
  const editor::TextViewport* active = ActiveEditorViewport();
  return active != nullptr && !active->is_placeholder() ? active : nullptr;
}

}
