#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "workspace/GitRepositoryService.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

std::string WorkspaceShell::BreadcrumbLabel() const {
  // Resolve the label inputs (mode, path, secondary labels) by reference so a cache
  // hit compares without allocating; only a genuine input change rebuilds the string.
  static const std::filesystem::path kEmptyPath;
  int mode = 0;
  bool placeholder = false;
  const std::filesystem::path* path = &kEmptyPath;
  std::string_view left;
  std::string_view right;
  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return "compare";
    }
    mode = 1;
    path = &compare_tab->path;
    left = compare_tab->left_label;
    right = compare_tab->right_label;
  } else if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return "merge";
    }
    mode = 2;
    path = &merge_tab->output_path;
    left = merge_tab->incoming_label;
    right = merge_tab->current_label;
  } else {
    const editor::TextViewport* viewport = ActiveEditorViewport();
    mode = 0;
    if (viewport != nullptr) {
      path = &viewport->path();
      placeholder = viewport->is_placeholder();
    }
  }

  const std::filesystem::path& root = context_.current_project_state.root;
  BreadcrumbLabelCache& cache = breadcrumb_label_cache_;
  if (cache.valid && cache.mode == mode && cache.placeholder == placeholder &&
      cache.root == root && cache.path == *path && cache.left_label == left &&
      cache.right_label == right) {
    return cache.label;
  }

  std::string label;
  switch (mode) {
    case 1:
      label = BuildCompareBreadcrumbLabel(root, *path, left, right);
      break;
    case 2:
      label = BuildMergeBreadcrumbLabel(root, *path, left, right);
      break;
    default:
      label = BuildEditorBreadcrumbLabel(root, *path, placeholder);
      break;
  }
  cache.valid = true;
  cache.mode = mode;
  cache.placeholder = placeholder;
  cache.root = root;
  cache.path = *path;
  cache.left_label = left;
  cache.right_label = right;
  cache.label = label;
  return label;
}

std::string WorkspaceShell::ProjectLabel() const {
  return context_.current_project_state.root.empty() ? "microide" : ProjectLabelForRoot(context_.current_project_state.root);
}

std::string WorkspaceShell::ProjectLabelForRoot(const std::filesystem::path& root) const {
  if (root.empty()) {
    return "Welcome";
  }
  const std::string filename = root.filename().string();
  return filename.empty() ? root.lexically_normal().string() : filename;
}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  const std::string label = ProjectLabelForRoot(root);
  return DirtyEditorTabIndicesForProject(index).empty() ? label : "*" + label;
}

std::string WorkspaceShell::ProjectTabTooltipLabel(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  return root.empty() ? ProjectLabelForRoot(root) : root.lexically_normal().string();
}

SDL_FRect WorkspaceShell::ComputeCaretAnchoredOverlayRect(const SDL_FRect& editor_area,
                                                          const SDL_FRect& caret_anchor) const {
  // Compact list popup placed next to the caret rather than a centered modal. Sized to the
  // visible row count and clamped inside the editor area, flipping above the caret when there
  // is not enough room below.
  constexpr float kRowStep = 22.0f;       // must match ComputeOverlayListLayout's row_step
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
  // Compact centered menu sized to the visible row count (matches the row metrics
  // ComputeCaretAnchoredOverlayRect uses), but horizontally centered and pinned to
  // a fixed vertical fraction — the canonical modal placement, without the
  // finder-sized footprint that would dwarf a two-or-three-item action list.
  constexpr float kRowStep = 22.0f;  // must match ComputeOverlayListLayout's row_step
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
  // The find/replace widget floats over the editor text region (below the
  // breadcrumb). Fall back to the supplied rect only if the surface is unavailable.
  if (const auto layout = CurrentWorkspaceLayout();
      layout.has_value() && layout->editor_surface.w > 0.0f && layout->editor_surface.h > 0.0f) {
    return layout->editor_surface;
  }
  return fallback;
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  const OverlayState& overlay = context_.current_project_state.overlay;
  if (overlay.mode == OverlayMode::Completion && overlay.caret_anchor.has_value()) {
    return ComputeCaretAnchoredOverlayRect(editor_area, *overlay.caret_anchor);
  }
  // Code actions are a centered, content-sized menu — the canonical modal look.
  if (overlay.mode == OverlayMode::CodeActions) {
    return ComputeCenteredMenuOverlayRect(editor_area);
  }
  // Local file search is a compact non-modal widget pinned to the top-right of the
  // editor *text* area (editor_surface, below the breadcrumb) — VSCode-style, not a
  // centered modal. Anchoring to editor_surface (not editor_area) keeps it clear of
  // the breadcrumb so editor redraws fully cover it. The same helper drives the
  // renderer, field hit-test, redraw region, and reveal so they stay aligned.
  if (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace) {
    return ComputeFindWidgetRect(FindWidgetAnchorRect(editor_area),
                                 overlay.mode == OverlayMode::BufferReplace);
  }
  if (overlay.mode == OverlayMode::CommitPicker ||
      overlay.mode == OverlayMode::LaunchConfigPicker ||
      overlay.mode == OverlayMode::CommandPalette) {
    return ComputePickerOverlaySurfaceRect(editor_area);
  }
  return ComputeOverlaySurfaceRect(editor_area);
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
          .is_git_repo_valid =
              [](const std::filesystem::path& project_root) {
                return GitRepositoryService::IsGitRepoValid(project_root);
              },
          .active_lsp_status_strings =
              [this](bool ensure_started, std::string& text, std::string& tooltip,
                     StatusBarSegmentTone& tone) {
                ActiveLspStatusStrings(ensure_started, text, tooltip, tone);
              },
          .startup_mode_text = startup_mode_text,
          .startup_mode_tooltip = startup_mode_tooltip,
      },
      context_.current_project_state, ActiveEditorViewport());
}

}  // namespace microide::workspace
