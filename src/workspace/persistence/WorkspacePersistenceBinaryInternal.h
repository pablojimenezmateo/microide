#pragma once

#include "workspace/persistence/WorkspacePersistenceFormat.h"

#include "compare/BranchReviewStateService.h"
#include "persistence/PersistedRecord.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace microide::workspace {
namespace {

using persistence::AppendTaggedRecord;
using persistence::PrimitiveReader;
using persistence::PrimitiveWriter;
using persistence::ReadTaggedRecord;
using persistence::TaggedRecordView;

constexpr std::uint32_t kSchemaVersion = 1;

// The project-session record carries editor groups (splits above tabs). This
// shape diverged from the original single-tab-list layout, so the session record
// is versioned independently of the other records: an older session file is
// cleanly rejected on decode (the workspace then starts with an empty session)
// rather than being misread against the new tag scheme.
constexpr std::uint32_t kProjectSessionSchemaVersion = 2;

enum class UserConfigTag : std::uint16_t {
  Schema = 1,
  UiScale = 2,
  Setting = 3,
  DisabledKeybinding = 4,
  DisabledPlugin = 5,
};

enum class ProjectConfigTag : std::uint16_t {
  Schema = 1,
  // Tags 2-4 (EditorTabSize/EditorIndentWidth/EditorSoftTabs) are retired: the
  // canonical editor preferences now round-trip solely through the layered
  // `Setting` records (tag 7). Old files carrying tags 2-4 are skipped on decode.
  ColorschemeName = 5,
  ProjectBaseColor = 6,
  Setting = 7,
  SidebarPolicy = 8,
  CommitDraft = 9,
  BranchReviewState = 10,
};

enum class BranchReviewTargetTag : std::uint16_t {
  RepositoryRoot = 1,
  BaseCommit = 2,
  HeadCommit = 3,
  MergeBaseCommit = 4,
  SnapshotGeneration = 5,
  LastAccessedUnixMs = 6,
  ReviewedFile = 7,
  ReviewedHunk = 8,
  Note = 9,
};

enum class BranchReviewHunkIdentityTag : std::uint16_t {
  Path = 1,
  OldStart = 2,
  OldCount = 3,
  NewStart = 4,
  NewCount = 5,
  ContentHash = 6,
};

enum class BranchReviewFileEntryTag : std::uint16_t {
  Path = 1,
  ReviewedSnapshotGeneration = 2,
  ReviewedAtUnixMs = 3,
};

enum class BranchReviewHunkEntryTag : std::uint16_t {
  Identity = 1,
  ReviewedAtUnixMs = 2,
};

enum class BranchReviewNoteTag : std::uint16_t {
  Scope = 1,
  Path = 2,
  HunkIdentity = 3,
  Text = 4,
  UpdatedAtUnixMs = 5,
};

enum class BranchReviewStateTag : std::uint16_t {
  Target = 1,
};

enum class CommitDraftTag : std::uint16_t {
  HeadOid = 1,
  BranchName = 2,
  Subject = 3,
  Body = 4,
};

enum class ProjectSessionTag : std::uint16_t {
  Schema = 1,
  SidebarVisible = 2,
  SidebarWidth = 3,
  BottomPanelHeight = 4,
  FocusedGroupIndex = 5,
  Group = 6,
  ChatRegistry = 7,
  OutgoingBaseKind = 8,
  OutgoingBaseCustomRef = 9,
  RightPaneVisible = 10,
  RightPaneWidth = 11,
  RightPaneMode = 12,
  // 13/14 were the single split's orientation + fraction, replaced by SplitTree.
  SplitTree = 20,
  ExpandedTreePath = 15,    // repeated; one relative path per record
  CollapsedTreePath = 16,   // repeated; one relative path per record
  SelectedTreePath = 17,
  SidebarScrollRow = 18,
  SidebarViewId = 19,
};

enum class EditorGroupTag : std::uint16_t {
  ActiveTabIndex = 1,
  Tab = 2,
};

enum class EditorTabTag : std::uint16_t {
  Kind = 1,
  // Inline single-view editor fields (formerly nested EditorView records).
  Path = 2,
  CursorLine = 3,
  CursorColumn = 4,
  ScrollLine = 26,
  HorizontalScroll = 27,
  DirtySnapshot = 28,
  LineEnding = 29,
  BufferLine = 30,
  ComparePath = 5,
  CompareLeftPath = 6,
  CompareRightPath = 7,
  CompareCommitHash = 8,
  CompareCommitShortHash = 9,
  CompareRightRef = 10,
  CompareRightLabel = 11,
  CompareSelectedRow = 12,
  CompareScrollRow = 13,
  CompareHorizontalScroll = 14,
  CompareDividerFraction = 25,
  MergeBasePath = 15,
  MergeIncomingPath = 16,
  MergeCurrentPath = 17,
  MergeOutputPath = 18,
  MergeSelectedHunk = 19,
  MergeScrollRow = 20,
  MergeHorizontalScroll = 21,
  MergeLeftDividerFraction = 22,
  MergeRightDividerFraction = 23,
  MergeHunkChoice = 24,
  // Appended after the fields above; older records simply omit these and decode
  // with defaults (empty string).
  CompareReviewMode = 31,
  CompareStagingView = 32,
  Utf8Bom = 33,
};

enum class WorkspaceSessionTag : std::uint16_t {
  Schema = 1,
  ProjectRoot = 2,
  ActiveProjectIndex = 3,
};

enum class DebugStateTag : std::uint16_t {
  Schema = 1,
  FileBreakpoints = 2,            // repeated, one nested record per file
  LaunchConfig = 3,               // repeated, one nested record per config
  SelectedLaunchConfigIndex = 4,
  WatchExpression = 5,            // Phase 6: repeated, one string record per expression
  ExceptionFilter = 6,            // Phase 7: repeated, one string record per enabled filter id
  ExceptionFiltersSeeded = 7,     // Phase 7: bool, whether adapter defaults were seeded once
  FunctionBreakpoint = 8,         // additive: repeated, one nested record per function breakpoint
  ExceptionFilterCondition = 9,   // additive: repeated, one nested {filterId, condition} record
};

enum class FunctionBreakpointTag : std::uint16_t {
  Name = 1,
  Enabled = 2,
  Condition = 3,
  HitCondition = 4,
};

enum class MruStateTag : std::uint16_t {
  Schema = 1,
  RecentProjectRoot = 2,  // repeated, one path record per project
  RecentFile = 3,         // repeated, one nested record per file
};

enum class RecentFileEntryTag : std::uint16_t {
  Path = 1,
  ProjectRoot = 2,
};

enum class ExceptionFilterConditionTag : std::uint16_t {
  FilterId = 1,
  Condition = 2,
};

enum class FileBreakpointsTag : std::uint16_t {
  Path = 1,
  Breakpoint = 2,                 // repeated, one nested record per breakpoint
};

enum class BreakpointTag : std::uint16_t {
  Line = 1,
  Enabled = 2,
  Condition = 3,
  HitCondition = 4,
  LogMessage = 5,
};

enum class LaunchConfigTag : std::uint16_t {
  Name = 1,
  Type = 2,
  Request = 3,
  ArgumentsJson = 4,
};

template <typename EnumTag, typename WritePayload>
bool AppendRecord(EnumTag tag, WritePayload write_payload, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  std::vector<std::byte> payload;
  PrimitiveWriter writer(&payload);
  if (!write_payload(writer)) {
    return false;
  }
  return AppendTaggedRecord(static_cast<std::uint16_t>(tag), payload, out);
}

bool WriteSize(PrimitiveWriter& writer, std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  return writer.WriteU32(static_cast<std::uint32_t>(value));
}

bool ReadSize(PrimitiveReader& reader, std::size_t* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t parsed = 0;
  if (!reader.ReadU32(&parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

// OnRecord is a deduced callable bool(EnumTag, std::span<const std::byte>); taking
// it as a template param (mirroring AppendRecord) avoids std::function type-erasure
// on the decode tree — all 25 call sites pass inline lambdas.
template <typename EnumTag, typename OnRecord>
bool ParseRecordStream(std::span<const std::byte> input, OnRecord on_record) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    TaggedRecordView record;
    if (!ReadTaggedRecord(input, &offset, &record)) {
      return false;
    }
    if (!on_record(static_cast<EnumTag>(record.tag), record.payload)) {
      return false;
    }
  }
  return true;
}

template <typename EnumTag>
bool ReadStringRecord(EnumTag expected_tag,
                      EnumTag actual_tag,
                      std::span<const std::byte> payload,
                      std::string* out) {
  if (actual_tag != expected_tag || out == nullptr) {
    return false;
  }
  PrimitiveReader reader(payload);
  return reader.ReadString(out) && reader.remaining() == 0;
}

[[maybe_unused]] bool EncodeEditorTab(const PersistedEditorTabState& tab, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(EditorTabTag::Kind, [&](PrimitiveWriter& w) { return w.WriteString(tab.kind); }, out) ||
      !AppendRecord(EditorTabTag::Path, [&](PrimitiveWriter& w) { return w.WritePath(tab.path); }, out) ||
      !AppendRecord(EditorTabTag::CursorLine,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.cursor_line); }, out) ||
      !AppendRecord(EditorTabTag::CursorColumn,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.cursor_column); }, out) ||
      !AppendRecord(EditorTabTag::ScrollLine,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.scroll_line); }, out) ||
      !AppendRecord(EditorTabTag::HorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.horizontal_scroll); }, out) ||
      !AppendRecord(EditorTabTag::DirtySnapshot,
                    [&](PrimitiveWriter& w) { return w.WriteBool(tab.dirty_snapshot); }, out) ||
      !AppendRecord(EditorTabTag::LineEnding,
                    [&](PrimitiveWriter& w) {
                      return w.WriteU8(static_cast<std::uint8_t>(tab.line_ending));
                    },
                    out) ||
      !AppendRecord(EditorTabTag::Utf8Bom,
                    [&](PrimitiveWriter& w) { return w.WriteBool(tab.utf8_bom); }, out) ||
      !AppendRecord(EditorTabTag::ComparePath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareLeftPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_left_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareRightPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_right_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareCommitHash,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_commit_hash); }, out) ||
      !AppendRecord(EditorTabTag::CompareCommitShortHash,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_commit_short_hash); },
                    out) ||
      !AppendRecord(EditorTabTag::CompareRightRef,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_right_ref); }, out) ||
      !AppendRecord(EditorTabTag::CompareRightLabel,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_right_label); }, out) ||
      !AppendRecord(EditorTabTag::CompareSelectedRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_selected_row); }, out) ||
      !AppendRecord(EditorTabTag::CompareScrollRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_scroll_row); }, out) ||
      !AppendRecord(EditorTabTag::CompareHorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_horizontal_scroll); },
                    out) ||
      !AppendRecord(EditorTabTag::CompareDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.compare_divider_fraction); },
                    out) ||
      !AppendRecord(EditorTabTag::CompareReviewMode,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_review_mode); }, out) ||
      !AppendRecord(EditorTabTag::CompareStagingView,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_staging_view); }, out) ||
      !AppendRecord(EditorTabTag::MergeBasePath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_base_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeIncomingPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_incoming_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeCurrentPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_current_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeOutputPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_output_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeSelectedHunk,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_selected_hunk); }, out) ||
      !AppendRecord(EditorTabTag::MergeScrollRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_scroll_row); }, out) ||
      !AppendRecord(EditorTabTag::MergeHorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_horizontal_scroll); }, out) ||
      !AppendRecord(EditorTabTag::MergeLeftDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.merge_left_divider_fraction); },
                    out) ||
      !AppendRecord(EditorTabTag::MergeRightDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.merge_right_divider_fraction); },
                    out)) {
    return false;
  }

  for (const std::string& line : tab.buffer_lines) {
    if (!AppendRecord(EditorTabTag::BufferLine,
                      [&](PrimitiveWriter& w) { return w.WriteString(line); }, out)) {
      return false;
    }
  }
  for (const std::string& choice : tab.merge_hunk_choices) {
    if (!AppendRecord(EditorTabTag::MergeHunkChoice,
                      [&](PrimitiveWriter& w) { return w.WriteString(choice); }, out)) {
      return false;
    }
  }
  return true;
}

// Project-session decode budgets. The 256 MiB raw-file cap is not enough on its own:
// a tiny empty-string BufferLine record still becomes a separate std::string + vector
// slot, so millions of them turn a merely oversized session into a startup stall /
// bad_alloc before any UI. Cap the repeated nested records and charge an aggregate
// decoded-byte budget for the dirty buffer snapshot. (TD-2026-07-16-20.)
inline constexpr std::size_t kMaxSessionBufferLines = 4'000'000;
inline constexpr std::size_t kMaxSessionBufferBytes = 64u * 1024 * 1024;  // 64 MiB snapshot
inline constexpr std::size_t kMaxSessionMergeHunkChoices = 200'000;
inline constexpr std::size_t kMaxSessionTabsPerGroup = 4096;

[[maybe_unused]] bool DecodeEditorTab(std::span<const std::byte> input, PersistedEditorTabState* tab) {
  if (tab == nullptr) {
    return false;
  }
  *tab = PersistedEditorTabState{};
  std::size_t buffer_bytes = 0;
  return ParseRecordStream<EditorTabTag>(
      input, [&](EditorTabTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case EditorTabTag::Kind: return reader.ReadString(&tab->kind) && reader.remaining() == 0;
          case EditorTabTag::Path: return reader.ReadPath(&tab->path) && reader.remaining() == 0;
          case EditorTabTag::CursorLine:
            return ReadSize(reader, &tab->cursor_line) && reader.remaining() == 0;
          case EditorTabTag::CursorColumn:
            return ReadSize(reader, &tab->cursor_column) && reader.remaining() == 0;
          case EditorTabTag::ScrollLine:
            return ReadSize(reader, &tab->scroll_line) && reader.remaining() == 0;
          case EditorTabTag::HorizontalScroll:
            return ReadSize(reader, &tab->horizontal_scroll) && reader.remaining() == 0;
          case EditorTabTag::DirtySnapshot:
            return reader.ReadBool(&tab->dirty_snapshot) && reader.remaining() == 0;
          case EditorTabTag::Utf8Bom:
            return reader.ReadBool(&tab->utf8_bom) && reader.remaining() == 0;
          case EditorTabTag::LineEnding: {
            std::uint8_t line_ending = 0;
            if (!reader.ReadU8(&line_ending) || reader.remaining() != 0 || line_ending > 2) {
              return false;
            }
            tab->line_ending = static_cast<util::LineEnding>(line_ending);
            return true;
          }
          case EditorTabTag::BufferLine: {
            std::string line;
            if (!reader.ReadString(&line) || reader.remaining() != 0) {
              return false;
            }
            // Fail closed on a snapshot past the product line-count or aggregate-byte
            // budget rather than materializing millions of tiny strings.
            if (tab->buffer_lines.size() >= kMaxSessionBufferLines ||
                buffer_bytes + line.size() > kMaxSessionBufferBytes) {
              return false;
            }
            buffer_bytes += line.size();
            tab->buffer_lines.push_back(std::move(line));
            return true;
          }
          case EditorTabTag::ComparePath:
            return reader.ReadPath(&tab->compare_path) && reader.remaining() == 0;
          case EditorTabTag::CompareLeftPath:
            return reader.ReadPath(&tab->compare_left_path) && reader.remaining() == 0;
          case EditorTabTag::CompareRightPath:
            return reader.ReadPath(&tab->compare_right_path) && reader.remaining() == 0;
          case EditorTabTag::CompareCommitHash:
            return reader.ReadString(&tab->compare_commit_hash) && reader.remaining() == 0;
          case EditorTabTag::CompareCommitShortHash:
            return reader.ReadString(&tab->compare_commit_short_hash) && reader.remaining() == 0;
          case EditorTabTag::CompareRightRef:
            return reader.ReadString(&tab->compare_right_ref) && reader.remaining() == 0;
          case EditorTabTag::CompareRightLabel:
            return reader.ReadString(&tab->compare_right_label) && reader.remaining() == 0;
          case EditorTabTag::CompareSelectedRow:
            return ReadSize(reader, &tab->compare_selected_row) && reader.remaining() == 0;
          case EditorTabTag::CompareScrollRow:
            return ReadSize(reader, &tab->compare_scroll_row) && reader.remaining() == 0;
          case EditorTabTag::CompareHorizontalScroll:
            return ReadSize(reader, &tab->compare_horizontal_scroll) && reader.remaining() == 0;
          case EditorTabTag::CompareDividerFraction:
            return reader.ReadF32(&tab->compare_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::CompareReviewMode:
            return reader.ReadString(&tab->compare_review_mode) && reader.remaining() == 0;
          case EditorTabTag::CompareStagingView:
            return reader.ReadString(&tab->compare_staging_view) && reader.remaining() == 0;
          case EditorTabTag::MergeBasePath:
            return reader.ReadPath(&tab->merge_base_path) && reader.remaining() == 0;
          case EditorTabTag::MergeIncomingPath:
            return reader.ReadPath(&tab->merge_incoming_path) && reader.remaining() == 0;
          case EditorTabTag::MergeCurrentPath:
            return reader.ReadPath(&tab->merge_current_path) && reader.remaining() == 0;
          case EditorTabTag::MergeOutputPath:
            return reader.ReadPath(&tab->merge_output_path) && reader.remaining() == 0;
          case EditorTabTag::MergeSelectedHunk:
            return ReadSize(reader, &tab->merge_selected_hunk) && reader.remaining() == 0;
          case EditorTabTag::MergeScrollRow:
            return ReadSize(reader, &tab->merge_scroll_row) && reader.remaining() == 0;
          case EditorTabTag::MergeHorizontalScroll:
            return ReadSize(reader, &tab->merge_horizontal_scroll) && reader.remaining() == 0;
          case EditorTabTag::MergeLeftDividerFraction:
            return reader.ReadF32(&tab->merge_left_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::MergeRightDividerFraction:
            return reader.ReadF32(&tab->merge_right_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::MergeHunkChoice: {
            std::string choice;
            if (!reader.ReadString(&choice) || reader.remaining() != 0) {
              return false;
            }
            if (tab->merge_hunk_choices.size() >= kMaxSessionMergeHunkChoices) {
              return false;
            }
            tab->merge_hunk_choices.push_back(std::move(choice));
            return true;
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeEditorGroup(const PersistedEditorGroupState& group,
                                        std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(EditorGroupTag::ActiveTabIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, group.active_tab_index); }, out)) {
    return false;
  }
  for (const auto& tab : group.tabs) {
    std::vector<std::byte> payload;
    if (!EncodeEditorTab(tab, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(EditorGroupTag::Tab), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeEditorGroup(std::span<const std::byte> input,
                                        PersistedEditorGroupState* group) {
  if (group == nullptr) {
    return false;
  }
  *group = PersistedEditorGroupState{};
  return ParseRecordStream<EditorGroupTag>(
      input, [&](EditorGroupTag tag, std::span<const std::byte> payload) {
        switch (tag) {
          case EditorGroupTag::ActiveTabIndex: {
            PrimitiveReader reader(payload);
            return ReadSize(reader, &group->active_tab_index) && reader.remaining() == 0;
          }
          case EditorGroupTag::Tab: {
            if (group->tabs.size() >= kMaxSessionTabsPerGroup) {
              return false;  // over the product per-group tab budget
            }
            PersistedEditorTabState tab;
            if (!DecodeEditorTab(payload, &tab)) {
              return false;
            }
            group->tabs.push_back(std::move(tab));
            return true;
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeSidebarPolicy(const PersistedSidebarViewPolicy& policy,
                                          std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  PrimitiveWriter writer(out);
  return writer.WriteString(policy.view_id) && writer.WriteBool(policy.hidden) &&
         writer.WriteI32(policy.order);
}

[[maybe_unused]] bool DecodeSidebarPolicy(std::span<const std::byte> input,
                                          PersistedSidebarViewPolicy* policy) {
  if (policy == nullptr) {
    return false;
  }
  *policy = PersistedSidebarViewPolicy{};
  PrimitiveReader reader(input);
  return reader.ReadString(&policy->view_id) && reader.ReadBool(&policy->hidden) &&
         reader.ReadI32(&policy->order) && reader.remaining() == 0;
}

[[maybe_unused]] bool EncodeCommitDraft(const PersistedCommitDraftState& draft,
                                        std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(CommitDraftTag::HeadOid,
                        [&](PrimitiveWriter& w) { return w.WriteString(draft.head_oid); }, out) &&
         AppendRecord(CommitDraftTag::BranchName,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.branch_name); }, out) &&
         AppendRecord(CommitDraftTag::Subject,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.subject); }, out) &&
         AppendRecord(CommitDraftTag::Body,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.body); }, out);
}

[[maybe_unused]] bool EncodeBranchReviewHunkIdentity(const PersistedBranchReviewHunkIdentity& identity,
                                                    std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(BranchReviewHunkIdentityTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(identity.path); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::OldStart,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.old_start); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::OldCount,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.old_count); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::NewStart,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.new_start); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::NewCount,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.new_count); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::ContentHash,
                      [&](PrimitiveWriter& w) { return w.WriteU64(identity.content_hash); }, out);
}

[[maybe_unused]] bool DecodeBranchReviewHunkIdentity(std::span<const std::byte> input,
                                                    PersistedBranchReviewHunkIdentity* identity) {
  if (identity == nullptr) {
    return false;
  }
  *identity = PersistedBranchReviewHunkIdentity{};
  return ParseRecordStream<BranchReviewHunkIdentityTag>(
      input, [&](BranchReviewHunkIdentityTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case BranchReviewHunkIdentityTag::Path:
            return reader.ReadPath(&identity->path) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::OldStart:
            return reader.ReadI32(&identity->old_start) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::OldCount:
            return reader.ReadI32(&identity->old_count) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::NewStart:
            return reader.ReadI32(&identity->new_start) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::NewCount:
            return reader.ReadI32(&identity->new_count) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::ContentHash:
            // content_hash is a full-range std::hash result; the top bit is set
            // ~half the time. Read it as unsigned so a high-bit hash is preserved
            // instead of failing a signed >= 0 guard and discarding the whole
            // project-config record. Wire bytes match the prior WriteI64 memcpy.
            return reader.ReadU64(&identity->content_hash) && reader.remaining() == 0;
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeBranchReviewTarget(const PersistedBranchReviewTarget& target,
                                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(BranchReviewTargetTag::RepositoryRoot,
                    [&](PrimitiveWriter& w) { return w.WritePath(target.repository_root); }, out) ||
      !AppendRecord(BranchReviewTargetTag::BaseCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.base_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::HeadCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.head_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::MergeBaseCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.merge_base_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::SnapshotGeneration,
                    [&](PrimitiveWriter& w) { return w.WriteI64(target.snapshot_generation); }, out) ||
      !AppendRecord(BranchReviewTargetTag::LastAccessedUnixMs,
                    [&](PrimitiveWriter& w) { return w.WriteI64(target.last_accessed_unix_ms); }, out)) {
    return false;
  }
  for (const PersistedBranchReviewFileEntry& file : target.reviewed_files) {
    std::vector<std::byte> payload;
    if (!AppendRecord(BranchReviewFileEntryTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(file.path); }, &payload) ||
        !AppendRecord(BranchReviewFileEntryTag::ReviewedSnapshotGeneration,
                      [&](PrimitiveWriter& w) {
                        return w.WriteI64(file.reviewed_snapshot_generation);
                      },
                      &payload) ||
        !AppendRecord(BranchReviewFileEntryTag::ReviewedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(file.reviewed_at_unix_ms); },
                      &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::ReviewedFile), payload,
                            out)) {
      return false;
    }
  }
  for (const PersistedBranchReviewHunkEntry& hunk : target.reviewed_hunks) {
    std::vector<std::byte> identity_payload;
    std::vector<std::byte> entry_payload;
    if (!EncodeBranchReviewHunkIdentity(hunk.identity, &identity_payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewHunkEntryTag::Identity),
                            identity_payload, &entry_payload) ||
        !AppendRecord(BranchReviewHunkEntryTag::ReviewedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(hunk.reviewed_at_unix_ms); },
                      &entry_payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::ReviewedHunk),
                            entry_payload, out)) {
      return false;
    }
  }
  for (const PersistedBranchReviewNote& note : target.notes) {
    std::vector<std::byte> payload;
    if (!AppendRecord(BranchReviewNoteTag::Scope,
                      [&](PrimitiveWriter& w) { return w.WriteString(note.scope); }, &payload) ||
        !AppendRecord(BranchReviewNoteTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(note.path); }, &payload)) {
      return false;
    }
    if (note.hunk_identity.has_value()) {
      std::vector<std::byte> identity_payload;
      if (!EncodeBranchReviewHunkIdentity(*note.hunk_identity, &identity_payload) ||
          !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewNoteTag::HunkIdentity),
                              identity_payload, &payload)) {
        return false;
      }
    }
    if (!AppendRecord(BranchReviewNoteTag::Text,
                      [&](PrimitiveWriter& w) { return w.WriteString(note.text); }, &payload) ||
        !AppendRecord(BranchReviewNoteTag::UpdatedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(note.updated_at_unix_ms); },
                      &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::Note), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeBranchReviewTarget(std::span<const std::byte> input,
                                              PersistedBranchReviewTarget* target) {
  if (target == nullptr) {
    return false;
  }
  *target = PersistedBranchReviewTarget{};
  return ParseRecordStream<BranchReviewTargetTag>(
      input, [&](BranchReviewTargetTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case BranchReviewTargetTag::RepositoryRoot:
            return reader.ReadPath(&target->repository_root) && reader.remaining() == 0;
          case BranchReviewTargetTag::BaseCommit:
            return reader.ReadString(&target->base_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::HeadCommit:
            return reader.ReadString(&target->head_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::MergeBaseCommit:
            return reader.ReadString(&target->merge_base_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::SnapshotGeneration: {
            std::int64_t value = 0;
            return reader.ReadI64(&value) && reader.remaining() == 0 && value >= 0 &&
                   (target->snapshot_generation = static_cast<std::uint64_t>(value), true);
          }
          case BranchReviewTargetTag::LastAccessedUnixMs: {
            std::int64_t value = 0;
            return reader.ReadI64(&value) && reader.remaining() == 0 && value >= 0 &&
                   (target->last_accessed_unix_ms = static_cast<std::uint64_t>(value), true);
          }
          case BranchReviewTargetTag::ReviewedFile: {
            PersistedBranchReviewFileEntry file;
            return ParseRecordStream<BranchReviewFileEntryTag>(
                payload, [&](BranchReviewFileEntryTag file_tag,
                             std::span<const std::byte> file_payload) {
                  PrimitiveReader file_reader(file_payload);
                  switch (file_tag) {
                    case BranchReviewFileEntryTag::Path:
                      return file_reader.ReadPath(&file.path) && file_reader.remaining() == 0;
                    case BranchReviewFileEntryTag::ReviewedSnapshotGeneration: {
                      std::int64_t value = 0;
                      return file_reader.ReadI64(&value) && file_reader.remaining() == 0 &&
                             value >= 0 &&
                             (file.reviewed_snapshot_generation = static_cast<std::uint64_t>(value),
                              true);
                    }
                    case BranchReviewFileEntryTag::ReviewedAtUnixMs: {
                      std::int64_t value = 0;
                      return file_reader.ReadI64(&value) && file_reader.remaining() == 0 &&
                             value >= 0 &&
                             (file.reviewed_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) &&
                   // Enforce the live service's per-target file cap at decode so a
                   // corrupt/hostile .microide config cannot bypass the caps that
                   // runtime PruneTarget maintains. Excess entries are dropped (keep
                   // the first N deterministically). (TD-2026-07-16-34.)
                   ((target->reviewed_files.size() >=
                             compare::BranchReviewStateService::kMaxFileEntriesPerTarget
                         ? true
                         : (target->reviewed_files.push_back(std::move(file)), true)));
          }
          case BranchReviewTargetTag::ReviewedHunk: {
            PersistedBranchReviewHunkEntry hunk;
            return ParseRecordStream<BranchReviewHunkEntryTag>(
                payload, [&](BranchReviewHunkEntryTag hunk_tag,
                             std::span<const std::byte> hunk_payload) {
                  switch (hunk_tag) {
                    case BranchReviewHunkEntryTag::Identity:
                      return DecodeBranchReviewHunkIdentity(hunk_payload, &hunk.identity);
                    case BranchReviewHunkEntryTag::ReviewedAtUnixMs: {
                      PrimitiveReader hunk_reader(hunk_payload);
                      std::int64_t value = 0;
                      return hunk_reader.ReadI64(&value) && hunk_reader.remaining() == 0 &&
                             value >= 0 &&
                             (hunk.reviewed_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) &&
                   ((target->reviewed_hunks.size() >=
                             compare::BranchReviewStateService::kMaxHunkEntriesPerTarget
                         ? true
                         : (target->reviewed_hunks.push_back(std::move(hunk)), true)));
          }
          case BranchReviewTargetTag::Note: {
            PersistedBranchReviewNote note;
            return ParseRecordStream<BranchReviewNoteTag>(
                payload, [&](BranchReviewNoteTag note_tag, std::span<const std::byte> note_payload) {
                  PrimitiveReader note_reader(note_payload);
                  switch (note_tag) {
                    case BranchReviewNoteTag::Scope:
                      return note_reader.ReadString(&note.scope) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::Path:
                      return note_reader.ReadPath(&note.path) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::HunkIdentity: {
                      PersistedBranchReviewHunkIdentity identity;
                      if (!DecodeBranchReviewHunkIdentity(note_payload, &identity)) {
                        return false;
                      }
                      note.hunk_identity = std::move(identity);
                      return true;
                    }
                    case BranchReviewNoteTag::Text:
                      return note_reader.ReadString(&note.text) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::UpdatedAtUnixMs: {
                      std::int64_t value = 0;
                      return note_reader.ReadI64(&value) && note_reader.remaining() == 0 &&
                             value >= 0 &&
                             (note.updated_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) &&
                   ((target->notes.size() >=
                             compare::BranchReviewStateService::kMaxNotesPerTarget
                         ? true
                         : (target->notes.push_back(std::move(note)), true)));
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeBranchReviewState(const PersistedBranchReviewState& state,
                                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  for (const PersistedBranchReviewTarget& target : state.targets) {
    std::vector<std::byte> payload;
    if (!EncodeBranchReviewTarget(target, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewStateTag::Target), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeBranchReviewState(std::span<const std::byte> input,
                                              PersistedBranchReviewState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedBranchReviewState{};
  return ParseRecordStream<BranchReviewStateTag>(
      input, [&](BranchReviewStateTag tag, std::span<const std::byte> payload) {
        if (tag != BranchReviewStateTag::Target) {
          return true;
        }
        // Enforce the live service's per-repository target cap at decode. Excess
        // targets are dropped (keep the first N) so a corrupt config cannot load more
        // review targets than runtime prunes to. (TD-2026-07-16-34.)
        if (state->targets.size() >= compare::BranchReviewStateService::kMaxTargetsPerRepository) {
          return true;
        }
        PersistedBranchReviewTarget target;
        if (!DecodeBranchReviewTarget(payload, &target)) {
          return false;
        }
        state->targets.push_back(std::move(target));
        return true;
      });
}

[[maybe_unused]] bool DecodeCommitDraft(std::span<const std::byte> input,
                                        PersistedCommitDraftState* draft) {
  if (draft == nullptr) {
    return false;
  }
  *draft = PersistedCommitDraftState{};
  // Per-field decode budgets: a persisted commit draft is a small INLINE editor draft,
  // not a bulk payload. Without these one string inside the 256 MiB raw-record cap can
  // load a huge body into the sidebar TextViewport and get re-snapshotted on every
  // precheck/save/dispatch. Fail closed on an over-budget field. (TD-2026-07-16-37.)
  constexpr std::size_t kMaxDraftOidBytes = 128;      // a git oid is 40-64 hex chars
  constexpr std::size_t kMaxDraftBranchBytes = 1024;  // ref-name length bound
  constexpr std::size_t kMaxDraftSubjectBytes = 8192;
  constexpr std::size_t kMaxDraftBodyBytes = 1u << 20;  // 1 MiB inline body ceiling
  const auto read_capped = [&](PrimitiveReader& reader, std::string* out,
                               std::size_t cap) -> bool {
    return reader.ReadString(out) && reader.remaining() == 0 && out->size() <= cap;
  };
  return ParseRecordStream<CommitDraftTag>(
      input, [&](CommitDraftTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case CommitDraftTag::HeadOid:
            return read_capped(reader, &draft->head_oid, kMaxDraftOidBytes);
          case CommitDraftTag::BranchName:
            return read_capped(reader, &draft->branch_name, kMaxDraftBranchBytes);
          case CommitDraftTag::Subject:
            return read_capped(reader, &draft->subject, kMaxDraftSubjectBytes);
          case CommitDraftTag::Body:
            return read_capped(reader, &draft->body, kMaxDraftBodyBytes);
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeSettingPair(const std::pair<std::string, std::string>& setting,
                                        std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  PrimitiveWriter writer(out);
  return writer.WriteString(setting.first) && writer.WriteString(setting.second);
}

[[maybe_unused]] bool DecodeSettingPair(std::span<const std::byte> input,
                                        std::pair<std::string, std::string>* setting) {
  if (setting == nullptr) {
    return false;
  }
  PrimitiveReader reader(input);
  return reader.ReadString(&setting->first) && reader.ReadString(&setting->second) &&
         reader.remaining() == 0;
}

}  // namespace

}  // namespace microide::workspace
