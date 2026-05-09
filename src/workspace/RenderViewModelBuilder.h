#pragma once

#include "editor/EditorViewModel.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceTabState.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

struct FrameSurfaceViewModel {
  struct CompareSurfaceViewModel {
    TabEntry::Kind kind = TabEntry::Kind::Editor;
  };

  WorkspaceLayout layout{};
  bool sidebar_visible = false;
  bool bottom_panel_visible = false;
  std::optional<CompareSurfaceViewModel> compare_surface;
  ProjectWorkspaceState* project_state = nullptr;
};

struct OverlaySurfaceViewModel {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  int scroll_row = 0;
  TextInputSurface current_surface = TextInputSurface::None;
  std::string buffer_search_query_text;
  const OverlayState* state = nullptr;
  ProjectWorkspaceState* project_state = nullptr;
};

struct TextInputSurfaceViewModel {
  TextInputSurface current_surface = TextInputSurface::None;
  bool prompt_editing = false;
  bool command_mode = false;
  const editor::SingleLineEditor* command_input = nullptr;
  const editor::SingleLineEditor* prompt_input = nullptr;
  const editor::SingleLineEditor* buffer_search_query = nullptr;
  const editor::SingleLineEditor* buffer_search_replace = nullptr;
  const editor::SingleLineEditor* project_search_query = nullptr;
  const editor::SingleLineEditor* project_search_edit_buffer = nullptr;
  const editor::SingleLineEditor* commit_picker_query = nullptr;
  const editor::SingleLineEditor* file_finder_query = nullptr;
  const editor::TextViewport* chat_composer = nullptr;
};

struct SidebarSurfaceViewModel {
  bool visible = false;
  SidebarMode mode = SidebarMode::Tree;
  int scroll_row = 0;
  bool project_search_editing = false;
  std::string query_fallback_text;
  std::string replace_fallback_text;
  ProjectWorkspaceState* project_state = nullptr;
};

struct BottomPanelSurfaceViewModel {
  bool command_mode = false;
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
  std::string output_channel_id;
  std::filesystem::path project_root;
  FocusTarget focus = FocusTarget::Sidebar;
  const CommandState* command_state = nullptr;
};

struct HoverPopupViewModel {
  bool visible = false;
  bool has_active_target = false;
};

struct HoverTargetsViewModel {
  bool hover_enabled = false;
  const editor::DiagnosticsStore* diagnostics_store = nullptr;
};

struct StatusBarSegmentViewModel {
  StatusBarSegmentId id = StatusBarSegmentId::Project;
  std::string_view text;
  bool clickable = false;
};

struct StatusBarViewModel {
  bool visible = false;
  SDL_FRect rect{};
  LayoutMode layout_mode = LayoutMode::Regular;
  std::vector<StatusBarSegmentViewModel> left_segments;
  std::vector<StatusBarSegmentViewModel> right_segments;
};

struct SettingsOverlayViewModel {
  bool visible = false;
  SettingsOverlayMode mode = SettingsOverlayMode::Settings;
  SDL_FRect rect{};
  int scroll_row = 0;
  std::string title;
  std::string query;
  std::vector<SettingsOverlayRow> settings_rows;
  std::vector<HelpAboutRow> help_rows;
};

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  OverlaySurfaceViewModel BuildOverlaySurface() const;
  TextInputSurfaceViewModel BuildTextInputSurface() const;
  SidebarSurfaceViewModel BuildSidebarSurface() const;
  editor::EditorViewModel BuildEditorViewModel(const editor::TextViewport& viewport,
                                               std::size_t visible_rows,
                                               const editor::FoldingModel* folding_model) const;
  BottomPanelSurfaceViewModel BuildBottomPanelSurface() const;
  HoverPopupViewModel BuildHoverPopup(bool has_active_target) const;
  HoverTargetsViewModel BuildHoverTargets() const;
  StatusBarViewModel BuildStatusBar(const WorkspaceLayout& layout,
                                    const class StatusBarService& service) const;
  SettingsOverlayViewModel BuildSettingsOverlay(
      const WorkspaceLayout& layout,
      const class SettingsOverlayService& service) const;

 private:
  const WorkspaceContext& context_;
};

}  // namespace microide::workspace
