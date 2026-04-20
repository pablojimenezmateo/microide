#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>

namespace microide::workspace {

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
    *current_view = context_.current_project_state.text_viewport;
  }

  if (auto* target_view = FindEditorView(*editor_tab, index); target_view != nullptr) {
    editor_tab->active_leaf_id = index;
    context_.current_project_state.text_viewport = *target_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
  }
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  RequestActiveTabRedraw(!context_.current_project_state.text_viewport.path().empty());
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
    *editor_tab = MakeEditorTabState(context_.current_project_state.text_viewport);
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
    context_.current_project_state.text_viewport = *new_view;
  }
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  ResetCaretBlink();
  RequestEditorSurfaceRedraw();
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
  context_.current_project_state.text_viewport = preserved_view;
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  ResetCaretBlink();
  RequestEditorSurfaceRedraw();
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
  std::vector<float> size_fractions(node->children.size(), 0.0f);
  for (std::size_t i = 0; i < node->children.size(); ++i) {
    size_fractions[i] = node->children[i]->size_fraction;
  }
  const auto split_layout = ComputeEditorSplitAxisLayout(rect, vertical, size_fractions);
  if (!split_layout.has_value()) {
    return;
  }

  for (std::size_t i = 0; i < node->children.size(); ++i) {
    if (path != nullptr) {
      path->push_back(i);
    }
    CollectEditorPaneLayouts(editor_tab, node->children[i].get(), split_layout->child_rects[i],
                             panes, dividers, path);
    if (path != nullptr) {
      path->pop_back();
    }

    if (i < split_layout->divider_rects.size()) {
      if (dividers != nullptr && path != nullptr) {
        dividers->push_back(EditorSplitDividerLayout{
            .node_path = *path,
            .divider_index = i,
            .rect = split_layout->divider_rects[i],
        });
      }
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
    std::vector<float> size_fractions(node->children.size(), 0.0f);
    for (std::size_t i = 0; i < node->children.size(); ++i) {
      size_fractions[i] = node->children[i]->size_fraction;
    }
    const auto split_layout = ComputeEditorSplitAxisLayout(rect, vertical, size_fractions);
    if (!split_layout.has_value()) {
      return std::nullopt;
    }

    return self(self, node->children[child_index].get(), split_layout->child_rects[child_index],
                depth + 1);
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

}  // namespace microide::workspace
