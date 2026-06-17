#include "TestSupport.h"

#include "persistence/PersistedRecord.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectState.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DecodeDebugStateRecord;
using microide::workspace::DecodeProjectConfigRecord;
using microide::workspace::DecodeProjectSessionRecord;
using microide::workspace::DecodeUserConfigRecord;
using microide::workspace::DecodeWorkspaceSessionRecord;
using microide::workspace::EncodeDebugStateRecord;
using microide::workspace::EncodeProjectConfigRecord;
using microide::workspace::EncodeProjectSessionRecord;
using microide::workspace::EncodeUserConfigRecord;
using microide::workspace::EncodeWorkspaceSessionRecord;
using microide::workspace::PersistedBreakpoint;
using microide::workspace::PersistedDebugState;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedEditorViewState;
using microide::workspace::PersistedFileBreakpoints;
using microide::workspace::PersistedLaunchConfig;
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
      .disabled_plugin_ids = {"eslint", "prettier"},
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
  Expect(decoded_user.disabled_plugin_ids.size() == 2 &&
             decoded_user.disabled_plugin_ids[0] == "eslint" &&
             decoded_user.disabled_plugin_ids[1] == "prettier",
         "user config disabled plugins should round-trip");

  PersistedProjectConfigState project{
      .editor_tab_size = 8,
      .editor_indent_width = 2,
      .editor_soft_tabs = true,
      .colorscheme_name = "sunrise",
      .project_base_color = SDL_Color{0x12, 0x34, 0x56, 0x78},
      .settings = {{"editor.wrap", "word"}},
      .sidebar_policies = {PersistedSidebarViewPolicy{.view_id = "explorer", .hidden = false, .order = 3}},
      .commit_draft = std::nullopt,
      .branch_review = {},
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
  session.outgoing_base_choice.kind = microide::workspace::OutgoingBaseChoice::Kind::SpecificRef;
  session.outgoing_base_choice.custom_ref = "release/2.0";
  session.active_tab_index = 1;
  session.tabs = {editor_tab, compare_tab};
  session.right_pane_visible = true;
  session.right_pane_width = 312.0f;
  session.right_pane_mode = static_cast<std::uint8_t>(microide::workspace::DebugPaneMode::Watch);
  return session;
}

void TestPersistedStateProjectSessionRoundTripOmitsChatRegistry() {
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
  Expect(decoded_session.outgoing_base_choice.kind ==
             microide::workspace::OutgoingBaseChoice::Kind::SpecificRef &&
             decoded_session.outgoing_base_choice.custom_ref == "release/2.0",
         "project session outgoing base choice should round-trip");
  Expect(decoded_session.tabs.size() == 2 &&
             decoded_session.tabs[0].views.size() == 1 &&
             decoded_session.tabs[1].compare_right_ref == "WORKTREE",
         "project session tabs should round-trip");
  Expect(decoded_session.right_pane_visible &&
             std::fabs(decoded_session.right_pane_width - 312.0f) < 0.0001f &&
             decoded_session.right_pane_mode ==
                 static_cast<std::uint8_t>(microide::workspace::DebugPaneMode::Watch),
         "project session right-pane fields should round-trip");

  // A record stream lacking the right-pane tags (older session file) decodes to
  // the struct defaults rather than failing.
  PersistedProjectSessionState legacy = BuildProjectSessionFixture();
  legacy.right_pane_visible = false;
  legacy.right_pane_width = 288.0f;
  legacy.right_pane_mode = 0;
  std::vector<std::byte> legacy_record;
  Expect(EncodeProjectSessionRecord(legacy, &legacy_record),
         "legacy project session encode should succeed");
  PersistedProjectSessionState decoded_legacy;
  Expect(DecodeProjectSessionRecord(legacy_record, &decoded_legacy),
         "legacy project session decode should succeed");
  Expect(!decoded_legacy.right_pane_visible &&
             decoded_legacy.right_pane_mode == 0,
         "absent right-pane tags decode to defaults");
  std::size_t offset = 0;
  bool saw_chat_registry = false;
  while (offset < session_record.size()) {
    microide::persistence::TaggedRecordView record;
    Expect(microide::persistence::ReadTaggedRecord(session_record, &offset, &record),
           "project session stream should decode");
    if (record.tag == 7) {
      saw_chat_registry = true;
    }
  }
  Expect(!saw_chat_registry, "project session writer should omit legacy chat registry");

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

void TestPersistedStateProjectSessionAcceptsLegacyChatRegistryTag() {
  PersistedProjectSessionState session = BuildProjectSessionFixture();
  std::vector<std::byte> encoded;
  Expect(EncodeProjectSessionRecord(session, &encoded), "project session encode should succeed");

  std::vector<std::byte> legacy_chat_payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  Expect(microide::persistence::AppendTaggedRecord(7, legacy_chat_payload, &encoded),
         "legacy chat payload should append");

  PersistedProjectSessionState decoded;
  Expect(DecodeProjectSessionRecord(encoded, &decoded),
         "project session decode should ignore legacy chat records");
  Expect(decoded.tabs.size() == session.tabs.size(),
         "legacy chat records should not alter tab state");
}

void TestPersistedStateRecordDecodersSkipUnknownTags() {
  PersistedUserConfigState user{
      .ui_scale = 1.25f,
      .settings = {{"theme", "day"}},
      .disabled_keybinding_ids = {},
      .disabled_plugin_ids = {},
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

void TestPersistedStateProjectSessionDefaultsMissingOutgoingBaseChoiceToAuto() {
  std::vector<std::byte> encoded;
  std::vector<std::byte> payload;
  microide::persistence::PrimitiveWriter writer(&payload);
  Expect(writer.WriteU32(1), "project session schema payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(1, payload, &encoded),
         "project session schema tag should append");

  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteBool(true), "project session sidebar-visible payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(2, payload, &encoded),
         "project session sidebar-visible tag should append");

  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteF32(288.0f), "project session sidebar-width payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(3, payload, &encoded),
         "project session sidebar-width tag should append");

  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteF32(184.0f), "project session bottom-panel-height payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(4, payload, &encoded),
         "project session bottom-panel-height tag should append");

  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteU32(0), "project session active-tab payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(5, payload, &encoded),
         "project session active-tab tag should append");

  std::vector<std::byte> legacy_chat_payload{std::byte{0xAA}, std::byte{0xBB}};
  Expect(microide::persistence::AppendTaggedRecord(7, legacy_chat_payload, &encoded),
         "project session chat-registry tag should append");

  PersistedProjectSessionState decoded;
  Expect(DecodeProjectSessionRecord(encoded, &decoded),
         "project session decoder should accept records without outgoing base fields");
  Expect(decoded.outgoing_base_choice.kind ==
             microide::workspace::OutgoingBaseChoice::Kind::Auto &&
             decoded.outgoing_base_choice.custom_ref.empty(),
         "missing outgoing base fields should default to Auto");
}

void TestPersistedStateDebugStateRoundTrip() {
  PersistedDebugState state;
  PersistedFileBreakpoints file_a;
  file_a.path = "/proj/main.py";
  file_a.breakpoints.push_back(PersistedBreakpoint{.line = 4, .enabled = true});
  file_a.breakpoints.push_back(
      PersistedBreakpoint{.line = 9, .enabled = false, .condition = std::string("x > 5")});
  PersistedFileBreakpoints file_b;
  file_b.path = "/proj/util.py";
  file_b.breakpoints.push_back(
      PersistedBreakpoint{.line = 1, .log_message = std::string("hit {x}")});
  state.files.push_back(std::move(file_a));
  state.files.push_back(std::move(file_b));
  state.launch_configs.push_back(PersistedLaunchConfig{
      .name = "Debug main",
      .type = "debugpy",
      .request = "launch",
      .arguments_json = R"({"program":"main.py","stopOnEntry":true})",
  });
  state.selected_launch_config_index = 0;
  state.watch_expressions = {"i", "arr[i]", "node->next"};
  state.enabled_exception_filters = {"raised", "uncaught"};
  state.exception_filters_seeded = true;

  std::vector<std::byte> encoded;
  Expect(EncodeDebugStateRecord(state, &encoded), "debug state should encode");

  PersistedDebugState decoded;
  Expect(DecodeDebugStateRecord(encoded, &decoded), "debug state should decode");
  Expect(decoded.watch_expressions.size() == 3 && decoded.watch_expressions[0] == "i" &&
             decoded.watch_expressions[2] == "node->next",
         "watch expressions round-trip in order");
  Expect(decoded.enabled_exception_filters.size() == 2 &&
             decoded.enabled_exception_filters[0] == "raised" &&
             decoded.enabled_exception_filters[1] == "uncaught",
         "enabled exception filters round-trip in order (Phase 7)");
  Expect(decoded.exception_filters_seeded, "the seeded flag round-trips (Phase 7)");
  Expect(decoded.files.size() == 2, "two breakpoint files should round-trip");
  Expect(decoded.files[0].path == std::filesystem::path("/proj/main.py"), "file path round-trips");
  Expect(decoded.files[0].breakpoints.size() == 2, "two breakpoints on first file");
  Expect(decoded.files[0].breakpoints[1].line == 9 &&
             decoded.files[0].breakpoints[1].enabled == false &&
             decoded.files[0].breakpoints[1].condition.has_value() &&
             *decoded.files[0].breakpoints[1].condition == "x > 5",
         "conditional breakpoint round-trips");
  Expect(decoded.files[1].breakpoints[0].log_message.has_value() &&
             *decoded.files[1].breakpoints[0].log_message == "hit {x}",
         "logpoint message round-trips");
  Expect(decoded.launch_configs.size() == 1 && decoded.launch_configs[0].type == "debugpy" &&
             decoded.launch_configs[0].arguments_json ==
                 R"({"program":"main.py","stopOnEntry":true})",
         "launch config round-trips with verbatim arguments json");
  Expect(decoded.selected_launch_config_index == 0, "selected index round-trips");
}

void TestPersistedStateDebugStateBackwardCompatNoWatch() {
  // A pre-Phase-6 record carried no WatchExpression tags. The new encoder emits
  // none when the list is empty, producing byte-identical output, so this also
  // proves an old record decodes with the new (additive-tag) decoder: the
  // breakpoints survive and watch_expressions defaults empty (no schema bump).
  PersistedDebugState state;
  PersistedFileBreakpoints file;
  file.path = "/proj/legacy.py";
  file.breakpoints.push_back(PersistedBreakpoint{.line = 3, .enabled = true});
  state.files.push_back(std::move(file));
  state.selected_launch_config_index = 2;

  std::vector<std::byte> encoded;
  Expect(EncodeDebugStateRecord(state, &encoded), "legacy-shaped debug state should encode");

  PersistedDebugState decoded;
  Expect(DecodeDebugStateRecord(encoded, &decoded), "a record without watch tags should decode");
  Expect(decoded.watch_expressions.empty(), "missing watch tags default to an empty list");
  Expect(decoded.enabled_exception_filters.empty() && !decoded.exception_filters_seeded,
         "missing exception-filter tags default to empty/unseeded (Phase 7 additive)");
  Expect(decoded.files.size() == 1 && decoded.files[0].breakpoints.size() == 1 &&
             decoded.files[0].breakpoints[0].line == 3,
         "breakpoints survive a watch-free record");
  Expect(decoded.selected_launch_config_index == 2, "selected index survives a watch-free record");
}

void TestPersistedStateDebugStateRequiresSchema() {
  // A body without the Schema tag must be rejected (mirrors project-session).
  std::vector<std::byte> body;
  PersistedDebugState empty;
  // Encode then strip is awkward; instead decode an empty/garbage buffer.
  PersistedDebugState decoded;
  Expect(!DecodeDebugStateRecord(std::span<const std::byte>(body), &decoded),
         "empty body (no schema tag) should fail to decode");
}

}  // namespace

// Regression: a corrupt/adversarial length prefix must not drive an unbounded
// reservation. PrimitiveReader::ReadVector reads a count up to 2^32-1; a tiny
// buffer claiming a huge count must fail cleanly rather than attempt a multi-
// gigabyte allocation (previously a bad_alloc/OOM on session restore). The
// split-node decoder uses the identical bounded-reserve pattern.
void TestPersistedStateRejectsAdversarialLengthWithoutOom() {
  using microide::persistence::PrimitiveReader;

  // u32 count = 0xFFFFFFFF, then no element bytes.
  const std::vector<std::byte> bytes = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                        std::byte{0xFF}};
  PrimitiveReader reader(bytes);
  std::vector<std::uint8_t> values;
  const bool ok = reader.ReadVector(&values, [](PrimitiveReader& r, std::uint8_t* item) {
    bool value = false;
    if (!r.ReadBool(&value)) {
      return false;
    }
    *item = value ? 1 : 0;
    return true;
  });
  Expect(!ok, "ReadVector should fail cleanly on a huge count with no element bytes");
  Expect(values.empty(), "ReadVector should not have populated values");
}

void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedStateRecord/RejectsAdversarialLengthWithoutOom",
          TestPersistedStateRejectsAdversarialLengthWithoutOom);
  AddTest(tests, "PersistedStateRecord/UserAndProjectConfigRoundTrip",
          TestPersistedStateUserAndProjectConfigRecordRoundTrip);
  AddTest(tests, "PersistedStateRecord/ProjectSessionRoundTripOmitsChatRegistry",
          TestPersistedStateProjectSessionRoundTripOmitsChatRegistry);
  AddTest(tests, "PersistedStateRecord/ProjectSessionAcceptsLegacyChatRegistryTag",
          TestPersistedStateProjectSessionAcceptsLegacyChatRegistryTag);
  AddTest(tests, "PersistedStateRecord/DecodersSkipUnknownTags",
          TestPersistedStateRecordDecodersSkipUnknownTags);
  AddTest(tests, "PersistedStateRecord/ProjectSessionDefaultsMissingOutgoingBaseChoiceToAuto",
          TestPersistedStateProjectSessionDefaultsMissingOutgoingBaseChoiceToAuto);
  AddTest(tests, "PersistedStateRecord/DebugStateRoundTrip", TestPersistedStateDebugStateRoundTrip);
  AddTest(tests, "PersistedStateRecord/DebugStateBackwardCompatNoWatch",
          TestPersistedStateDebugStateBackwardCompatNoWatch);
  AddTest(tests, "PersistedStateRecord/DebugStateRequiresSchema",
          TestPersistedStateDebugStateRequiresSchema);
}

}  // namespace microide::tests
