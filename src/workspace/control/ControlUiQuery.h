#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct WorkspaceContext;

struct ControlUiElement {
  std::string name;
  std::string_view kind;
  SDL_FRect rect{};
  std::string label;
  std::optional<bool> enabled;
  std::optional<bool> active;
  std::optional<bool> selected;
  std::string command;
  std::string menu;
  std::optional<SDL_FRect> thumb;
};

using ControlUiElements = std::vector<ControlUiElement>;

namespace control_ui_kind {
inline constexpr std::string_view kMenu = "menu";
inline constexpr std::string_view kMenuItem = "menu-item";
inline constexpr std::string_view kProjectTab = "project-tab";
inline constexpr std::string_view kTab = "tab";
inline constexpr std::string_view kSidebarView = "sidebar-view";
inline constexpr std::string_view kRow = "row";
inline constexpr std::string_view kPanelTab = "panel-tab";
inline constexpr std::string_view kStatus = "status";
inline constexpr std::string_view kButton = "button";
inline constexpr std::string_view kDivider = "divider";
inline constexpr std::string_view kOverlayRow = "overlay-row";
inline constexpr std::string_view kField = "field";
inline constexpr std::string_view kSetting = "setting";
inline constexpr std::string_view kSettingsCategory = "settings-category";
inline constexpr std::string_view kDebugTab = "debug-tab";
inline constexpr std::string_view kDebugRow = "debug-row";
inline constexpr std::string_view kNotification = "notification";
inline constexpr std::string_view kScrollbar = "scrollbar";
}

void AddControlUiElement(ControlUiElements& out,
                         std::string_view kind,
                         std::string_view locator,
                         const SDL_FRect& rect,
                         std::string label = {});

void AddControlUiScrollbar(ControlUiElements& out,
                           std::string_view locator,
                           const ScrollbarGeometry& geometry);

void MakeControlUiNamesUnique(ControlUiElements& elements);

class ControlUiQuery {
 public:
  using ElementSource = std::function<void(const WorkspaceLayout&, ControlUiElements&)>;

  struct Operations {
    std::function<std::optional<WorkspaceLayout>()> layout;
    std::function<SDL_FPoint()> window_per_unit;
    std::function<std::string_view(float, float)> cursor_at;
    std::function<std::string_view()> pointer_cursor;
    std::function<std::string(MenuId)> menu_label;
    std::function<std::string(MenuId, std::size_t)> menu_row_locator;
    std::function<std::string()> modal;
    ElementSource menus;
    ElementSource tabs;
    ElementSource sidebar;
    ElementSource panel;
    ElementSource status_bar;
    ElementSource buttons;
    ElementSource overlay;
    ElementSource dialogs;
    ElementSource fields;
    ElementSource debug;
    std::function<void(const WorkspaceLayout&, util::JsonObject&)> details;
    std::function<util::JsonValue()> action_counts;
  };

  static util::JsonValue Build(const WorkspaceContext& context, const Operations& operations);
};

std::string_view ControlUiFocusName(FocusTarget focus);
std::string_view ControlUiOverlayModeName(OverlayMode mode);
std::string_view ControlUiPanelContentName(PanelContentKind content);

}
