#include "workspace/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/Parse.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode> TabCoordinator::MakeEditorLeafNode(
    std::size_t leaf_id,
    float size_fraction) {
  auto leaf = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
  leaf->leaf_id = leaf_id;
  leaf->orientation = EditorSplitOrientation::None;
  leaf->size_fraction = size_fraction;
  return leaf;
}

TabCoordinator::EditorSplitSlot TabCoordinator::FindEditorLeafSlot(
    TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) {
  EditorSplitSlot result;
  const auto find_slot =
      [&](auto&& self,
          std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode>* slot,
          TabEntry::EditorTabState::EditorSplitNode* parent,
          std::size_t index) -> bool {
    if (slot == nullptr || slot->get() == nullptr) {
      return false;
    }

    auto* node = slot->get();
    if (node->IsLeaf()) {
      if (node->leaf_id != leaf_id) {
        return false;
      }
      result.parent = parent;
      result.index = index;
      result.slot = slot;
      return true;
    }

    for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
      if (self(self, &node->children[child_index], node, child_index)) {
        return true;
      }
    }
    return false;
  };
  find_slot(find_slot, &editor_tab.split_root, nullptr, 0);
  return result;
}

TabEntry::EditorTabState::EditorViewState* TabCoordinator::FindEditorViewState(
    TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(),
                         [&](const auto& view) { return view.leaf_id == leaf_id; });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

const TabEntry::EditorTabState::EditorViewState* TabCoordinator::FindEditorViewState(
    const TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) const {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(),
                         [&](const auto& view) { return view.leaf_id == leaf_id; });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

bool TabCoordinator::RestoreEditorView(TabEntry::EditorTabState::EditorViewState& view) {
  util::PerformanceTrace::Scope perf_scope("TabCoordinator::RestoreEditorView");
  if (!view.needs_restore) {
    return true;
  }
  if (view.restored_path.empty()) {
    return false;
  }

  editor::TextViewport loaded_view;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::RestoreEditorView::OpenFile");
    if (!loaded_view.OpenFile(view.restored_path)) {
      return false;
    }
  }
  loaded_view.MoveCursorTo(view.restored_cursor_line, view.restored_cursor_column);
  loaded_view.SetScrollLine(view.restored_scroll_line);
  loaded_view.SetHorizontalScroll(view.restored_horizontal_scroll);
  operations_.apply_editor_preferences(loaded_view);
  operations_.apply_detected_indent_on_open(loaded_view);
  view.viewport = std::move(loaded_view);
  view.needs_restore = false;
  return true;
}

void TabCoordinator::CollectEditorLeafOrder(
    const TabEntry::EditorTabState::EditorSplitNode* node,
    std::vector<std::size_t>& order) const {
  if (node == nullptr) {
    return;
  }
  if (node->IsLeaf()) {
    order.push_back(node->leaf_id);
    return;
  }
  for (const auto& child : node->children) {
    CollectEditorLeafOrder(child.get(), order);
  }
}

std::vector<std::size_t> TabCoordinator::EditorLeafOrder(
    const TabEntry::EditorTabState& editor_tab) const {
  std::vector<std::size_t> order;
  CollectEditorLeafOrder(editor_tab.split_root.get(), order);
  return order;
}

void TabCoordinator::SetActiveEditorSplit(std::size_t leaf_id) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return;
  }

  operations_.normalize_editor_split_tree(*editor_tab);
  if (auto* current_view = operations_.find_editor_view(*editor_tab, editor_tab->active_leaf_id);
      current_view != nullptr) {
    *current_view = state_.welcome_surface.viewport;
  }

  if (auto* target_view = operations_.find_editor_view(*editor_tab, leaf_id); target_view != nullptr) {
    editor_tab->active_leaf_id = leaf_id;
    state_.welcome_surface.viewport = *target_view;
    SyncActiveEditorTabMetadata();
    operations_.reset_caret_blink();
  }
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_active_tab_redraw(!state_.welcome_surface.viewport.path().empty());
}

bool TabCoordinator::ActivateOrderedEditorSplit(std::size_t order_index) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  operations_.normalize_editor_split_tree(*editor_tab);
  const std::vector<std::size_t> leaf_order = EditorLeafOrder(*editor_tab);
  if (order_index >= leaf_order.size()) {
    return false;
  }

  SetActiveEditorSplit(leaf_order[order_index]);
  return true;
}

bool TabCoordinator::SplitActiveEditor(EditorSplitOrientation orientation) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || orientation == EditorSplitOrientation::None) {
    return false;
  }

  if (editor_tab->views.empty()) {
    *editor_tab = operations_.make_editor_tab_state(state_.welcome_surface.viewport);
  }

  operations_.normalize_editor_split_tree(*editor_tab);
  SyncActiveEditorTab();
  auto* active_view = operations_.find_editor_view(*editor_tab, editor_tab->active_leaf_id);
  if (active_view == nullptr) {
    return false;
  }

  const std::size_t new_leaf_id = editor_tab->next_leaf_id++;
  editor_tab->views.push_back(TabEntry::EditorTabState::EditorViewState{
      .leaf_id = new_leaf_id,
      .viewport = *active_view,
      .restored_path = active_view->path().lexically_normal(),
      .restored_cursor_line = active_view->cursor_line(),
      .restored_cursor_column = active_view->cursor_column(),
      .restored_scroll_line = active_view->scroll_line(),
      .restored_horizontal_scroll = active_view->horizontal_scroll(),
      .needs_restore = false,
  });

  EditorSplitSlot active_slot = FindEditorLeafSlot(*editor_tab, editor_tab->active_leaf_id);
  if (active_slot.slot == nullptr || active_slot.slot->get() == nullptr) {
    editor_tab->views.pop_back();
    return false;
  }

  if (active_slot.parent != nullptr && active_slot.parent->orientation == orientation) {
    auto sibling = MakeEditorLeafNode(new_leaf_id, (*active_slot.slot)->size_fraction * 0.5f);
    (*active_slot.slot)->size_fraction *= 0.5f;
    active_slot.parent->children.insert(
        active_slot.parent->children.begin() + static_cast<std::ptrdiff_t>(active_slot.index + 1),
        std::move(sibling));
    operations_.normalize_editor_split_tree(*editor_tab);
  } else {
    const float branch_fraction = std::max(0.0f, (*active_slot.slot)->size_fraction);
    auto group = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
    group->orientation = orientation;
    group->size_fraction = branch_fraction > 0.0f ? branch_fraction : 1.0f;
    (*active_slot.slot)->size_fraction = 0.5f;
    group->children.push_back(std::move(*active_slot.slot));
    group->children.push_back(MakeEditorLeafNode(new_leaf_id, 0.5f));
    *active_slot.slot = std::move(group);
    operations_.normalize_editor_split_tree(*editor_tab);
  }

  editor_tab->active_leaf_id = new_leaf_id;
  if (auto* new_view = operations_.find_editor_view(*editor_tab, new_leaf_id); new_view != nullptr) {
    state_.welcome_surface.viewport = *new_view;
  }
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

bool TabCoordinator::UnsplitActiveEditor() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  SyncActiveEditorTab();
  auto* active_view = operations_.find_editor_view(*editor_tab, editor_tab->active_leaf_id);
  if (active_view == nullptr) {
    return false;
  }

  const editor::TextViewport preserved_view = *active_view;
  *editor_tab = operations_.make_editor_tab_state(preserved_view);
  state_.welcome_surface.viewport = preserved_view;
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

bool TabCoordinator::CycleEditorSplit(int delta) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2 || delta == 0) {
    return false;
  }

  operations_.normalize_editor_split_tree(*editor_tab);
  const std::vector<std::size_t> leaf_order = EditorLeafOrder(*editor_tab);
  if (leaf_order.size() < 2) {
    return false;
  }

  const int size = static_cast<int>(leaf_order.size());
  const auto current_it = std::find(leaf_order.begin(), leaf_order.end(), editor_tab->active_leaf_id);
  const int current =
      current_it == leaf_order.end() ? 0 : static_cast<int>(current_it - leaf_order.begin());
  const int next = (current + delta % size + size) % size;
  SetActiveEditorSplit(leaf_order[static_cast<std::size_t>(next)]);
  return true;
}

bool TabCoordinator::EnsureEditorTabLoaded(TabEntry& tab) {
  util::PerformanceTrace::Scope perf_scope("TabCoordinator::EnsureEditorTabLoaded");
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  auto& editor_state = *tab.editor_state;
  bool loaded_any = false;
  bool active_loaded = false;
  if (auto* active_view = FindEditorViewState(editor_state, editor_state.active_leaf_id);
      active_view != nullptr && active_view->needs_restore) {
    if (RestoreEditorView(*active_view)) {
      loaded_any = true;
      active_loaded = true;
    }
  }
  for (auto& view : editor_state.views) {
    if (!view.needs_restore) {
      loaded_any = true;
      if (view.leaf_id == editor_state.active_leaf_id) {
        active_loaded = true;
      }
      continue;
    }
  }

  if (!loaded_any) {
    return false;
  }

  if (!active_loaded) {
    auto loaded_it = std::find_if(editor_state.views.begin(), editor_state.views.end(),
                                  [](const auto& view) { return !view.needs_restore; });
    if (loaded_it != editor_state.views.end()) {
      editor_state.active_leaf_id = loaded_it->leaf_id;
    }
  }

  operations_.normalize_editor_split_tree(editor_state);
  const auto* active_view = FindEditorViewState(editor_state, editor_state.active_leaf_id);
  if (active_view != nullptr) {
    tab.path = operations_.editor_view_path(*active_view);
    tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
  }
  return true;
}

std::optional<std::size_t> TabCoordinator::FindIndexBySpecifier(std::string_view specifier,
                                                                std::string* error_message) const {
  if (specifier.empty()) {
    if (error_message != nullptr) {
      *error_message = "usage: tabswitch <tab>";
    }
    return std::nullopt;
  }

  const std::string lowered_specifier = util::ToLowerAscii(specifier);
  if (const auto tab_number = util::ParseInt(specifier); tab_number.has_value()) {
    if (*tab_number >= 1 && static_cast<std::size_t>(*tab_number) <= state_.open_tabs.size()) {
      return static_cast<std::size_t>(*tab_number - 1);
    }
    if (error_message != nullptr) {
      *error_message = "Invalid tab index";
    }
    return std::nullopt;
  }

  std::vector<std::size_t> exact_matches;
  std::vector<std::size_t> partial_matches;
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    const TabEntry& tab = state_.open_tabs[i];
    const std::string lowered_title = util::ToLowerAscii(tab.title);
    const std::string lowered_path = util::ToLowerAscii(RelativePathLabel(state_.root, tab.path));
    const std::string lowered_absolute_path = util::ToLowerAscii(tab.path.lexically_normal().string());
    const bool exact_match = lowered_title == lowered_specifier ||
                             (!lowered_path.empty() && lowered_path == lowered_specifier) ||
                             (!lowered_absolute_path.empty() &&
                              lowered_absolute_path == lowered_specifier);
    const bool partial_match = lowered_title.find(lowered_specifier) != std::string::npos ||
                               (!lowered_path.empty() &&
                                lowered_path.find(lowered_specifier) != std::string::npos) ||
                               (!lowered_absolute_path.empty() &&
                                lowered_absolute_path.find(lowered_specifier) != std::string::npos);
    if (exact_match) {
      exact_matches.push_back(i);
    } else if (partial_match) {
      partial_matches.push_back(i);
    }
  }

  if (exact_matches.size() == 1) {
    return exact_matches.front();
  }
  if (exact_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (error_message != nullptr) {
    *error_message = "Unknown tab: " + std::string(specifier);
  }
  return std::nullopt;
}

bool TabCoordinator::ReopenActive() {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return false;
  }

  auto& tab = state_.open_tabs[state_.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return false;
  }
  const std::filesystem::path reopen_path = state_.welcome_surface.viewport.path().empty()
                                                ? tab.path.lexically_normal()
                                                : state_.welcome_surface.viewport.path().lexically_normal();
  if (reopen_path.empty() || state_.welcome_surface.viewport.dirty()) {
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(reopen_path)) {
    return false;
  }
  operations_.apply_editor_preferences(reopened_view);
  operations_.apply_detected_indent_on_open(reopened_view);

  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    operations_.normalize_editor_split_tree(*tab.editor_state);
    for (auto& view : tab.editor_state->views) {
      if (view.leaf_id == tab.editor_state->active_leaf_id ||
          operations_.editor_view_path(view) == reopen_path) {
        view.viewport = reopened_view;
        view.restored_path = reopen_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
      }
    }
    tab.editor_state->folding_model.Clear();
    state_.welcome_surface.viewport = reopened_view;
  } else {
    state_.welcome_surface.viewport = reopened_view;
    tab.editor_state = operations_.make_editor_tab_state(reopened_view);
  }
  SyncActiveEditorTabMetadata();
  operations_.invalidate_editor_blame_path(reopen_path);
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

}  // namespace microide::workspace
