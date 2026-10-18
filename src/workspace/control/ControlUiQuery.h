#pragma once

// The `ui` control query: what is on screen, by name, with the rects a click is
// tested against.
//
// A driver of the real window (tools/qa) can send keys through the control
// channel, but without this it can only click coordinates. `ui` names the menu
// bar items, the open popup's rows, the editor and panel tabs, the sidebar's view
// buttons and visible rows, the status-bar segments and the banner/notification
// buttons, each with its rect in window coordinates (the coordinates mouse events
// use), plus focus, sidebar, overlay, menu and panel state and the layout's
// surface rects. Element names are stable locators: `menu:Edit`,
// `menu-item:Edit/Undo`, `tab:0/main.cpp`, `row:tree/src`, `status:line-column`.
//
// Geometry comes from the calls the mouse paths make (the shell wires them as
// Operations), so a reported rect is the rect a click is tested against.
// Answering reads state and computes layout; it never changes state or requests a
// redraw, and needs no frame to have been painted.

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
  // `<kind>:<locator>`; made unique by MakeControlUiNamesUnique.
  std::string name;
  std::string_view kind;
  SDL_FRect rect{};
  std::string label;
  std::optional<bool> enabled;
  std::optional<bool> active;
  std::optional<bool> selected;
  // A menu row's command (the name the `ui` action receipt reports when it runs);
  // empty for elements that run none, and for submenu rows.
  std::string command;
};

using ControlUiElements = std::vector<ControlUiElement>;

// Element kinds (the prefix of every name).
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
}  // namespace control_ui_kind

// Appends an element named `<kind>:<locator>`.
void AddControlUiElement(ControlUiElements& out,
                         std::string_view kind,
                         std::string_view locator,
                         const SDL_FRect& rect,
                         std::string label = {});

// Gives the second and later elements that share a name a `#2`, `#3`, ... suffix,
// in the order they were collected (visual order), so a name resolves to one
// element.
void MakeControlUiNamesUnique(ControlUiElements& elements);

class ControlUiQuery {
 public:
  // One element source per surface; each appends that surface's elements in visual
  // order. Any may be null (that surface then reports nothing).
  using ElementSource = std::function<void(const WorkspaceLayout&, ControlUiElements&)>;

  struct Operations {
    std::function<std::optional<WorkspaceLayout>()> layout;
    // The display label of a menu (`Edit`), for the menu state block.
    std::function<std::string(MenuId)> menu_label;
    // `<menu>/<label>` of a menu's row, the locator its `menu-item:` element uses.
    std::function<std::string(MenuId, std::size_t)> menu_row_locator;
    // The modal that owns the keyboard and pointer, if any: `settings`, `help`,
    // `prompt` (a confirm/input prompt) or `unsaved-prompt`; empty for none.
    std::function<std::string()> modal;
    ElementSource menus;       // menu bar items, then the open popup's (and submenu's) rows
    ElementSource tabs;        // project tabs, each editor group's tabs, split dividers
    ElementSource sidebar;     // view buttons, header buttons, visible rows
    ElementSource panel;       // bottom-panel tabs and header buttons
    ElementSource status_bar;  // segments
    ElementSource buttons;     // editor banner, notification buttons
    ElementSource overlay;     // the open overlay's rows, query field, find-widget controls
    ElementSource dialogs;     // prompt and unsaved-prompt buttons/field, Settings rows
    ElementSource fields;      // sidebar text fields (search view, commit message) and
                               // the git view's header buttons
    ElementSource debug;       // the debug toolbar's buttons, the debug pane's mode tabs
                               // and its visible rows (call stack, variables, watch,
                               // breakpoints)
    // State no element carries: the overlay's query, match count and selection,
    // the open prompt's text, and the editor popups (hover, signature help).
    // Merged into the answer's `overlay`, and added as `prompt` and `popups`.
    std::function<void(const WorkspaceLayout&, util::JsonObject&)> details;
  };

  static util::JsonValue Build(const WorkspaceContext& context, const Operations& operations);
};

std::string_view ControlUiFocusName(FocusTarget focus);
std::string_view ControlUiOverlayModeName(OverlayMode mode);
std::string_view ControlUiPanelContentName(PanelContentKind content);

}  // namespace microide::workspace
