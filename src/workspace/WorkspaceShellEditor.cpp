#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace microide::workspace {

namespace {

constexpr float kEditorSplitDividerThickness = 6.0f;
constexpr float kMinSplitPaneExtent = 180.0f;

SDL_FRect MakeRect(float x, float y, float w, float h) {
  return SDL_FRect{x, y, w, h};
}

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "welcome" : "untitled";
}

}

void WorkspaceShell::ActivateTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (active_tab_index_ < open_tabs_.size() && active_tab_index_ != index) {
    SyncActiveEditorTab();
  }

  active_tab_index_ = index;
  auto& tab = open_tabs_[index];
  if (tab.kind == TabEntry::Kind::Editor) {
    if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
        LogMessage("Failed to restore tab: " + tab.title);
        return;
      }
      NormalizeEditorSplitTree(*tab.editor_state);
      editor::TextViewport* active_view =
          FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
      if (active_view == nullptr && !tab.editor_state->views.empty()) {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
        active_view = &tab.editor_state->views.front().viewport;
      }
      if (active_view != nullptr) {
        text_viewport_ = *active_view;
        ApplyEditorPreferences(text_viewport_);
      }
    } else {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        LogMessage("Failed to open file: " + tab.path.lexically_normal().string());
        return;
      }
      ApplyEditorPreferences(loaded_view);
      text_viewport_ = loaded_view;
      tab.editor_state = MakeEditorTabState(loaded_view);
    }
  }
  SyncActiveEditorTabMetadata();
  if (tab.kind == TabEntry::Kind::Compare) {
    RevealActiveCompareSelection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    RevealActiveMergeSelection();
  } else if (tab.kind == TabEntry::Kind::Editor && !text_viewport_.path().empty()) {
    directory_tree_.SelectPath(text_viewport_.path().lexically_normal());
  }
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
}

void WorkspaceShell::SyncActiveEditorTab() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  if (tab.editor_state->views.empty()) {
    tab.editor_state = MakeEditorTabState(text_viewport_);
    return;
  }

  NormalizeEditorSplitTree(*tab.editor_state);
  if (auto* active_view = FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
      active_view != nullptr) {
    if (active_view->needs_restore) {
      tab.path = EditorViewPath(*active_view);
      tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
      return;
    }
    active_view->viewport = text_viewport_;
    active_view->restored_path = text_viewport_.path().lexically_normal();
    active_view->restored_cursor_line = text_viewport_.cursor_line();
    active_view->restored_cursor_column = text_viewport_.cursor_column();
    active_view->restored_scroll_line = text_viewport_.scroll_line();
    active_view->restored_horizontal_scroll = text_viewport_.horizontal_scroll();
    active_view->needs_restore = false;
  }
  if (active_tab_index_ < open_tabs_.size() && &tab == &open_tabs_[active_tab_index_]) {
    SyncActiveEditorTabMetadata();
  }
}

bool WorkspaceShell::ActiveTabIsEditor() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Editor &&
         open_tabs_[active_tab_index_].editor_state.has_value();
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

WorkspaceShell::TabEntry::EditorTabState WorkspaceShell::MakeEditorTabState(
    const editor::TextViewport& view) {
  TabEntry::EditorTabState state;
  state.views.push_back(TabEntry::EditorTabState::EditorViewState{
      .leaf_id = 1,
      .viewport = view,
      .restored_path = view.path().lexically_normal(),
      .restored_cursor_line = view.cursor_line(),
      .restored_cursor_column = view.cursor_column(),
      .restored_scroll_line = view.scroll_line(),
      .restored_horizontal_scroll = view.horizontal_scroll(),
      .needs_restore = false,
  });
  state.active_leaf_id = 1;
  state.next_leaf_id = 2;
  state.split_root = MakeEditorLeafNode(1);
  return state;
}

std::unique_ptr<WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode>
WorkspaceShell::MakeEditorLeafNode(std::size_t leaf_id, float size_fraction) {
  auto leaf = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
  leaf->leaf_id = leaf_id;
  leaf->orientation = EditorSplitOrientation::None;
  leaf->size_fraction = size_fraction;
  return leaf;
}

void WorkspaceShell::SyncActiveEditorTabMetadata() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const std::filesystem::path active_path = text_viewport_.path().lexically_normal();
  const bool path_changed = tab.path != active_path;
  tab.path = active_path;
  tab.title = EditorTabLabel(text_viewport_);
  if (path_changed && !active_path.empty()) {
    directory_tree_.SelectPath(active_path);
  }
}

WorkspaceShell::TabEntry::EditorTabState::EditorViewState* WorkspaceShell::FindEditorViewState(
    TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

const WorkspaceShell::TabEntry::EditorTabState::EditorViewState*
WorkspaceShell::FindEditorViewState(const TabEntry::EditorTabState& editor_tab,
                                    std::size_t leaf_id) const {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

std::filesystem::path WorkspaceShell::EditorViewPath(
    const TabEntry::EditorTabState::EditorViewState& view) const {
  return view.needs_restore ? view.restored_path.lexically_normal()
                            : view.viewport.path().lexically_normal();
}

bool WorkspaceShell::RestoreEditorView(TabEntry::EditorTabState::EditorViewState& view) {
  if (!view.needs_restore) {
    return true;
  }
  if (view.restored_path.empty()) {
    return false;
  }

  editor::TextViewport loaded_view;
  if (!loaded_view.OpenFile(view.restored_path)) {
    return false;
  }
  loaded_view.MoveCursorTo(view.restored_cursor_line, view.restored_cursor_column);
  loaded_view.SetScrollLine(view.restored_scroll_line);
  loaded_view.SetHorizontalScroll(view.restored_horizontal_scroll);
  ApplyEditorPreferences(loaded_view);
  view.viewport = std::move(loaded_view);
  view.needs_restore = false;
  return true;
}

bool WorkspaceShell::EnsureEditorTabLoaded(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  auto& editor_state = *tab.editor_state;
  bool loaded_any = false;
  bool active_loaded = false;
  for (auto& view : editor_state.views) {
    if (!view.needs_restore) {
      loaded_any = true;
      if (view.leaf_id == editor_state.active_leaf_id) {
        active_loaded = true;
      }
      continue;
    }
    if (RestoreEditorView(view)) {
      loaded_any = true;
      if (view.leaf_id == editor_state.active_leaf_id) {
        active_loaded = true;
      }
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

  NormalizeEditorSplitTree(editor_state);
  if (const auto* active_view = FindEditorViewState(editor_state, editor_state.active_leaf_id);
      active_view != nullptr) {
    tab.path = EditorViewPath(*active_view);
    tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
  }
  return true;
}

bool WorkspaceShell::ActivateCurrentTabAfterStateLoad() {
  if (open_tabs_.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(active_tab_index_, open_tabs_.size() - 1);
  active_tab_index_ = open_tabs_.size();
  ActivateTab(active_index);
  return active_tab_index_ == active_index;
}

bool WorkspaceShell::ReplaceActiveEditorView(const editor::TextViewport& viewport) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return false;
  }

  editor::TextViewport configured_view = viewport;
  ApplyEditorPreferences(configured_view);

  NormalizeEditorSplitTree(*editor_tab);
  if (auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      active_view != nullptr) {
    *active_view = configured_view;
    text_viewport_ = configured_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
    return true;
  }
  return false;
}

editor::TextViewport* WorkspaceShell::FindEditorView(TabEntry::EditorTabState& editor_tab,
                                                     std::size_t leaf_id) {
  if (auto* view = FindEditorViewState(editor_tab, leaf_id); view != nullptr) {
    return &view->viewport;
  }
  return nullptr;
}

const editor::TextViewport* WorkspaceShell::FindEditorView(
    const TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) const {
  if (const auto* view = FindEditorViewState(editor_tab, leaf_id); view != nullptr) {
    return &view->viewport;
  }
  return nullptr;
}

WorkspaceShell::EditorSplitSlot WorkspaceShell::FindEditorLeafSlot(
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

WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode* WorkspaceShell::FindEditorSplitNode(
    TabEntry::EditorTabState::EditorSplitNode* node,
    const std::vector<std::size_t>& path) {
  auto* current = node;
  for (std::size_t index : path) {
    if (current == nullptr || index >= current->children.size()) {
      return nullptr;
    }
    current = current->children[index].get();
  }
  return current;
}

const WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode*
WorkspaceShell::FindEditorSplitNode(const TabEntry::EditorTabState::EditorSplitNode* node,
                                    const std::vector<std::size_t>& path) const {
  auto* current = node;
  for (std::size_t index : path) {
    if (current == nullptr || index >= current->children.size()) {
      return nullptr;
    }
    current = current->children[index].get();
  }
  return current;
}

void WorkspaceShell::NormalizeEditorSplitNode(TabEntry::EditorTabState::EditorSplitNode& node) {
  if (node.IsLeaf()) {
    node.orientation = EditorSplitOrientation::None;
    node.size_fraction = std::max(0.0f, node.size_fraction);
    return;
  }

  float total = 0.0f;
  for (auto& child : node.children) {
    NormalizeEditorSplitNode(*child);
    child->size_fraction = std::max(0.0f, child->size_fraction);
    total += child->size_fraction;
  }

  if (total <= 0.0f) {
    const float even_fraction = node.children.empty() ? 1.0f : 1.0f / node.children.size();
    for (auto& child : node.children) {
      child->size_fraction = even_fraction;
    }
  } else {
    for (auto& child : node.children) {
      child->size_fraction /= total;
    }
  }
}

void WorkspaceShell::NormalizeEditorSplitTree(TabEntry::EditorTabState& editor_tab) {
  if (editor_tab.views.empty()) {
    editor_tab.active_leaf_id = 0;
    editor_tab.next_leaf_id = 1;
    editor_tab.split_root.reset();
    return;
  }

  if (editor_tab.split_root == nullptr) {
    editor_tab.split_root = MakeEditorLeafNode(editor_tab.views.front().leaf_id);
  }

  while (editor_tab.split_root != nullptr && !editor_tab.split_root->IsLeaf() &&
         editor_tab.split_root->children.size() == 1) {
    editor_tab.split_root = std::move(editor_tab.split_root->children.front());
  }

  if (editor_tab.split_root != nullptr) {
    editor_tab.split_root->size_fraction = 1.0f;
    NormalizeEditorSplitNode(*editor_tab.split_root);
  }

  std::vector<std::size_t> leaf_ids = EditorLeafOrder(editor_tab);
  if (leaf_ids.empty()) {
    editor_tab.split_root = MakeEditorLeafNode(editor_tab.views.front().leaf_id);
    leaf_ids = EditorLeafOrder(editor_tab);
  }

  const auto active_it = std::find(leaf_ids.begin(), leaf_ids.end(), editor_tab.active_leaf_id);
  if (active_it == leaf_ids.end()) {
    editor_tab.active_leaf_id = leaf_ids.front();
  }

  std::size_t next_leaf_id = 1;
  for (const auto& view : editor_tab.views) {
    next_leaf_id = std::max(next_leaf_id, view.leaf_id + 1);
  }
  editor_tab.next_leaf_id = next_leaf_id;
}

void WorkspaceShell::CollectEditorLeafOrder(
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

std::vector<std::size_t> WorkspaceShell::EditorLeafOrder(
    const TabEntry::EditorTabState& editor_tab) const {
  std::vector<std::size_t> order;
  CollectEditorLeafOrder(editor_tab.split_root.get(), order);
  return order;
}

void WorkspaceShell::SetActiveEditorSplit(std::size_t index) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return;
  }

  NormalizeEditorSplitTree(*editor_tab);
  if (auto* current_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      current_view != nullptr) {
    *current_view = text_viewport_;
  }

  if (auto* target_view = FindEditorView(*editor_tab, index); target_view != nullptr) {
    editor_tab->active_leaf_id = index;
    text_viewport_ = *target_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
  }
  focus_ = FocusTarget::Editor;
}

bool WorkspaceShell::ActivateOrderedEditorSplit(std::size_t order_index) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  NormalizeEditorSplitTree(*editor_tab);
  const std::vector<std::size_t> leaf_order = EditorLeafOrder(*editor_tab);
  if (order_index >= leaf_order.size()) {
    return false;
  }

  SetActiveEditorSplit(leaf_order[order_index]);
  return true;
}

bool WorkspaceShell::SplitActiveEditor(EditorSplitOrientation orientation) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || orientation == EditorSplitOrientation::None) {
    return false;
  }

  if (editor_tab->views.empty()) {
    *editor_tab = MakeEditorTabState(text_viewport_);
  }

  NormalizeEditorSplitTree(*editor_tab);
  SyncActiveEditorTab();
  auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
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
    NormalizeEditorSplitNode(*active_slot.parent);
  } else {
    const float branch_fraction = std::max(0.0f, (*active_slot.slot)->size_fraction);
    auto group = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
    group->orientation = orientation;
    group->size_fraction = branch_fraction > 0.0f ? branch_fraction : 1.0f;
    (*active_slot.slot)->size_fraction = 0.5f;
    group->children.push_back(std::move(*active_slot.slot));
    group->children.push_back(MakeEditorLeafNode(new_leaf_id, 0.5f));
    NormalizeEditorSplitNode(*group);
    *active_slot.slot = std::move(group);
  }

  NormalizeEditorSplitTree(*editor_tab);
  editor_tab->active_leaf_id = new_leaf_id;
  if (auto* new_view = FindEditorView(*editor_tab, new_leaf_id); new_view != nullptr) {
    text_viewport_ = *new_view;
  }
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::UnsplitActiveEditor() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  SyncActiveEditorTab();
  auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
  if (active_view == nullptr) {
    return false;
  }

  const editor::TextViewport preserved_view = *active_view;
  *editor_tab = MakeEditorTabState(preserved_view);
  text_viewport_ = preserved_view;
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::CycleEditorSplit(int delta) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2 || delta == 0) {
    return false;
  }

  NormalizeEditorSplitTree(*editor_tab);
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

void WorkspaceShell::CollectEditorPaneLayouts(
    const TabEntry::EditorTabState& editor_tab,
    const TabEntry::EditorTabState::EditorSplitNode* node,
    const SDL_FRect& rect,
    std::vector<EditorPaneLayout>& panes,
    std::vector<EditorSplitDividerLayout>* dividers,
    std::vector<std::size_t>* path) const {
  if (node == nullptr) {
    return;
  }

  if (node->IsLeaf() || node->orientation == EditorSplitOrientation::None ||
      node->children.empty()) {
    panes.push_back(EditorPaneLayout{
        .leaf_id = node->leaf_id,
        .rect = rect,
        .active = node->leaf_id == editor_tab.active_leaf_id,
    });
    return;
  }

  const bool vertical = node->orientation == EditorSplitOrientation::Vertical;
  const std::size_t child_count = node->children.size();
  const float total_extent = std::max(
      0.0f, (vertical ? rect.w : rect.h) -
                kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
  std::vector<float> weights(child_count, 0.0f);
  float total_weight = 0.0f;
  for (std::size_t i = 0; i < child_count; ++i) {
    weights[i] = std::max(0.0f, node->children[i]->size_fraction);
    total_weight += weights[i];
  }
  if (total_weight <= 0.0f) {
    std::fill(weights.begin(), weights.end(), 1.0f);
    total_weight = static_cast<float>(weights.size());
  }

  float cursor = vertical ? rect.x : rect.y;
  float remaining_extent = total_extent;
  float remaining_weight = total_weight;
  for (std::size_t i = 0; i < child_count; ++i) {
    const std::size_t remaining_children = child_count - i;
    float child_extent = remaining_children == 1
                             ? remaining_extent
                             : std::floor(remaining_weight > 0.0f
                                              ? remaining_extent * (weights[i] / remaining_weight)
                                              : remaining_extent /
                                                    static_cast<float>(remaining_children));
    if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
      child_extent = std::clamp(
          child_extent, kMinSplitPaneExtent,
          remaining_extent - kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
    }

    const SDL_FRect child_rect =
        vertical ? MakeRect(cursor, rect.y, std::max(0.0f, child_extent), rect.h)
                 : MakeRect(rect.x, cursor, rect.w, std::max(0.0f, child_extent));
    if (path != nullptr) {
      path->push_back(i);
    }
    CollectEditorPaneLayouts(editor_tab, node->children[i].get(), child_rect, panes, dividers, path);
    if (path != nullptr) {
      path->pop_back();
    }

    cursor += child_extent;
    remaining_extent = std::max(0.0f, remaining_extent - child_extent);
    remaining_weight = std::max(0.0f, remaining_weight - weights[i]);

    if (i + 1 < child_count) {
      if (dividers != nullptr && path != nullptr) {
        dividers->push_back(EditorSplitDividerLayout{
            .node_path = *path,
            .divider_index = i,
            .rect = vertical ? MakeRect(cursor, rect.y, kEditorSplitDividerThickness, rect.h)
                             : MakeRect(rect.x, cursor, rect.w, kEditorSplitDividerThickness),
        });
      }
      cursor += kEditorSplitDividerThickness;
    }
  }
}

std::optional<SDL_FRect> WorkspaceShell::ComputeEditorSplitNodeRect(
    const SDL_FRect& editor_surface,
    const std::vector<std::size_t>& path) const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->split_root == nullptr) {
    return std::nullopt;
  }

  const auto compute_rect = [&](auto&& self,
                                const TabEntry::EditorTabState::EditorSplitNode* node,
                                const SDL_FRect& rect,
                                std::size_t depth) -> std::optional<SDL_FRect> {
    if (node == nullptr) {
      return std::nullopt;
    }
    if (depth >= path.size()) {
      return rect;
    }
    if (node->IsLeaf() || node->orientation == EditorSplitOrientation::None ||
        node->children.empty()) {
      return std::nullopt;
    }

    const std::size_t child_index = path[depth];
    if (child_index >= node->children.size()) {
      return std::nullopt;
    }

    const bool vertical = node->orientation == EditorSplitOrientation::Vertical;
    const std::size_t child_count = node->children.size();
    const float total_extent = std::max(
        0.0f, (vertical ? rect.w : rect.h) -
                  kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
    std::vector<float> weights(child_count, 0.0f);
    float total_weight = 0.0f;
    for (std::size_t i = 0; i < child_count; ++i) {
      weights[i] = std::max(0.0f, node->children[i]->size_fraction);
      total_weight += weights[i];
    }
    if (total_weight <= 0.0f) {
      std::fill(weights.begin(), weights.end(), 1.0f);
      total_weight = static_cast<float>(weights.size());
    }

    float cursor = vertical ? rect.x : rect.y;
    float remaining_extent = total_extent;
    float remaining_weight = total_weight;
    for (std::size_t i = 0; i < child_count; ++i) {
      const std::size_t remaining_children = child_count - i;
      float child_extent = remaining_children == 1
                               ? remaining_extent
                               : std::floor(remaining_weight > 0.0f
                                                ? remaining_extent * (weights[i] / remaining_weight)
                                                : remaining_extent /
                                                      static_cast<float>(remaining_children));
      if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
        child_extent = std::clamp(
            child_extent, kMinSplitPaneExtent,
            remaining_extent - kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
      }

      const SDL_FRect child_rect =
          vertical ? MakeRect(cursor, rect.y, std::max(0.0f, child_extent), rect.h)
                   : MakeRect(rect.x, cursor, rect.w, std::max(0.0f, child_extent));
      if (i == child_index) {
        return self(self, node->children[i].get(), child_rect, depth + 1);
      }

      cursor += child_extent + kEditorSplitDividerThickness;
      remaining_extent = std::max(0.0f, remaining_extent - child_extent);
      remaining_weight = std::max(0.0f, remaining_weight - weights[i]);
    }
    return std::nullopt;
  };

  return compute_rect(compute_rect, editor_tab->split_root.get(), editor_surface, 0);
}

std::vector<WorkspaceShell::EditorPaneLayout> WorkspaceShell::ComputeEditorPaneLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorPaneLayout> panes;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return panes;
  }

  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(), editor_surface, panes,
                           nullptr, nullptr);
  return panes;
}

std::vector<WorkspaceShell::EditorSplitDividerLayout>
WorkspaceShell::ComputeEditorSplitDividerLayouts(const SDL_FRect& editor_surface) const {
  std::vector<EditorSplitDividerLayout> dividers;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
    return dividers;
  }

  std::vector<std::size_t> path;
  std::vector<EditorPaneLayout> ignored_panes;
  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(), editor_surface, ignored_panes,
                           &dividers, &path);
  return dividers;
}

void WorkspaceShell::RequestCloseTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (TabIsDirty(index)) {
    ShowDirtyPromptForTab(index);
    return;
  }

  CloseTab(index);
}

void WorkspaceShell::CloseTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (active_tab_index_ < open_tabs_.size() && index != active_tab_index_) {
    SyncActiveEditorTab();
  }

  const std::string closed_title = open_tabs_[index].title;
  open_tabs_.erase(open_tabs_.begin() + static_cast<std::ptrdiff_t>(index));

  if (open_tabs_.empty()) {
    active_tab_index_ = 0;
    tab_scroll_index_ = 0;
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    focus_ = FocusTarget::Editor;
    LogMessage("Closed tab: " + closed_title);
    return;
  }

  if (index < active_tab_index_) {
    --active_tab_index_;
  } else if (index == active_tab_index_) {
    active_tab_index_ = std::min(index, open_tabs_.size() - 1);
    auto& tab = open_tabs_[active_tab_index_];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
        LogMessage("Failed to restore tab: " + tab.title);
      } else {
        NormalizeEditorSplitTree(*tab.editor_state);
        if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
            active_view != nullptr) {
          text_viewport_ = *active_view;
          ApplyEditorPreferences(text_viewport_);
        }
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        LogMessage("Failed to open file: " + tab.path.lexically_normal().string());
      } else {
        ApplyEditorPreferences(loaded_view);
        text_viewport_ = loaded_view;
        tab.editor_state = MakeEditorTabState(loaded_view);
      }
    }
    if (!tab.path.empty()) {
      directory_tree_.SelectPath(tab.path);
    }
    focus_ = FocusTarget::Editor;
  }

  tab_scroll_index_ =
      std::clamp(tab_scroll_index_, 0, std::max(0, static_cast<int>(open_tabs_.size()) - 1));
  EnsureActiveTabVisible();
  LogMessage("Closed tab: " + closed_title);
}

}  // namespace microide::workspace
