#include "workspace/control/ControlUiQuery.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

namespace {

util::JsonValue RectJson(const SDL_FRect& rect) {
  util::JsonArray array;
  array.reserve(4);
  for (const float value : {rect.x, rect.y, rect.w, rect.h}) {
    // Whole pixels: every rect the layout produces is pixel-aligned, and a driver
    // clicks integers.
    array.push_back(util::JsonValue(static_cast<std::int64_t>(value < 0.0f ? value - 0.5f
                                                                           : value + 0.5f)));
  }
  return util::JsonValue(std::move(array));
}

bool HasArea(const SDL_FRect& rect) { return rect.w > 0.0f && rect.h > 0.0f; }

util::JsonValue OptionalStringJson(const std::string& text) {
  return text.empty() ? util::JsonValue(nullptr) : util::JsonValue(text);
}

}  // namespace

void AddControlUiElement(ControlUiElements& out,
                         std::string_view kind,
                         std::string_view locator,
                         const SDL_FRect& rect,
                         std::string label) {
  ControlUiElement element;
  element.name.reserve(kind.size() + 1 + locator.size());
  element.name.append(kind).append(":").append(locator);
  element.kind = kind;
  element.rect = rect;
  element.label = std::move(label);
  out.push_back(std::move(element));
}

void AddControlUiScrollbar(ControlUiElements& out,
                           std::string_view locator,
                           const ScrollbarGeometry& geometry) {
  AddControlUiElement(out, control_ui_kind::kScrollbar, locator, geometry.track,
                      geometry.vertical ? "vertical" : "horizontal");
  out.back().thumb = geometry.thumb;
}

void MakeControlUiNamesUnique(ControlUiElements& elements) {
  std::unordered_map<std::string, int> seen;
  seen.reserve(elements.size());
  for (ControlUiElement& element : elements) {
    const int count = ++seen[element.name];
    if (count > 1) {
      element.name += "#" + std::to_string(count);
    }
  }
}

std::string_view ControlUiFocusName(FocusTarget focus) {
  switch (focus) {
    case FocusTarget::Sidebar: return "sidebar";
    case FocusTarget::Editor: return "editor";
    case FocusTarget::Panel: return "panel";
    case FocusTarget::Overlay: return "overlay";
    case FocusTarget::DebugPane: return "debug-pane";
  }
  return "editor";
}

std::string_view ControlUiOverlayModeName(OverlayMode mode) {
  switch (mode) {
    case OverlayMode::FileFinder: return "file-finder";
    case OverlayMode::BufferSearch: return "buffer-search";
    case OverlayMode::BufferReplace: return "buffer-replace";
    case OverlayMode::ProjectSearch: return "project-search";
    case OverlayMode::CommitPicker: return "commit-picker";
    case OverlayMode::LaunchConfigPicker: return "launch-config-picker";
    case OverlayMode::CommandPalette: return "command-palette";
    case OverlayMode::Completion: return "completion";
    case OverlayMode::CodeActions: return "code-actions";
  }
  return "file-finder";
}

std::string_view ControlUiPanelContentName(PanelContentKind content) {
  switch (content) {
    case PanelContentKind::None: return "none";
    case PanelContentKind::Terminal: return "terminal";
    case PanelContentKind::Output: return "output";
    case PanelContentKind::PluginSurface: return "plugin-surface";
  }
  return "none";
}

util::JsonValue ControlUiQuery::Build(const WorkspaceContext& context, const Operations& operations) {
  util::JsonObject object;
  const std::optional<WorkspaceLayout> layout =
      operations.layout ? operations.layout() : std::nullopt;
  if (!layout.has_value()) {
    // No window laid out yet (or no project): an empty answer, not an error, so a
    // driver can poll for the first layout.
    object["elements"] = util::JsonValue(util::JsonArray{});
    return util::JsonValue(std::move(object));
  }

  const ProjectWorkspaceState& state = context.current_project_state;
  util::JsonObject window;
  window["width"] = util::JsonValue(static_cast<std::int64_t>(layout->full.w));
  window["height"] = util::JsonValue(static_cast<std::int64_t>(layout->full.h));
  const SDL_FPoint per_unit = operations.window_per_unit ? operations.window_per_unit() : SDL_FPoint{1.0f, 1.0f};
  util::JsonArray scale;
  scale.emplace_back(static_cast<double>(per_unit.x));
  scale.emplace_back(static_cast<double>(per_unit.y));
  window["scale"] = util::JsonValue(std::move(scale));
  object["window"] = util::JsonValue(std::move(window));
  object["focus"] = util::JsonValue(std::string(ControlUiFocusName(state.surface.focus)));

  util::JsonObject sidebar;
  sidebar["visible"] = util::JsonValue(HasArea(layout->sidebar));
  sidebar["view"] = util::JsonValue(state.sidebar.view_id);
  sidebar["temporary"] = util::JsonValue(state.sidebar.temporary);
  object["sidebar"] = util::JsonValue(std::move(sidebar));

  {
    const std::string modal = operations.modal ? operations.modal() : std::string{};
    object["modal"] = OptionalStringJson(modal);
  }
  util::JsonObject overlay;
  overlay["visible"] = util::JsonValue(state.overlay.visible);
  overlay["mode"] = state.overlay.visible
                        ? util::JsonValue(std::string(ControlUiOverlayModeName(state.overlay.mode)))
                        : util::JsonValue(nullptr);
  object["overlay"] = util::JsonValue(std::move(overlay));

  const MenuSurfaceState& menus = context.menu_state;
  const auto menu_label = [&](MenuId id) {
    return id == MenuId::None || !operations.menu_label ? std::string{} : operations.menu_label(id);
  };
  util::JsonObject menu;
  menu["open"] = util::JsonValue(menus.menu_bar_open);
  menu["menu"] = OptionalStringJson(menus.menu_bar_open ? menu_label(menus.active_menu_id) : "");
  menu["activeItem"] = util::JsonValue(static_cast<std::int64_t>(menus.active_menu_item_index));
  menu["submenu"] = OptionalStringJson(menus.menu_bar_open ? menu_label(menus.active_submenu_id) : "");
  menu["contextMenu"] = util::JsonValue(menus.tree_context_menu.open);
  menu["overflow"] = util::JsonValue(menus.overflow_popup_open);
  menu["rowsRun"] = util::JsonValue(static_cast<std::int64_t>(menus.rows_run));
  menu["lastRun"] = OptionalStringJson(
      menus.rows_run == 0 || !operations.menu_row_locator
          ? std::string{}
          : operations.menu_row_locator(menus.last_run_menu_id, menus.last_run_item_index));
  object["menu"] = util::JsonValue(std::move(menu));

  util::JsonObject panel;
  panel["visible"] = util::JsonValue(HasArea(layout->bottom_panel));
  panel["content"] = util::JsonValue(std::string(ControlUiPanelContentName(state.panel.content)));
  panel["maximized"] = util::JsonValue(state.panel.maximized);
  object["panel"] = util::JsonValue(std::move(panel));

  util::JsonObject surfaces;
  for (const auto& [key, rect] :
       {std::pair<const char*, SDL_FRect>{"menuBar", layout->menu_bar},
        {"projectTabStrip", layout->project_tab_strip},
        {"tabStrip", layout->tab_strip},
        {"breadcrumb", layout->breadcrumb},
        {"editorSurface", layout->editor_surface},
        {"sidebar", layout->sidebar},
        {"rightPane", layout->right_pane},
        {"bottomPanel", layout->bottom_panel},
        {"statusBar", layout->status_bar}}) {
    surfaces[key] = HasArea(rect) ? RectJson(rect) : util::JsonValue(nullptr);
  }
  object["surfaces"] = util::JsonValue(std::move(surfaces));

  ControlUiElements elements;
  for (const ElementSource* source : {&operations.menus, &operations.tabs, &operations.sidebar,
                                      &operations.fields, &operations.panel,
                                      &operations.status_bar, &operations.buttons,
                                      &operations.debug, &operations.overlay,
                                      &operations.dialogs}) {
    if (*source) {
      (*source)(*layout, elements);
    }
  }
  // The window's own resize handles are pure layout: the rects the divider drags
  // hit-test (the *ResizeHitRect family).
  if (HasArea(layout->sidebar)) {
    AddControlUiElement(elements, control_ui_kind::kDivider, "sidebar",
                        SidebarResizeHitRect(*layout));
  }
  if (HasArea(layout->bottom_panel)) {
    AddControlUiElement(elements, control_ui_kind::kDivider, "panel",
                        BottomPanelResizeHitRect(*layout));
  }
  if (HasArea(layout->right_pane)) {
    AddControlUiElement(elements, control_ui_kind::kDivider, "right-pane",
                        RightPaneResizeHitRect(*layout));
  }
  MakeControlUiNamesUnique(elements);

  util::JsonArray array;
  array.reserve(elements.size());
  for (const ControlUiElement& element : elements) {
    util::JsonObject entry;
    entry["name"] = util::JsonValue(element.name);
    entry["kind"] = util::JsonValue(std::string(element.kind));
    entry["rect"] = RectJson(element.rect);
    if (!element.label.empty()) {
      entry["label"] = util::JsonValue(element.label);
    }
    if (element.enabled.has_value()) {
      entry["enabled"] = util::JsonValue(*element.enabled);
    }
    if (element.active.has_value()) {
      entry["active"] = util::JsonValue(*element.active);
    }
    if (element.selected.has_value()) {
      entry["selected"] = util::JsonValue(*element.selected);
    }
    if (!element.command.empty()) {
      entry["command"] = util::JsonValue(element.command);
    }
    if (element.thumb.has_value()) {
      entry["thumb"] = RectJson(*element.thumb);
    }
    array.push_back(util::JsonValue(std::move(entry)));
  }
  object["elements"] = util::JsonValue(std::move(array));
  if (operations.details) {
    operations.details(*layout, object);
  }
  return util::JsonValue(std::move(object));
}

}  // namespace microide::workspace
