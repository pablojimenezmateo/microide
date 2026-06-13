#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>

#include "workspace/EditorTabService.h"

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
  MakeEditorTabService().SetActiveEditorSplit(index);
}

bool WorkspaceShell::ActivateOrderedEditorSplit(std::size_t order_index) {
  return MakeEditorTabService().ActivateOrderedEditorSplit(order_index);
}

bool WorkspaceShell::SplitActiveEditor(EditorSplitOrientation orientation) {
  return MakeEditorTabService().SplitActiveEditor(orientation);
}

bool WorkspaceShell::UnsplitActiveEditor() {
  return MakeEditorTabService().UnsplitActiveEditor();
}

bool WorkspaceShell::CycleEditorSplit(int delta) {
  return MakeEditorTabService().CycleEditorSplit(delta);
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

SDL_FRect WorkspaceShell::EditorSurfaceBelowBanner(const SDL_FRect& editor_surface) const {
  if (ActiveEditorBannerForTab(context_.current_project_state) == nullptr) {
    return editor_surface;
  }
  const SDL_FRect strip = ComputeEditorBannerStripRect(editor_surface);
  SDL_FRect content = editor_surface;
  content.y += strip.h;
  content.h = std::max(0.0f, content.h - strip.h);
  return content;
}

std::vector<WorkspaceShell::EditorPaneLayout> WorkspaceShell::ComputeEditorPaneLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorPaneLayout> panes;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return panes;
  }

  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(),
                           EditorSurfaceBelowBanner(editor_surface), panes, nullptr, nullptr);
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
  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(),
                           EditorSurfaceBelowBanner(editor_surface), ignored_panes, &dividers,
                           &path);
  return dividers;
}

}  // namespace microide::workspace
