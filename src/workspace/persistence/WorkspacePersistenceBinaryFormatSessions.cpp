#include "workspace/persistence/WorkspacePersistenceBinaryInternal.h"

#include "util/PathMatch.h"
#include "workspace/TerminalPaneLayout.h"

namespace microide::workspace {

namespace {

// Cap the number of expanded/collapsed tree paths a single decoded session may
// carry. A real project tree never approaches this; the bound stops a hostile or
// corrupt session file from allocating an unbounded string vector at restore.
constexpr std::size_t kMaxSessionTreePaths = 100000;

std::string SerializeOutgoingBaseChoiceKind(OutgoingBaseChoice::Kind kind) {
  switch (kind) {
    case OutgoingBaseChoice::Kind::Auto:
      return "auto";
    case OutgoingBaseChoice::Kind::PreviousCommit:
      return "previous_commit";
    case OutgoingBaseChoice::Kind::SpecificRef:
      return "specific_ref";
  }
  return "auto";
}

OutgoingBaseChoice::Kind ParseOutgoingBaseChoiceKind(std::string_view value) {
  if (value == "previous_commit") {
    return OutgoingBaseChoice::Kind::PreviousCommit;
  }
  if (value == "specific_ref") {
    return OutgoingBaseChoice::Kind::SpecificRef;
  }
  return OutgoingBaseChoice::Kind::Auto;
}

// The split tree, pre-order: one entry per node as (orientation, child count,
// child weights). Children follow their parent, so the shape needs no explicit
// links — see EditorSplitTree::Flatten / ::Load, which own the validation.
bool WriteSplitTree(PrimitiveWriter& w, const EditorSplitTreeRecord& record) {
  if (!w.WriteU8(static_cast<std::uint8_t>(record.size()))) {
    return false;
  }
  for (const EditorSplitNodeRecord& node : record) {
    if (!w.WriteU8(static_cast<std::uint8_t>(node.orientation)) ||
        !w.WriteU8(static_cast<std::uint8_t>(node.weights.size()))) {
      return false;
    }
    for (const float weight : node.weights) {
      if (!w.WriteF32(weight)) {
        return false;
      }
    }
  }
  return true;
}

bool ReadSplitTree(PrimitiveReader& reader, EditorSplitTreeRecord* out) {
  std::uint8_t node_count = 0;
  if (!reader.ReadU8(&node_count) || node_count > kMaxEditorSplitNodes) {
    return false;
  }
  for (std::uint8_t i = 0; i < node_count; ++i) {
    std::uint8_t orientation = 0;
    std::uint8_t weight_count = 0;
    if (!reader.ReadU8(&orientation) || !reader.ReadU8(&weight_count) ||
        orientation > static_cast<std::uint8_t>(EditorSplitOrientation::Horizontal) ||
        weight_count > kMaxEditorGroups) {
      return false;
    }
    EditorSplitNodeRecord node;
    node.orientation = static_cast<EditorSplitOrientation>(orientation);
    for (std::uint8_t w = 0; w < weight_count; ++w) {
      float weight = 0.0f;
      if (!reader.ReadF32(&weight)) {
        return false;
      }
      node.weights.push_back(weight);
    }
    out->push_back(node);
  }
  return true;
}

bool EncodeTerminalTab(const TerminalTabRecord& tab, std::vector<std::byte>* out) {
  out->clear();
  if (!AppendRecord(TerminalTabTag::ActivePane,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.active_pane); }, out)) {
    return false;
  }
  for (const TerminalPaneRecord& pane : tab.panes) {
    std::vector<std::byte> payload;
    if (!AppendRecord(TerminalPaneTag::Cwd, [&](PrimitiveWriter& w) { return w.WritePath(pane.cwd); },
                      &payload) ||
        !AppendRecord(TerminalPaneTag::Command,
                      [&](PrimitiveWriter& w) { return w.WriteString(pane.command); }, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(TerminalTabTag::Pane), payload, out)) {
      return false;
    }
  }
  for (const float weight : tab.weights) {
    if (!AppendRecord(TerminalTabTag::Weight, [&](PrimitiveWriter& w) { return w.WriteF32(weight); },
                      out)) {
      return false;
    }
  }
  return true;
}

// Panes past kMaxTerminalPanes are dropped (with their weights, which then no
// longer match and restore even) rather than failing the session.
bool DecodeTerminalTab(std::span<const std::byte> input, TerminalTabRecord* tab) {
  return ParseRecordStream<TerminalTabTag>(
      input, [&](TerminalTabTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case TerminalTabTag::ActivePane:
            return ReadSize(reader, &tab->active_pane) && reader.remaining() == 0;
          case TerminalTabTag::Pane: {
            if (tab->panes.size() >= kMaxTerminalPanes) {
              return true;
            }
            TerminalPaneRecord pane;
            if (!ParseRecordStream<TerminalPaneTag>(
                    payload, [&](TerminalPaneTag pane_tag, std::span<const std::byte> field) {
                      PrimitiveReader field_reader(field);
                      switch (pane_tag) {
                        case TerminalPaneTag::Cwd:
                          return field_reader.ReadPath(&pane.cwd) && field_reader.remaining() == 0;
                        case TerminalPaneTag::Command:
                          return field_reader.ReadString(&pane.command) &&
                                 field_reader.remaining() == 0;
                      }
                      return true;
                    })) {
              return false;
            }
            tab->panes.push_back(std::move(pane));
            return true;
          }
          case TerminalTabTag::Weight: {
            float weight = 0.0f;
            if (!reader.ReadF32(&weight) || reader.remaining() != 0) {
              return false;
            }
            if (tab->weights.size() < kMaxTerminalPanes) {
              tab->weights.push_back(weight);
            }
            return true;
          }
        }
        return true;
      });
}

}  // namespace

bool EncodeProjectSessionRecord(const PersistedProjectSessionState& state,
                                std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(ProjectSessionTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kProjectSessionSchemaVersion); }, out) ||
      !AppendRecord(ProjectSessionTag::SidebarVisible,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.sidebar_visible); }, out) ||
      !AppendRecord(ProjectSessionTag::SidebarWidth,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.sidebar_width); }, out) ||
      !AppendRecord(ProjectSessionTag::BottomPanelHeight,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.bottom_panel_height); }, out) ||
      !AppendRecord(ProjectSessionTag::OutgoingBaseKind,
                    [&](PrimitiveWriter& w) {
                      return w.WriteString(
                          SerializeOutgoingBaseChoiceKind(state.outgoing_base_choice.kind));
                    },
                    out) ||
      !AppendRecord(ProjectSessionTag::OutgoingBaseCustomRef,
                    [&](PrimitiveWriter& w) {
                      return w.WriteString(state.outgoing_base_choice.custom_ref);
                    },
                    out) ||
      !AppendRecord(ProjectSessionTag::FocusedGroupIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.focused_group_index); }, out) ||
      !AppendRecord(ProjectSessionTag::SplitTree,
                    [&](PrimitiveWriter& w) { return WriteSplitTree(w, state.split_tree); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneVisible,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.right_pane_visible); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneWidth,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.right_pane_width); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneMode,
                    [&](PrimitiveWriter& w) { return w.WriteU8(state.right_pane_mode); }, out) ||
      !AppendRecord(ProjectSessionTag::SelectedTreePath,
                    [&](PrimitiveWriter& w) { return w.WriteString(state.selected_tree_path); },
                    out) ||
      !AppendRecord(ProjectSessionTag::SidebarScrollRow,
                    [&](PrimitiveWriter& w) {
                      return w.WriteU32(static_cast<std::uint32_t>(std::max(0, state.sidebar_scroll_row)));
                    },
                    out) ||
      !AppendRecord(ProjectSessionTag::SidebarViewId,
                    [&](PrimitiveWriter& w) { return w.WriteString(state.sidebar_view_id); }, out) ||
      !AppendRecord(ProjectSessionTag::PanelMaximized,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.panel_maximized); }, out) ||
      !AppendRecord(ProjectSessionTag::ActiveTerminalTab,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.terminal_layout.active_tab); },
                    out)) {
    return false;
  }
  for (const TerminalTabRecord& tab : state.terminal_layout.tabs) {
    std::vector<std::byte> payload;
    if (!EncodeTerminalTab(tab, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectSessionTag::TerminalTab), payload, out)) {
      return false;
    }
  }

  for (const auto& path : state.expanded_tree_paths) {
    if (!AppendRecord(ProjectSessionTag::ExpandedTreePath,
                      [&](PrimitiveWriter& w) { return w.WriteString(path); }, out)) {
      return false;
    }
  }
  for (const auto& path : state.collapsed_tree_paths) {
    if (!AppendRecord(ProjectSessionTag::CollapsedTreePath,
                      [&](PrimitiveWriter& w) { return w.WriteString(path); }, out)) {
      return false;
    }
  }

  for (const auto& group : state.groups) {
    std::vector<std::byte> payload;
    if (!EncodeEditorGroup(group, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectSessionTag::Group), payload, out)) {
      return false;
    }
  }

  return true;
}

bool DecodeProjectSessionRecord(std::span<const std::byte> input,
                                PersistedProjectSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedProjectSessionState{};
  bool seen_schema = false;
  return ParseRecordStream<ProjectSessionTag>(
             input, [&](ProjectSessionTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case ProjectSessionTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 ||
                       schema != kProjectSessionSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case ProjectSessionTag::SidebarVisible:
                   return reader.ReadBool(&state->sidebar_visible) && reader.remaining() == 0;
                 case ProjectSessionTag::SidebarWidth:
                   return reader.ReadF32(&state->sidebar_width) && reader.remaining() == 0;
                 case ProjectSessionTag::BottomPanelHeight:
                   return reader.ReadF32(&state->bottom_panel_height) && reader.remaining() == 0;
                 case ProjectSessionTag::OutgoingBaseKind: {
                   std::string value;
                   if (!reader.ReadString(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   state->outgoing_base_choice.kind = ParseOutgoingBaseChoiceKind(value);
                   return true;
                 }
                 case ProjectSessionTag::OutgoingBaseCustomRef:
                   return reader.ReadString(&state->outgoing_base_choice.custom_ref) &&
                          reader.remaining() == 0;
                 case ProjectSessionTag::FocusedGroupIndex:
                   return ReadSize(reader, &state->focused_group_index) && reader.remaining() == 0;
                 case ProjectSessionTag::SplitTree:
                   return ReadSplitTree(reader, &state->split_tree) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneVisible:
                   return reader.ReadBool(&state->right_pane_visible) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneWidth:
                   return reader.ReadF32(&state->right_pane_width) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneMode:
                   return reader.ReadU8(&state->right_pane_mode) && reader.remaining() == 0;
                 case ProjectSessionTag::ExpandedTreePath: {
                   std::string value;
                   if (!reader.ReadString(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   // Drop paths past the cap rather than failing the whole
                   // record: a corrupt session still restores its other state.
                   if (state->expanded_tree_paths.size() < kMaxSessionTreePaths) {
                     state->expanded_tree_paths.push_back(std::move(value));
                   }
                   return true;
                 }
                 case ProjectSessionTag::CollapsedTreePath: {
                   std::string value;
                   if (!reader.ReadString(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   if (state->collapsed_tree_paths.size() < kMaxSessionTreePaths) {
                     state->collapsed_tree_paths.push_back(std::move(value));
                   }
                   return true;
                 }
                 case ProjectSessionTag::SelectedTreePath:
                   return reader.ReadString(&state->selected_tree_path) && reader.remaining() == 0;
                 case ProjectSessionTag::SidebarScrollRow: {
                   std::uint32_t value = 0;
                   if (!reader.ReadU32(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   // Encode clamps to >= 0 then writes u32; a forged 0xFFFFFFFF
                   // must not decode to a negative row. Cap at INT_MAX.
                   state->sidebar_scroll_row = static_cast<int>(
                       std::min(value, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
                   return true;
                 }
                 case ProjectSessionTag::SidebarViewId:
                   return reader.ReadString(&state->sidebar_view_id) && reader.remaining() == 0;
                 case ProjectSessionTag::PanelMaximized:
                   return reader.ReadBool(&state->panel_maximized) && reader.remaining() == 0;
                 case ProjectSessionTag::ActiveTerminalTab:
                   return ReadSize(reader, &state->terminal_layout.active_tab) &&
                          reader.remaining() == 0;
                 case ProjectSessionTag::TerminalTab: {
                   // Checked before decoding, like Group: an over-cap tab is skipped
                   // whole, never parsed.
                   if (state->terminal_layout.tabs.size() >= kMaxRevivedTerminalTabs) {
                     return true;
                   }
                   TerminalTabRecord tab;
                   if (!DecodeTerminalTab(payload, &tab)) {
                     return false;
                   }
                   if (!tab.panes.empty()) {
                     state->terminal_layout.tabs.push_back(std::move(tab));
                   }
                   return true;
                 }
                 case ProjectSessionTag::Group: {
                   // Check the group cap BEFORE decoding the payload. DecodeEditorGroup can
                   // parse up to 4096 tabs, each carrying a dirty-buffer snapshot up to the
                   // session byte budget; a forged session could list oversized third-and-
                   // later groups purely to make startup materialize nested payloads that are
                   // then discarded. Skip over-cap groups without decoding their nested tabs
                   // (TD-2026-07-17A-059).
                   if (state->groups.size() >= kMaxEditorGroups) {
                     return true;  // tolerate extra groups; keep only what fits
                   }
                   PersistedEditorGroupState group;
                   if (!DecodeEditorGroup(payload, &group)) {
                     return false;
                   }
                   state->groups.push_back(std::move(group));
                   return true;
                 }
                case ProjectSessionTag::ChatRegistry:
                  // Legacy AI/chat records are tolerated during decode and ignored.
                  return true;
               }
               return true;
             }) &&
         seen_schema;
}

bool EncodeWorkspaceSessionRecord(const PersistedWorkspaceSessionState& state,
                                  std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(WorkspaceSessionTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out) ||
      !AppendRecord(WorkspaceSessionTag::ActiveProjectIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.active_project_index); },
                    out)) {
    return false;
  }
  for (const auto& root : state.project_roots) {
    if (!AppendRecord(WorkspaceSessionTag::ProjectRoot,
                      [&](PrimitiveWriter& w) { return w.WritePath(root); }, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeWorkspaceSessionRecord(std::span<const std::byte> input,
                                  PersistedWorkspaceSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedWorkspaceSessionState{};
  bool seen_schema = false;
  return ParseRecordStream<WorkspaceSessionTag>(
             input, [&](WorkspaceSessionTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case WorkspaceSessionTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case WorkspaceSessionTag::ProjectRoot: {
                   std::filesystem::path root;
                   if (!reader.ReadPath(&root) || reader.remaining() != 0) {
                     return false;
                   }
                   // Cap and dedupe: a corrupt session with thousands of (or
                   // duplicate) roots would otherwise spin up a large startup loop
                   // and duplicate project tabs. 256 is far past any real workspace.
                   constexpr std::size_t kMaxWorkspaceProjectRoots = 256;
                   const std::filesystem::path normalized = root.lexically_normal();
                   const bool already_present =
                       std::any_of(state->project_roots.begin(), state->project_roots.end(),
                                   [&](const std::filesystem::path& existing) {
                                     return util::SameAsNormalizedPath(existing, normalized);
                                   });
                   if (!already_present &&
                       state->project_roots.size() < kMaxWorkspaceProjectRoots) {
                     state->project_roots.push_back(std::move(root));
                   }
                   return true;
                 }
                 case WorkspaceSessionTag::ActiveProjectIndex:
                   return ReadSize(reader, &state->active_project_index) && reader.remaining() == 0;
               }
               return true;
             }) &&
         seen_schema;
}

}  // namespace microide::workspace
