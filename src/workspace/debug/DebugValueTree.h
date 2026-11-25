#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/debug/DapProtocol.h"

namespace microide::workspace {

enum class DebugValueKind {
  Plain,
  Scope,
  Aggregate,
  Number,
  String,
  Pointer,
  Boolean,
  Pending,
  Error,
};

struct DebugVariableRowView {
  std::string display_name;
  std::string display_value;
  std::string display_type;
  DebugValueKind kind = DebugValueKind::Plain;
  int depth = 0;
  bool has_children = false;
  bool expanded = false;
  bool editable = false;
  bool is_placeholder = false;
  bool is_show_more = false;
  std::uint32_t node_id = 0;
};

class DebugValueTree {
 public:
  struct EditTarget {
    std::uint32_t node_id = 0;
    int container_reference = 0;
    std::string name;
  };

  struct ChildFetch {
    int reference = 0;
    int start = 0;
    int count = 0;
  };

  static constexpr int kChildPageSize = 200;

  static constexpr std::size_t kMaxLoadedNodes = 100000;

  static constexpr std::size_t kMaxNodeSlots = 2 * kMaxLoadedNodes;

  bool Truncated() const { return truncated_; }

  void SetDefaultExpandedScope(std::string name) { default_expanded_scope_ = std::move(name); }

  void ResetExpansionForNewSession() {
    expanded_paths_.clear();
    pending_default_expansion_ = true;
  }

  void Clear();

  void ClearRoots();

  std::uint32_t AddRoot(std::string name, std::string value, std::string type,
                        int variables_reference, bool is_scope, int total_count = 0,
                        bool total_known = false);

  std::vector<ChildFetch> RestoreExpandedRoots();

  void Rebuild();

  ChildFetch ToggleRow(std::size_t row_index);

  std::vector<ChildFetch> ApplyVariables(int variables_reference,
                                         const std::vector<dap_protocol::DapVariable>& variables,
                                         int start);

  void MarkChildrenError(int variables_reference);

  void ApplySetVariable(std::uint32_t node_id, const dap_protocol::DapSetVariableResult& result);

  void SetNodeValue(std::uint32_t node_id, std::string value, std::string type,
                    int variables_reference);

  const std::vector<DebugVariableRowView>& Rows() const { return rows_; }
  bool Empty() const { return rows_.empty(); }

  std::size_t SelectedRow() const { return selected_row_; }
  void SetSelectedRow(std::size_t row);
  void MoveSelection(int delta);

  bool BeginEdit(std::size_t row_index);
  void CancelEdit();
  bool IsEditing() const { return editing_node_.has_value(); }
  std::optional<std::uint32_t> EditingNodeId() const { return editing_node_; }
  std::optional<EditTarget> EditTargetForCommit() const;
  editor::SingleLineEditor& EditBuffer() { return edit_buffer_; }
  const editor::SingleLineEditor& EditBuffer() const { return edit_buffer_; }

 private:
  struct Node {
    std::uint32_t id = 0;
    bool live = false;
    std::string name;
    std::string value;
    std::string type;
    std::uint32_t parent_id = 0;
    std::uint32_t sibling_ordinal = 0;
    int variables_reference = 0;
    int container_reference = 0;
    bool is_scope = false;
    bool expanded = false;
    bool children_loaded = false;
    bool fetching = false;
    bool more_available = false;
    bool load_error = false;
    int loaded_count = 0;
    int total_count = 0;
    bool total_known = false;
    std::vector<std::uint32_t> children;
  };

  Node* FindNode(std::uint32_t id);
  const Node* FindNode(std::uint32_t id) const;
  void DropAllNodes();
  Node* FindNodeByReference(int variables_reference);
  std::uint32_t AddNode(Node node);
  void EraseSubtree(std::uint32_t node_id);
  std::string PathKey(const Node& node) const;
  void PathKeyInto(const Node& node, std::string& out) const;
  std::vector<ChildFetch> CollectAutoExpand(const std::vector<std::uint32_t>& node_ids);
  bool RebindReference(Node& node, int new_reference);
  void FlattenInto(std::uint32_t node_id, int depth);

  std::vector<Node> nodes_;
  std::uint32_t id_base_ = 1;
  std::size_t live_nodes_ = 0;
  std::unordered_map<int, std::uint32_t> reference_to_node_;
  std::vector<std::uint32_t> roots_;
  std::unordered_set<std::string> expanded_paths_;
  struct PathKeySegment {
    std::uint32_t ordinal = 0;
    std::string_view name;
  };
  mutable std::vector<PathKeySegment> path_key_segments_;
  mutable std::string path_key_scratch_;
  std::string default_expanded_scope_;
  bool pending_default_expansion_ = true;
  bool truncated_ = false;
  std::vector<DebugVariableRowView> rows_;
  std::uint32_t next_id_ = 1;
  std::size_t selected_row_ = 0;
  std::optional<std::uint32_t> editing_node_;
  editor::SingleLineEditor edit_buffer_;
};

}
