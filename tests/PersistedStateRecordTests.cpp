#include "TestSupport.h"

#include "persistence/PersistedRecord.h"
#include "workspace/WorkspacePersistenceFormat.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DecodeConversationRegistryRecord;
using microide::workspace::DecodeProjectConfigRecord;
using microide::workspace::DecodeProjectSessionRecord;
using microide::workspace::DecodeUserConfigRecord;
using microide::workspace::DecodeWorkspaceSessionRecord;
using microide::workspace::EncodeConversationRegistryRecord;
using microide::workspace::EncodeProjectConfigRecord;
using microide::workspace::EncodeProjectSessionRecord;
using microide::workspace::EncodeUserConfigRecord;
using microide::workspace::EncodeWorkspaceSessionRecord;
using microide::workspace::PersistedChatState;
using microide::workspace::PersistedConversationState;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedEditorViewState;
using microide::workspace::PersistedMessageState;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::PersistedSidebarViewPolicy;
using microide::workspace::PersistedSplitNodeState;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::PersistedWorkspaceSessionState;

void TestPersistedStateUserAndProjectConfigRecordRoundTrip() {
  PersistedUserConfigState user{
      .ui_scale = 1.5f,
      .settings = {{"theme", "solarized"}, {"diagnostics.inline", "true"}},
      .disabled_keybinding_ids = {"terminal.focus", "sidebar.toggle"},
  };
  std::vector<std::byte> encoded_user;
  Expect(EncodeUserConfigRecord(user, &encoded_user), "user config record encode should succeed");
  PersistedUserConfigState decoded_user;
  Expect(DecodeUserConfigRecord(encoded_user, &decoded_user), "user config record decode should succeed");
  Expect(std::fabs(decoded_user.ui_scale - 1.5f) < 0.0001f, "user config ui scale should round-trip");
  Expect(decoded_user.settings.size() == 2 && decoded_user.settings[1].first == "diagnostics.inline",
         "user config settings should round-trip");
  Expect(decoded_user.disabled_keybinding_ids.size() == 2 &&
             decoded_user.disabled_keybinding_ids[0] == "terminal.focus",
         "user config disabled keybindings should round-trip");

  PersistedProjectConfigState project{
      .editor_tab_size = 8,
      .editor_indent_width = 2,
      .editor_soft_tabs = true,
      .colorscheme_name = "sunrise",
      .project_base_color = SDL_Color{0x12, 0x34, 0x56, 0x78},
      .settings = {{"editor.wrap", "word"}},
      .sidebar_policies = {PersistedSidebarViewPolicy{.view_id = "explorer", .hidden = false, .order = 3}},
  };
  std::vector<std::byte> encoded_project;
  Expect(EncodeProjectConfigRecord(project, &encoded_project),
         "project config record encode should succeed");
  PersistedProjectConfigState decoded_project;
  Expect(DecodeProjectConfigRecord(encoded_project, &decoded_project),
         "project config record decode should succeed");
  Expect(decoded_project.editor_tab_size == 8 && decoded_project.editor_indent_width == 2 &&
             decoded_project.editor_soft_tabs,
         "project config editor settings should round-trip");
  Expect(decoded_project.project_base_color.has_value() &&
             decoded_project.project_base_color->r == 0x12 &&
             decoded_project.project_base_color->a == 0x78,
         "project config project base color should round-trip");
  Expect(decoded_project.settings.size() == 1 && decoded_project.settings[0].second == "word",
         "project config settings should round-trip");
  Expect(decoded_project.sidebar_policies.size() == 1 &&
             decoded_project.sidebar_policies[0].order == 3,
         "project config sidebar policy should round-trip");
}

PersistedChatState BuildChatFixture() {
  PersistedChatState chat;
  chat.active_conversation_id = "conv-1";
  chat.conversations.push_back(PersistedConversationState{
      .schema_version = 5,
      .id = "conv-1",
      .title = "Chat",
      .provider_id = "openai.chat",
      .model_id = "gpt-4.1-mini",
      .status = "succeeded",
      .tool_mode = "ask",
      .draft = "draft",
      .system_prompt = "be concise",
      .created_at = "2026-04-28T08:00:00Z",
      .updated_at = "2026-04-28T08:00:01Z",
      .last_request_duration_ms = 123,
      .messages = {
          PersistedMessageState{
              .id = "msg-1",
              .role = "assistant",
              .content = "hello",
              .timestamp = "2026-04-28T08:00:01Z",
              .provider_id = "openai.chat",
              .model = "gpt-4.1-mini",
              .status = "succeeded",
              .request_duration_ms = 123,
              .error = {},
              .tool_events = {
                  PersistedMessageState::PersistedToolEventState{
                      .call_id = "tool-1",
                      .tool_id = "phase5.echo",
                      .display_name = "Echo",
                      .arguments_summary = "{\"ping\":1}",
                      .status = "completed",
                      .permission_decision = "session",
                      .capability_scope = "phase5.echo",
                      .started_at = "2026-04-28T08:00:00Z",
                      .finished_at = "2026-04-28T08:00:01Z",
                      .duration_ms = 11,
                      .error = {},
                      .output_summary = "{\"pong\":1}",
                  },
              },
          },
      },
  });
  return chat;
}

PersistedProjectSessionState BuildProjectSessionFixture() {
  PersistedEditorTabState editor_tab;
  editor_tab.kind = "editor";
  editor_tab.active_leaf_id = 9;
  editor_tab.views.push_back(PersistedEditorViewState{
      .leaf_id = 9,
      .path = "/tmp/project/src/main.cpp",
      .cursor_line = 12,
      .cursor_column = 4,
      .scroll_line = 8,
      .horizontal_scroll = 2,
      .dirty_snapshot = true,
      .line_ending = microide::util::LineEnding::CRLF,
      .buffer_lines = {"line1", "line2"},
  });
  editor_tab.split_nodes.push_back(PersistedSplitNodeState{
      .path = {},
      .orientation = "leaf",
      .size_fraction = 1.0f,
      .leaf_id = 9,
  });

  PersistedEditorTabState compare_tab;
  compare_tab.kind = "compare";
  compare_tab.compare_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_left_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_right_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_commit_hash = "abcdef123456";
  compare_tab.compare_commit_short_hash = "abcdef1";
  compare_tab.compare_right_ref = "WORKTREE";
  compare_tab.compare_right_label = "Working tree";
  compare_tab.compare_selected_row = 3;
  compare_tab.compare_scroll_row = 4;
  compare_tab.compare_horizontal_scroll = 5;

  PersistedProjectSessionState session;
  session.sidebar_visible = false;
  session.sidebar_width = 320.0f;
  session.bottom_panel_height = 208.0f;
  session.active_tab_index = 1;
  session.tabs = {editor_tab, compare_tab};
  session.chat = BuildChatFixture();
  return session;
}

void TestPersistedStateConversationAndSessionRecordRoundTrip() {
  PersistedChatState chat = BuildChatFixture();
  std::vector<std::byte> chat_record;
  Expect(EncodeConversationRegistryRecord(chat, &chat_record),
         "conversation registry encode should succeed");
  PersistedChatState decoded_chat;
  Expect(DecodeConversationRegistryRecord(chat_record, &decoded_chat),
         "conversation registry decode should succeed");
  Expect(decoded_chat.active_conversation_id == "conv-1" &&
             decoded_chat.conversations.size() == 1,
         "conversation registry identity should round-trip");
  Expect(decoded_chat.conversations[0].messages.size() == 1 &&
             decoded_chat.conversations[0].messages[0].tool_events.size() == 1 &&
             decoded_chat.conversations[0].messages[0].tool_events[0].permission_decision == "session",
         "conversation registry tool events should round-trip");

  PersistedProjectSessionState session = BuildProjectSessionFixture();
  std::vector<std::byte> session_record;
  Expect(EncodeProjectSessionRecord(session, &session_record),
         "project session encode should succeed");
  PersistedProjectSessionState decoded_session;
  Expect(DecodeProjectSessionRecord(session_record, &decoded_session),
         "project session decode should succeed");
  Expect(!decoded_session.sidebar_visible &&
             std::fabs(decoded_session.sidebar_width - 320.0f) < 0.0001f &&
             decoded_session.active_tab_index == 1,
         "project session top-level fields should round-trip");
  Expect(decoded_session.tabs.size() == 2 &&
             decoded_session.tabs[0].views.size() == 1 &&
             decoded_session.tabs[1].compare_right_ref == "WORKTREE",
         "project session tabs should round-trip");
  Expect(decoded_session.chat.conversations.size() == 1 &&
             decoded_session.chat.conversations[0].messages.size() == 1,
         "project session embedded chat should round-trip");

  PersistedWorkspaceSessionState workspace{
      .project_roots = {"/tmp/project-a", "/tmp/project-b"},
      .active_project_index = 1,
  };
  std::vector<std::byte> workspace_record;
  Expect(EncodeWorkspaceSessionRecord(workspace, &workspace_record),
         "workspace session encode should succeed");
  PersistedWorkspaceSessionState decoded_workspace;
  Expect(DecodeWorkspaceSessionRecord(workspace_record, &decoded_workspace),
         "workspace session decode should succeed");
  Expect(decoded_workspace.project_roots.size() == 2 &&
             decoded_workspace.project_roots[1] == "/tmp/project-b" &&
             decoded_workspace.active_project_index == 1,
         "workspace session should round-trip");
}

void TestPersistedStateRecordDecodersSkipUnknownTags() {
  PersistedUserConfigState user{
      .ui_scale = 1.25f,
      .settings = {{"theme", "day"}},
      .disabled_keybinding_ids = {},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeUserConfigRecord(user, &encoded), "user encode should succeed");

  std::vector<std::byte> unknown_payload;
  microide::persistence::PrimitiveWriter unknown_writer(&unknown_payload);
  Expect(unknown_writer.WriteString("unknown"), "unknown payload write should succeed");
  Expect(microide::persistence::AppendTaggedRecord(65000, unknown_payload, &encoded),
         "appending unknown tag should succeed");

  PersistedUserConfigState decoded;
  Expect(DecodeUserConfigRecord(encoded, &decoded),
         "user decode should skip unknown tags");
  Expect(std::fabs(decoded.ui_scale - 1.25f) < 0.0001f &&
             decoded.settings.size() == 1 && decoded.settings[0].second == "day",
         "known fields should remain intact when unknown tags are present");
}

}  // namespace

void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedStateRecord/UserAndProjectConfigRoundTrip",
          TestPersistedStateUserAndProjectConfigRecordRoundTrip);
  AddTest(tests, "PersistedStateRecord/ConversationAndSessionRoundTrip",
          TestPersistedStateConversationAndSessionRecordRoundTrip);
  AddTest(tests, "PersistedStateRecord/DecodersSkipUnknownTags",
          TestPersistedStateRecordDecodersSkipUnknownTags);
}

}  // namespace microide::tests
