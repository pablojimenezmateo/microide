#include "TestSupport.h"

#include "persistence/PersistedRecord.h"
#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"
#include "workspace/persistence/PersistenceService.h"
#include "workspace/persistence/RecentsService.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/state/WorkspaceProjectState.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DecodeDebugStateRecord;
using microide::workspace::DecodeMruRecord;
using microide::workspace::DecodeProjectConfigRecord;
using microide::workspace::DecodeProjectSessionRecord;
using microide::workspace::DecodeUserConfigRecord;
using microide::workspace::DecodeWorkspaceSessionRecord;
using microide::workspace::EncodeDebugStateRecord;
using microide::workspace::EncodeMruRecord;
using microide::workspace::EncodeProjectConfigRecord;
using microide::workspace::EncodeProjectSessionRecord;
using microide::workspace::EncodeUserConfigRecord;
using microide::workspace::EncodeWorkspaceSessionRecord;
using microide::workspace::PersistedBreakpoint;
using microide::workspace::PersistedDebugState;
using microide::workspace::PersistedEditorGroupState;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedFileBreakpoints;
using microide::workspace::PersistedFunctionBreakpoint;
using microide::workspace::PersistedLaunchConfig;
using microide::workspace::PersistedMruState;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedRecentFile;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::PersistedSidebarViewPolicy;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::PersistedWorkspaceSessionState;

// Regression: a config record with duplicate setting ids (a corrupt/hand-edited
// file) must decode to a single last-writer-wins entry, not a split-brain state
// where the UI shows one value and layering applies another.
void TestPersistedStateConfigDedupesDuplicateSettingIds() {
  PersistedUserConfigState user{
      .ui_scale = 1.0f,
      .settings = {{"editor.tab_size", "2"}, {"other", "x"}, {"editor.tab_size", "8"}},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeUserConfigRecord(user, &encoded), "encode should succeed");
  PersistedUserConfigState decoded;
  Expect(DecodeUserConfigRecord(encoded, &decoded), "decode should succeed");

  int tab_size_count = 0;
  std::string tab_size_value;
  for (const auto& [id, value] : decoded.settings) {
    if (id == "editor.tab_size") {
      ++tab_size_count;
      tab_size_value = value;
    }
  }
  Expect(tab_size_count == 1, "a duplicate setting id must decode to exactly one entry");
  Expect(tab_size_value == "8", "the last value wins for a duplicate setting id");
}

// TD-2026-07-16-36: repeated disabled keybinding/plugin ids dedupe by value on decode
// so stale duplicates cannot inflate downstream resolution.
void TestPersistedStateConfigDedupesDisabledIds() {
  PersistedUserConfigState user{
      .ui_scale = 1.0f,
      .disabled_keybinding_ids = {"a", "a", "b", "a"},
      .disabled_plugin_ids = {"p", "p"},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeUserConfigRecord(user, &encoded), "encode should succeed");
  PersistedUserConfigState decoded;
  Expect(DecodeUserConfigRecord(encoded, &decoded), "decode should succeed");
  Expect(decoded.disabled_keybinding_ids.size() == 2,
         "duplicate disabled keybinding ids dedupe to the unique set");
  Expect(decoded.disabled_plugin_ids.size() == 1, "duplicate disabled plugin ids dedupe");
}

// TD-2026-07-16-37: a persisted commit-draft body over the inline-editor budget must
// fail decode (fail closed) rather than load a huge string into the sidebar editor. A
// normal-sized draft round-trips.
void TestPersistedStateCommitDraftBodyBudget() {
  using microide::workspace::PersistedCommitDraftState;

  // Normal draft round-trips.
  PersistedProjectConfigState ok_config;
  ok_config.commit_draft = PersistedCommitDraftState{
      .head_oid = "abc123", .branch_name = "main", .subject = "fix", .body = "details\n"};
  std::vector<std::byte> ok_encoded;
  Expect(EncodeProjectConfigRecord(ok_config, &ok_encoded), "encode normal draft");
  PersistedProjectConfigState ok_decoded;
  Expect(DecodeProjectConfigRecord(ok_encoded, &ok_decoded), "decode normal draft");
  Expect(ok_decoded.commit_draft.has_value() && ok_decoded.commit_draft->subject == "fix",
         "a normal commit draft round-trips");

  // Oversized body (> 1 MiB) must fail decode closed.
  PersistedProjectConfigState big_config;
  big_config.commit_draft = PersistedCommitDraftState{
      .head_oid = "abc123",
      .branch_name = "main",
      .subject = "fix",
      .body = std::string((1u << 20) + 1, 'x'),
  };
  std::vector<std::byte> big_encoded;
  Expect(EncodeProjectConfigRecord(big_config, &big_encoded),
         "encode still writes the oversized draft bytes");
  PersistedProjectConfigState big_decoded;
  Expect(!DecodeProjectConfigRecord(big_encoded, &big_decoded),
         "decode must fail closed on an over-budget commit-draft body");
}

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


// Encode/decode of the user and project config records over random content:
// setting keys and values with control bytes, multi-byte scalars, an embedded
// NUL, empty strings and long runs; id lists of random length; a random color.
// Every field must come back byte for byte.
void TestPersistedStateConfigRecordsRoundTripRandomContent() {
  std::uint64_t state = 0xC0FFEE1234567890ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kAtoms[] = {"a", "Z", "0", " ", "\n", "\t", "\r", "=", ":", "\"",
                                           "\\", "é", "中", "😀", "\x01", "\x7f", "x.y", ""};
  const auto random_string = [&](std::size_t max_atoms, bool allow_nul) {
    std::string out;
    const std::size_t atoms = next(max_atoms + 1);
    for (std::size_t i = 0; i < atoms; ++i) {
      out += kAtoms[next(std::size(kAtoms))];
    }
    if (allow_nul && next(8) == 0) {
      out.push_back('\0');
      out += "after";
    }
    if (next(16) == 0) {
      out.append(3000, 'w');
    }
    return out;
  };
  for (int round = 0; round < 40; ++round) {
    PersistedUserConfigState user;
    user.ui_scale = 0.5f + static_cast<float>(next(300)) / 100.0f;
    // Keys are distinct valid ids (the decoder dedupes last-writer-wins and drops
    // an invalid id — that policy has its own test below); values are hostile.
    const std::size_t settings = next(6);
    for (std::size_t i = 0; i < settings; ++i) {
      user.settings.emplace_back("k" + std::to_string(round) + "." + std::to_string(i) +
                                     std::string(next(2), '-'),
                                 random_string(6, true));
    }
    // Disabled ids dedupe on decode (covered above), so keep them distinct here.
    const std::size_t keybindings = next(5);
    for (std::size_t i = 0; i < keybindings; ++i) {
      user.disabled_keybinding_ids.push_back(std::to_string(i) + random_string(3, false));
    }
    const std::size_t plugins = next(5);
    for (std::size_t i = 0; i < plugins; ++i) {
      user.disabled_plugin_ids.push_back(std::to_string(i) + random_string(3, false));
    }
    std::vector<std::byte> encoded;
    Expect(EncodeUserConfigRecord(user, &encoded), "user config encodes");
    PersistedUserConfigState decoded;
    const auto dump = [](const std::string& text) {
      std::string out;
      for (const unsigned char c : text) {
        if (c >= 0x20 && c < 0x7f) out.push_back(static_cast<char>(c));
        else { char buf[8]; std::snprintf(buf, sizeof(buf), "\\x%02x", c); out += buf; }
      }
      if (out.size() > 60) out = out.substr(0, 60) + "...(" + std::to_string(text.size()) + ")";
      return out;
    };
    std::string inputs;
    for (const auto& [k, v] : user.settings) inputs += "[" + dump(k) + "=" + dump(v) + "]";
    for (const auto& id : user.disabled_keybinding_ids) inputs += "{kb " + dump(id) + "}";
    for (const auto& id : user.disabled_plugin_ids) inputs += "{pl " + dump(id) + "}";
    Expect(DecodeUserConfigRecord(encoded, &decoded),
           ("user config decodes; inputs=" + inputs + " bytes=" + std::to_string(encoded.size()))
               .c_str());
    Expect(std::fabs(decoded.ui_scale - user.ui_scale) < 0.0001f, "ui scale round-trips");
    Expect(decoded.settings == user.settings,
           ("round " + std::to_string(round) + ": settings round-trip byte for byte").c_str());
    Expect(decoded.disabled_keybinding_ids == user.disabled_keybinding_ids,
           "disabled keybinding ids round-trip");
    Expect(decoded.disabled_plugin_ids == user.disabled_plugin_ids, "disabled plugin ids round-trip");

    PersistedProjectConfigState project;
    project.colorscheme_name = random_string(4, false);
    if (next(2) == 0) {
      project.project_base_color = SDL_Color{static_cast<Uint8>(next(256)), static_cast<Uint8>(next(256)),
                                             static_cast<Uint8>(next(256)), static_cast<Uint8>(next(256))};
    }
    const std::size_t project_settings = next(6);
    for (std::size_t i = 0; i < project_settings; ++i) {
      project.settings.emplace_back("p" + std::to_string(round) + "." + std::to_string(i),
                                    random_string(6, true));
    }
    const std::size_t policies = next(4);
    for (std::size_t i = 0; i < policies; ++i) {
      project.sidebar_policies.push_back(PersistedSidebarViewPolicy{
          .view_id = random_string(3, false), .hidden = next(2) == 0, .order = static_cast<int>(next(50))});
    }
    std::vector<std::byte> encoded_project;
    Expect(EncodeProjectConfigRecord(project, &encoded_project), "project config encodes");
    PersistedProjectConfigState decoded_project;
    Expect(DecodeProjectConfigRecord(encoded_project, &decoded_project), "project config decodes");
    Expect(decoded_project.colorscheme_name == project.colorscheme_name, "colorscheme round-trips");
    Expect(decoded_project.project_base_color.has_value() == project.project_base_color.has_value() &&
               (!project.project_base_color.has_value() ||
                (decoded_project.project_base_color->r == project.project_base_color->r &&
                 decoded_project.project_base_color->g == project.project_base_color->g &&
                 decoded_project.project_base_color->b == project.project_base_color->b &&
                 decoded_project.project_base_color->a == project.project_base_color->a)),
           "base color round-trips");
    Expect(decoded_project.settings == project.settings,
           ("round " + std::to_string(round) + ": project settings round-trip byte for byte").c_str());
    Expect(decoded_project.sidebar_policies.size() == project.sidebar_policies.size(),
           "sidebar policy count round-trips");
    for (std::size_t i = 0; i < project.sidebar_policies.size() &&
                            i < decoded_project.sidebar_policies.size();
         ++i) {
      Expect(decoded_project.sidebar_policies[i].view_id == project.sidebar_policies[i].view_id &&
                 decoded_project.sidebar_policies[i].hidden == project.sidebar_policies[i].hidden &&
                 decoded_project.sidebar_policies[i].order == project.sidebar_policies[i].order,
             "sidebar policy round-trips");
    }
  }
}


// TD-2026-07-16-36 kept malformed ids (control char / space / empty) out of the
// runtime by failing the decode. One malformed setting id now costs that entry
// and nothing else — the validation choke still holds. Decoding used to
// fail the whole record — every setting and disabled id gone for one bad key,
// which is what a later version's tighter id rule does to a config an earlier
// one wrote — and the encoder wrote such an id without complaint.
void TestPersistedStateConfigRecordDropsOnlyTheInvalidSettingId() {
  PersistedUserConfigState user{
      .ui_scale = 1.25f,
      .settings = {{"editor.tab_size", "2"}, {"has space", "x"}, {"", "y"}, {"theme", "dark"}},
      .disabled_keybinding_ids = {"terminal.focus"},
      .disabled_plugin_ids = {"eslint"},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeUserConfigRecord(user, &encoded), "the record encodes");
  PersistedUserConfigState decoded;
  Expect(DecodeUserConfigRecord(encoded, &decoded), "the record decodes despite the bad ids");
  Expect(decoded.settings.size() == 2 && decoded.settings[0].first == "editor.tab_size" &&
             decoded.settings[1].first == "theme",
         "the valid settings survive, in order");
  Expect(decoded.disabled_keybinding_ids.size() == 1 && decoded.disabled_plugin_ids.size() == 1,
         "the disabled ids survive");
  // The encoder never writes an invalid id: the bytes carry no trace of it.
  const std::string bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  Expect(bytes.find("has space") == std::string::npos, "an invalid id is not written");

  PersistedProjectConfigState project{
      .colorscheme_name = "sunrise",
      .settings = {{"bad key", "1"}, {"editor.wrap", "word"}},
  };
  std::vector<std::byte> encoded_project;
  Expect(EncodeProjectConfigRecord(project, &encoded_project), "the project record encodes");
  PersistedProjectConfigState decoded_project;
  Expect(DecodeProjectConfigRecord(encoded_project, &decoded_project), "and decodes");
  Expect(decoded_project.settings.size() == 1 && decoded_project.settings[0].first == "editor.wrap",
         "the project's valid setting survives alone");
}

PersistedProjectSessionState BuildProjectSessionFixture() {
  PersistedEditorTabState editor_tab;
  editor_tab.kind = "editor";
  editor_tab.path = "/tmp/project/src/main.cpp";
  editor_tab.cursor_line = 12;
  editor_tab.cursor_column = 4;
  editor_tab.scroll_line = 8;
  editor_tab.horizontal_scroll = 2;
  editor_tab.dirty_snapshot = true;
  editor_tab.line_ending = microide::util::LineEnding::CRLF;
  editor_tab.buffer_lines = {"line1", "line2"};

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
  PersistedEditorGroupState group_zero;
  group_zero.active_tab_index = 1;
  group_zero.tabs = {editor_tab, compare_tab};
  PersistedEditorTabState second_group_tab;
  second_group_tab.kind = "editor";
  second_group_tab.path = "/tmp/project/src/other.cpp";
  second_group_tab.cursor_line = 3;
  second_group_tab.scroll_line = 1;
  PersistedEditorGroupState group_one;
  group_one.active_tab_index = 0;
  group_one.tabs = {second_group_tab};
  session.groups = {group_zero, group_one};
  session.focused_group_index = 1;
  // A stacked pair, weighted 0.4/0.6 — the tree shape the session must carry now.
  {
    microide::workspace::EditorSplitTree tree;
    tree.InsertLeaf(0, microide::workspace::EditorSplitOrientation::Horizontal, false);
    tree.ResizeDivider(tree.root(), 0, 0.4f);
    session.split_tree = tree.Flatten();
  }
  session.right_pane_visible = true;
  session.right_pane_width = 312.0f;
  session.right_pane_mode = static_cast<std::uint8_t>(microide::workspace::DebugPaneMode::Watch);
  session.expanded_tree_paths = {"dir_a", "dir_a/sub"};
  session.collapsed_tree_paths = {"dir_b"};
  session.selected_tree_path = "dir_a/sub/leaf.txt";
  session.sidebar_scroll_row = 7;
  session.sidebar_view_id = "git";
  return session;
}

// TD-2026-07-16-20: project-session decode caps repeated nested records. A group with
// more tabs than the per-group budget must fail closed rather than materialize them.
// Editor groups are capped at 2, and the third-and-later groups must be skipped BEFORE
// their nested tab payloads are decoded (TD-2026-07-17A-059). A forged session that lists
// an oversized third group (here: one that would fail DecodeEditorGroup because it exceeds
// the per-group tab cap) must still decode successfully with two groups — proving the
// over-cap group's payload was never materialized/validated.
void TestPersistedStateProjectSessionSkipsOverCapGroupsBeforeDecoding() {
  PersistedProjectSessionState session;
  for (std::size_t i = 0; i < microide::workspace::kMaxEditorGroups; ++i) {
    PersistedEditorGroupState group;
    PersistedEditorTabState tab;
    tab.kind = "editor";
    tab.path = "/tmp/keep.txt";
    group.tabs.push_back(std::move(tab));
    session.groups.push_back(std::move(group));
  }
  // One group past the cap, whose payload would FAIL to decode (over the 4096
  // per-group tab cap): if the decoder reached and validated it, the whole record
  // would fail closed.
  PersistedEditorGroupState over_cap_group;
  for (std::size_t i = 0; i < 4097; ++i) {
    PersistedEditorTabState tab;
    tab.kind = "editor";
    tab.path = "/tmp/x" + std::to_string(i) + ".txt";
    over_cap_group.tabs.push_back(std::move(tab));
  }
  session.groups.push_back(std::move(over_cap_group));

  std::vector<std::byte> record;
  Expect(EncodeProjectSessionRecord(session, &record),
         "encoding an over-cap group count should write bytes");
  PersistedProjectSessionState decoded;
  Expect(DecodeProjectSessionRecord(record, &decoded),
         "a group past the cap must be skipped before decoding, not fail the whole record");
  Expect(decoded.groups.size() == microide::workspace::kMaxEditorGroups,
         "only as many editor groups as the editor area can hold are kept");
}

void TestPersistedStateProjectSessionDecodeHonorsTabCap() {
  PersistedProjectSessionState session;
  PersistedEditorGroupState group;
  // One past the per-group tab cap (4096). Each tab is minimal.
  for (std::size_t i = 0; i < 4097; ++i) {
    PersistedEditorTabState tab;
    tab.kind = "editor";
    tab.path = "/tmp/f" + std::to_string(i) + ".txt";
    group.tabs.push_back(std::move(tab));
  }
  session.groups = {std::move(group)};

  std::vector<std::byte> record;
  Expect(EncodeProjectSessionRecord(session, &record),
         "encoding an over-cap session should still write bytes");
  PersistedProjectSessionState decoded;
  Expect(!DecodeProjectSessionRecord(record, &decoded),
         "decoding a session with more tabs than the per-group budget must fail closed");
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
             decoded_session.focused_group_index == 1,
         "project session top-level fields should round-trip");
  Expect(decoded_session.outgoing_base_choice.kind ==
             microide::workspace::OutgoingBaseChoice::Kind::SpecificRef &&
             decoded_session.outgoing_base_choice.custom_ref == "release/2.0",
         "project session outgoing base choice should round-trip");
  microide::workspace::EditorSplitTree decoded_tree;
  Expect(decoded_tree.Load(decoded_session.split_tree) && decoded_tree.leaf_count() == 2 &&
             decoded_tree.node(decoded_tree.root()).orientation ==
                 microide::workspace::EditorSplitOrientation::Horizontal &&
             std::fabs(decoded_tree.node(decoded_tree.root()).weights[0] - 0.4f) < 0.0001f,
         "project session group split layout should round-trip");
  Expect(decoded_session.groups.size() == 2 &&
             decoded_session.groups[0].tabs.size() == 2 &&
             decoded_session.groups[0].active_tab_index == 1 &&
             decoded_session.groups[0].tabs[0].path == "/tmp/project/src/main.cpp" &&
             decoded_session.groups[0].tabs[0].scroll_line == 8 &&
             decoded_session.groups[0].tabs[1].compare_right_ref == "WORKTREE" &&
             decoded_session.groups[1].tabs.size() == 1 &&
             decoded_session.groups[1].tabs[0].path == "/tmp/project/src/other.cpp",
         "project session groups should round-trip");
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

  // Regression: duplicate roots (a corrupt session) are deduped at decode so
  // restore does not open duplicate project tabs.
  PersistedWorkspaceSessionState dup{
      .project_roots = {"/tmp/p", "/tmp/p", "/tmp/./p", "/tmp/q"},
      .active_project_index = 0,
  };
  std::vector<std::byte> dup_record;
  Expect(EncodeWorkspaceSessionRecord(dup, &dup_record), "dup encode should succeed");
  PersistedWorkspaceSessionState decoded_dup;
  Expect(DecodeWorkspaceSessionRecord(dup_record, &decoded_dup), "dup decode should succeed");
  Expect(decoded_dup.project_roots.size() == 2,
         "duplicate/equivalent roots collapse to unique entries at decode");
}

void TestPersistedStateProjectSessionRoundTripsTreeState() {
  PersistedProjectSessionState session = BuildProjectSessionFixture();
  std::vector<std::byte> record;
  Expect(EncodeProjectSessionRecord(session, &record),
         "project session encode should succeed");
  PersistedProjectSessionState decoded;
  Expect(DecodeProjectSessionRecord(record, &decoded),
         "project session decode should succeed");
  Expect(decoded.expanded_tree_paths.size() == 2 &&
             decoded.expanded_tree_paths[0] == "dir_a" &&
             decoded.expanded_tree_paths[1] == "dir_a/sub",
         "expanded tree paths should round-trip in order");
  Expect(decoded.collapsed_tree_paths.size() == 1 &&
             decoded.collapsed_tree_paths[0] == "dir_b",
         "collapsed tree paths should round-trip");
  Expect(decoded.selected_tree_path == "dir_a/sub/leaf.txt" &&
             decoded.sidebar_scroll_row == 7 && decoded.sidebar_view_id == "git",
         "selected node, sidebar scroll, and active view should round-trip");

  // An older session file lacking the tree/sidebar tags decodes to the empty/zero
  // defaults rather than failing (additive backward compatibility).
  PersistedProjectSessionState legacy = BuildProjectSessionFixture();
  legacy.expanded_tree_paths.clear();
  legacy.collapsed_tree_paths.clear();
  legacy.selected_tree_path.clear();
  legacy.sidebar_scroll_row = 0;
  legacy.sidebar_view_id.clear();
  std::vector<std::byte> legacy_record;
  Expect(EncodeProjectSessionRecord(legacy, &legacy_record),
         "legacy project session encode should succeed");
  PersistedProjectSessionState decoded_legacy;
  Expect(DecodeProjectSessionRecord(legacy_record, &decoded_legacy),
         "legacy project session decode should succeed");
  Expect(decoded_legacy.expanded_tree_paths.empty() &&
             decoded_legacy.collapsed_tree_paths.empty() &&
             decoded_legacy.selected_tree_path.empty() &&
             decoded_legacy.sidebar_scroll_row == 0 &&
             decoded_legacy.sidebar_view_id.empty(),
         "absent tree/sidebar tags decode to defaults");
}

// A1 regression: compare `review_mode` and `staging_view` must survive a binary
// session round-trip. Before the fix the tab schema had no tags for these fields,
// so EncodeEditorTab/DecodeEditorTab dropped them and a rebuilt CompareTabState
// silently reverted to the default WorkingTree/Combined lens. This asserts the
// *rebuilt* CompareTabState (built exactly as RestoreSessionState builds it from
// the decoded persisted tab), not only the decoded intermediate struct.
void TestPersistedStateCompareTabReviewFieldsRoundTrip() {
  using microide::compare::CompareReviewMode;
  using microide::compare::WorkingTreeStagingView;
  using microide::workspace::CompareTabState;

  // Mirror of the RestoreSessionState mapping (WorkspacePersistenceCoordinator
  // Session.cpp): translate the persisted string labels back into a rebuilt
  // CompareTabState's review_mode / staging_view.
  const auto rebuild_compare = [](const PersistedEditorTabState& tab) {
    CompareTabState compare_state;  // defaults: WorkingTree + Combined
    if (!tab.compare_review_mode.empty()) {
      if (tab.compare_review_mode == "commit") {
        compare_state.review_mode = CompareReviewMode::Commit;
      } else if (tab.compare_review_mode == "branch") {
        compare_state.review_mode = CompareReviewMode::Branch;
      } else if (tab.compare_review_mode == "conflict") {
        compare_state.review_mode = CompareReviewMode::Conflict;
      } else {
        compare_state.review_mode = CompareReviewMode::WorkingTree;
      }
    }
    if (!tab.compare_staging_view.empty()) {
      if (tab.compare_staging_view == "staged") {
        compare_state.staging_view = WorkingTreeStagingView::Staged;
      } else if (tab.compare_staging_view == "unstaged") {
        compare_state.staging_view = WorkingTreeStagingView::Unstaged;
      } else {
        compare_state.staging_view = WorkingTreeStagingView::Combined;
      }
    }
    return compare_state;
  };

  PersistedEditorTabState compare_tab;
  compare_tab.kind = "compare";
  compare_tab.compare_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_left_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_right_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_commit_hash = "abcdef123456";
  compare_tab.compare_commit_short_hash = "abcdef1";
  compare_tab.compare_right_ref = "WORKTREE";
  compare_tab.compare_review_mode = "commit";   // non-default (default is WorkingTree)
  compare_tab.compare_staging_view = "staged";  // non-default (default is Combined)

  PersistedEditorGroupState group;
  group.active_tab_index = 0;
  group.tabs = {compare_tab};
  PersistedProjectSessionState session;
  session.groups = {group};

  std::vector<std::byte> record;
  Expect(EncodeProjectSessionRecord(session, &record),
         "compare-review session encode should succeed");
  PersistedProjectSessionState decoded;
  Expect(DecodeProjectSessionRecord(record, &decoded),
         "compare-review session decode should succeed");
  Expect(decoded.groups.size() == 1 && decoded.groups[0].tabs.size() == 1,
         "compare-review session should round-trip a single compare tab");

  const PersistedEditorTabState& decoded_tab = decoded.groups[0].tabs[0];
  Expect(decoded_tab.compare_review_mode == "commit",
         "decoded persisted tab should preserve compare_review_mode");
  Expect(decoded_tab.compare_staging_view == "staged",
         "decoded persisted tab should preserve compare_staging_view");

  const CompareTabState rebuilt = rebuild_compare(decoded_tab);
  Expect(rebuilt.review_mode == CompareReviewMode::Commit,
         "rebuilt CompareTabState should preserve the commit review mode");
  Expect(rebuilt.staging_view == WorkingTreeStagingView::Staged,
         "rebuilt CompareTabState should preserve the staged staging view");

  // Backward compatibility: an older record with no review/staging tags must
  // decode to empty strings so the rebuilt tab keeps the struct defaults instead
  // of failing to decode.
  PersistedEditorTabState legacy_tab = compare_tab;
  legacy_tab.compare_review_mode.clear();
  legacy_tab.compare_staging_view.clear();
  PersistedEditorGroupState legacy_group;
  legacy_group.tabs = {legacy_tab};
  PersistedProjectSessionState legacy_session;
  legacy_session.groups = {legacy_group};
  std::vector<std::byte> legacy_record;
  Expect(EncodeProjectSessionRecord(legacy_session, &legacy_record),
         "legacy compare session (no review tags) should encode");
  PersistedProjectSessionState decoded_legacy;
  Expect(DecodeProjectSessionRecord(legacy_record, &decoded_legacy),
         "legacy compare session should decode");
  const PersistedEditorTabState& decoded_legacy_tab = decoded_legacy.groups[0].tabs[0];
  Expect(decoded_legacy_tab.compare_review_mode.empty() &&
             decoded_legacy_tab.compare_staging_view.empty(),
         "absent review/staging tags decode to empty strings");
  const CompareTabState rebuilt_legacy = rebuild_compare(decoded_legacy_tab);
  Expect(rebuilt_legacy.review_mode == CompareReviewMode::WorkingTree &&
             rebuilt_legacy.staging_view == WorkingTreeStagingView::Combined,
         "a legacy record rebuilds to the default WorkingTree/Combined lens");
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
  Expect(decoded.groups.size() == session.groups.size(),
         "legacy chat records should not alter group state");
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
  Expect(writer.WriteU32(2), "project session schema payload should encode");
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
  state.function_breakpoints.push_back(PersistedFunctionBreakpoint{.name = "main"});
  state.function_breakpoints.push_back(PersistedFunctionBreakpoint{
      .name = "compute", .enabled = false, .condition = std::string("n > 0")});
  state.exception_filter_conditions = {{"throw", "x == 2"}};

  std::vector<std::byte> encoded;
  Expect(EncodeDebugStateRecord(state, &encoded), "debug state should encode");

  PersistedDebugState decoded;
  Expect(DecodeDebugStateRecord(encoded, &decoded), "debug state should decode");
  Expect(decoded.function_breakpoints.size() == 2 &&
             decoded.function_breakpoints[0].name == "main" &&
             decoded.function_breakpoints[1].name == "compute" &&
             decoded.function_breakpoints[1].enabled == false &&
             decoded.function_breakpoints[1].condition.has_value() &&
             *decoded.function_breakpoints[1].condition == "n > 0",
         "function breakpoints round-trip in order with their fields");
  Expect(decoded.exception_filter_conditions.size() == 1 &&
             decoded.exception_filter_conditions.at("throw") == "x == 2",
         "per-filter exception conditions round-trip");
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

// Watch expressions, conditions, and log messages can contain quotes, newlines,
// and non-ASCII — the length-prefixed binary format must round-trip them verbatim
// (a previous concern was special characters corrupting the record).
void TestPersistedStateDebugStateSpecialCharacters() {
  PersistedDebugState state;
  PersistedFileBreakpoints file;
  file.path = "/proj/π/main.py";  // non-ASCII path component
  file.breakpoints.push_back(PersistedBreakpoint{
      .line = 7,
      .enabled = true,
      .condition = std::string("s == \"a\\tb\" && n > 0\nx"),  // quotes, tab, newline
      .log_message = std::string("héllo {x}\nline2"),          // unicode + newline
  });
  state.files.push_back(std::move(file));
  state.watch_expressions = {"\"quoted\"", "a\nb", "ünïcödé", "tab\tsep", ""};

  std::vector<std::byte> encoded;
  Expect(EncodeDebugStateRecord(state, &encoded), "special-character debug state should encode");

  PersistedDebugState decoded;
  Expect(DecodeDebugStateRecord(encoded, &decoded), "special-character debug state should decode");
  Expect(decoded.watch_expressions.size() == 5 && decoded.watch_expressions[0] == "\"quoted\"" &&
             decoded.watch_expressions[1] == "a\nb" && decoded.watch_expressions[2] == "ünïcödé" &&
             decoded.watch_expressions[3] == "tab\tsep" && decoded.watch_expressions[4].empty(),
         "watch expressions round-trip special characters verbatim");
  Expect(decoded.files.size() == 1 &&
             decoded.files[0].path == std::filesystem::path("/proj/π/main.py"),
         "a non-ASCII path round-trips");
  const auto& bp = decoded.files[0].breakpoints[0];
  Expect(bp.condition.has_value() && *bp.condition == "s == \"a\\tb\" && n > 0\nx",
         "a condition with quotes/tab/newline round-trips verbatim");
  Expect(bp.log_message.has_value() && *bp.log_message == "héllo {x}\nline2",
         "a log message with unicode + newline round-trips verbatim");
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

// J16 regression: the debug-state decoder must bound each repeated collection so
// a corrupt or hostile state file cannot force huge allocations before the
// project UI opens. A record whose watch-expression count exceeds the per-section
// cap (4096) must fail cleanly rather than materialize every entry.
void TestPersistedStateDebugStateEnforcesDecodeCaps() {
  PersistedDebugState state;
  state.selected_launch_config_index = 0;
  // 5000 > the 4096 per-section cap.
  for (int i = 0; i < 5000; ++i) {
    state.watch_expressions.push_back("w");
  }
  std::vector<std::byte> encoded;
  Expect(EncodeDebugStateRecord(state, &encoded),
         "an over-cap debug record should still encode (writer is uncapped)");
  PersistedDebugState decoded;
  Expect(!DecodeDebugStateRecord(encoded, &decoded),
         "a record exceeding the per-section watch cap should fail to decode");

  // A record right at the cap still decodes so legitimate state is not rejected.
  PersistedDebugState at_cap;
  at_cap.selected_launch_config_index = 0;
  for (int i = 0; i < 4096; ++i) {
    at_cap.watch_expressions.push_back("w");
  }
  std::vector<std::byte> at_cap_encoded;
  Expect(EncodeDebugStateRecord(at_cap, &at_cap_encoded), "at-cap debug record should encode");
  PersistedDebugState at_cap_decoded;
  Expect(DecodeDebugStateRecord(at_cap_encoded, &at_cap_decoded),
         "a record at the per-section cap should still decode cleanly");
  Expect(at_cap_decoded.watch_expressions.size() == 4096,
         "at-cap watch expressions should all decode");
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

void TestPersistedStateMruRecordRoundTrip() {
  PersistedMruState mru{
      .recent_project_roots = {"/home/u/proj-a", "/home/u/proj-b"},
      .recent_files = {{"/home/u/proj-a/src/main.cpp", "/home/u/proj-a"},
                       {"/home/u/proj-b/README.md", "/home/u/proj-b"}},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeMruRecord(mru, &encoded), "mru record encode should succeed");
  PersistedMruState decoded;
  Expect(DecodeMruRecord(encoded, &decoded), "mru record decode should succeed");
  Expect(decoded.recent_project_roots.size() == 2 &&
             decoded.recent_project_roots[0] == std::filesystem::path("/home/u/proj-a"),
         "mru project roots should round-trip in order");
  Expect(decoded.recent_files.size() == 2 &&
             decoded.recent_files[1].path == std::filesystem::path("/home/u/proj-b/README.md") &&
             decoded.recent_files[1].project_root == std::filesystem::path("/home/u/proj-b"),
         "mru recent files should round-trip with their project root");
}

// J17 regression: the MRU decoder must enforce RecentsService::MaxProjects() /
// MaxFiles() so a malformed recents file cannot allocate every entry during
// startup only for the service to discard almost all of them. Records are
// newest-first, so the decoder keeps the leading cap-many entries and drops the
// rest.
void TestPersistedStateMruRecordEnforcesCaps() {
  using microide::workspace::RecentsService;
  PersistedMruState mru;
  const std::size_t project_overflow = RecentsService::MaxProjects() * 4;
  const std::size_t file_overflow = RecentsService::MaxFiles() * 4;
  for (std::size_t i = 0; i < project_overflow; ++i) {
    mru.recent_project_roots.push_back(std::filesystem::path("/home/u/proj") /
                                       std::to_string(i));
  }
  for (std::size_t i = 0; i < file_overflow; ++i) {
    mru.recent_files.push_back(
        {std::filesystem::path("/home/u/proj/file") / std::to_string(i), "/home/u/proj"});
  }
  std::vector<std::byte> encoded;
  Expect(EncodeMruRecord(mru, &encoded), "over-cap mru record should encode (writer is uncapped)");
  PersistedMruState decoded;
  Expect(DecodeMruRecord(encoded, &decoded), "over-cap mru record should still decode");
  Expect(decoded.recent_project_roots.size() == RecentsService::MaxProjects(),
         "mru decode should cap project roots at MaxProjects()");
  Expect(decoded.recent_files.size() == RecentsService::MaxFiles(),
         "mru decode should cap recent files at MaxFiles()");
  // The retained entries are the leading (newest-first) ones.
  Expect(decoded.recent_project_roots.front() == std::filesystem::path("/home/u/proj/0"),
         "mru decode should keep the newest project roots");
  Expect(decoded.recent_files.front().path == std::filesystem::path("/home/u/proj/file/0"),
         "mru decode should keep the newest recent files");
}

void TestPersistedStateMruRecordRequiresSchema() {
  // A stream that omits the schema record must be rejected (mirrors debug state).
  PersistedMruState empty;
  std::vector<std::byte> encoded;
  Expect(EncodeMruRecord(empty, &encoded), "empty mru record encode should succeed");
  Expect(!encoded.empty(), "encoded mru record should at least carry the schema tag");
  PersistedMruState decoded;
  Expect(DecodeMruRecord(std::span<const std::byte>{}, &decoded) == false,
         "mru decode should fail when the schema record is missing");
}

void TestPersistedStateProjectConfigMigratesLegacyEditorPrefTags() {
  // Retired typed tags 2 (EditorTabSize, U32) / 3 (EditorIndentWidth, U32) /
  // 4 (EditorSoftTabs, Bool) predate the layered `Setting` records. A config that
  // carried indentation ONLY in these tags must migrate the values into the settings
  // layer on decode instead of silently reverting to the spec default.
  PersistedProjectConfigState base{
      .colorscheme_name = {},
      .project_base_color = std::nullopt,
      .settings = {},
      .sidebar_policies = {},
      .commit_draft = std::nullopt,
      .branch_review = {},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeProjectConfigRecord(base, &encoded),
         "legacy-tag base project config should encode");

  std::vector<std::byte> payload;
  microide::persistence::PrimitiveWriter writer(&payload);
  Expect(writer.WriteU32(8), "legacy tab-size payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(2, payload, &encoded),
         "legacy EditorTabSize tag should append");
  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteU32(3), "legacy indent-width payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(3, payload, &encoded),
         "legacy EditorIndentWidth tag should append");
  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteBool(true), "legacy soft-tabs payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(4, payload, &encoded),
         "legacy EditorSoftTabs tag should append");

  PersistedProjectConfigState decoded;
  Expect(DecodeProjectConfigRecord(encoded, &decoded),
         "project config with legacy editor-pref tags should decode");
  const auto find = [&](std::string_view id) -> const std::string* {
    for (const auto& [key, value] : decoded.settings) {
      if (key == id) {
        return &value;
      }
    }
    return nullptr;
  };
  const std::string* tab_size = find("editor.tab_size");
  const std::string* indent_width = find("editor.indent_width");
  const std::string* soft_tabs = find("editor.soft_tabs");
  Expect(tab_size != nullptr && *tab_size == "8",
         "legacy EditorTabSize should migrate into editor.tab_size");
  Expect(indent_width != nullptr && *indent_width == "3",
         "legacy EditorIndentWidth should migrate into editor.indent_width");
  Expect(soft_tabs != nullptr && *soft_tabs == "true",
         "legacy EditorSoftTabs should migrate into editor.soft_tabs");

  // A modern `Setting` record for the same id wins: the legacy tag must not clobber it.
  PersistedProjectConfigState with_setting{
      .colorscheme_name = {},
      .project_base_color = std::nullopt,
      .settings = {{"editor.tab_size", "2"}},
      .sidebar_policies = {},
      .commit_draft = std::nullopt,
      .branch_review = {},
  };
  std::vector<std::byte> encoded_with_setting;
  Expect(EncodeProjectConfigRecord(with_setting, &encoded_with_setting),
         "project config with modern setting should encode");
  payload.clear();
  writer = microide::persistence::PrimitiveWriter(&payload);
  Expect(writer.WriteU32(8), "legacy tab-size payload should encode");
  Expect(microide::persistence::AppendTaggedRecord(2, payload, &encoded_with_setting),
         "legacy EditorTabSize tag should append after modern setting");
  PersistedProjectConfigState decoded_with_setting;
  Expect(DecodeProjectConfigRecord(encoded_with_setting, &decoded_with_setting),
         "project config with both should decode");
  int tab_size_entries = 0;
  const std::string* resolved_tab_size = nullptr;
  for (const auto& [key, value] : decoded_with_setting.settings) {
    if (key == "editor.tab_size") {
      ++tab_size_entries;
      resolved_tab_size = &value;
    }
  }
  Expect(tab_size_entries == 1 && resolved_tab_size != nullptr && *resolved_tab_size == "2",
         "a modern editor.tab_size setting should win over the legacy tag");
}

// Regression: when the primary state file is present but corrupt, a recovery
// from the `.bak` must NOT auto-overwrite the still-recoverable primary with
// stale backup state — only a genuine user mutation may heal it.
void TestPersistenceServiceGuardsCorruptPrimaryFromBackupOverwrite() {
  using microide::persistence::PersistedRecordReader;
  using microide::persistence::PersistedRecordWriter;
  using microide::workspace::PersistenceService;

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "user.config";
  PersistenceService service;

  // Save v1 then v2: the second save rotates v1 into the `.bak` sibling.
  PersistedUserConfigState v1{.ui_scale = 1.0f, .settings = {{"theme", "backup-good"}}};
  Expect(service.SaveUserConfig(path, v1), "first save should succeed");
  // Land v1 before saving v2. Saves coalesce per path, so back-to-back saves are
  // ONE write of the latest body -- and this test needs two real writes for the
  // first to be rotated into the backup by the second.
  service.FlushPendingWrites();
  PersistedUserConfigState v2{.ui_scale = 1.0f, .settings = {{"theme", "current"}}};
  Expect(service.SaveUserConfig(path, v2), "second save should rotate v1 into the backup");
  service.FlushPendingWrites();
  Expect(std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "a backup of the prior primary should exist");

  // Corrupt the primary in place; the valid backup remains.
  WriteFile(path, "corrupt-not-a-valid-record-header");

  // Load recovers the backup (v1) and arms the overwrite guard for this path.
  PersistedUserConfigState loaded;
  Expect(service.LoadUserConfig(path, &loaded), "load should recover from the backup");
  Expect(loaded.settings.size() == 1 && loaded.settings[0].second == "backup-good",
         "the recovered state should be the backup contents (v1)");

  // An immediate save of the unchanged recovered state must be suppressed so the
  // still-recoverable corrupt primary is preserved for manual recovery.
  Expect(service.SaveUserConfig(path, loaded), "no-op save should report success");
  service.FlushPendingWrites();
  {
    const auto reread = PersistedRecordReader::ReadFile(path);
    Expect(reread.has_value() && reread->used_backup,
           "the primary must still be corrupt (the stale overwrite was suppressed)");
  }

  // A genuine mutation lifts the guard: the save now heals the primary on disk.
  loaded.settings[0].second = "user-edited";
  Expect(service.SaveUserConfig(path, loaded), "a mutated save should write through");
  service.FlushPendingWrites();
  {
    const auto reread = PersistedRecordReader::ReadFile(path);
    Expect(reread.has_value() && !reread->used_backup,
           "the primary should be valid again after a mutated save");
    PersistedUserConfigState healed;
    Expect(DecodeUserConfigRecord(reread->body, &healed), "the healed primary should decode");
    Expect(healed.settings.size() == 1 && healed.settings[0].second == "user-edited",
           "the healed primary should hold the mutated state, not the stale backup");
  }
}

// A save whose encoded body already sits on disk is skipped -- these writes are
// durable (temp + fsync + backup rotation + rename) and run on the shell thread,
// so re-writing identical bytes is a stall for nothing. The two things that skip
// must never break: a record that changed still writes, and a record whose file
// went away underneath the memo is recreated rather than assumed present.
void TestPersistenceServiceSkipsRewritingIdenticalState() {
  using microide::persistence::PersistedRecordReader;
  using microide::persistence::PersistedRecordWriter;
  using microide::workspace::PersistenceService;

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "user.config";
  PersistenceService service;

  PersistedUserConfigState state{.ui_scale = 1.0f, .settings = {{"theme", "dark"}}};
  Expect(service.SaveUserConfig(path, state), "first save should succeed");
  // Saves are applied on a background worker; this test asserts on-disk effects,
  // so it waits for them rather than racing the writer.
  service.FlushPendingWrites();
  Expect(std::filesystem::exists(path), "the record should exist after the first save");
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "a first save has no prior primary to rotate into a backup");

  // Identical state: skipped, so no backup rotation happens. That absence is the
  // observable proof the durable write did not run.
  Expect(service.SaveUserConfig(path, state), "an unchanged save should report success");
  service.FlushPendingWrites();
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "an unchanged save must not rewrite the record (no backup rotation)");

  // Changed state writes through, and now there is a prior primary to rotate.
  state.settings[0].second = "light";
  Expect(service.SaveUserConfig(path, state), "a changed save should write through");
  service.FlushPendingWrites();
  Expect(std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "a real write rotates the previous primary into the backup");
  {
    const auto reread = PersistedRecordReader::ReadFile(path);
    PersistedUserConfigState reloaded;
    Expect(reread.has_value() && DecodeUserConfigRecord(reread->body, &reloaded),
           "the rewritten primary should decode");
    Expect(reloaded.settings.size() == 1 && reloaded.settings[0].second == "light",
           "the rewritten primary should hold the changed state");
  }

  // The file disappearing must invalidate the memo: an identical save recreates it.
  std::error_code error;
  std::filesystem::remove(path, error);
  Expect(!std::filesystem::exists(path), "the record should be gone before the recreate check");
  Expect(service.SaveUserConfig(path, state), "an identical save should recreate a missing record");
  service.FlushPendingWrites();
  Expect(std::filesystem::exists(path),
         "a save must not be skipped against a record that no longer exists");

  // A load also seeds the memo, so the very first save after a restore of
  // unchanged state is skipped too.
  PersistenceService fresh_service;
  PersistedUserConfigState loaded;
  Expect(fresh_service.LoadUserConfig(path, &loaded), "load should succeed");
  std::filesystem::remove(PersistedRecordWriter::BackupPathFor(path), error);
  Expect(fresh_service.SaveUserConfig(path, loaded), "an unchanged post-load save should succeed");
  fresh_service.FlushPendingWrites();
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "a save of exactly what was loaded must not rewrite the record");
}

void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedStateRecord/PersistenceServiceSkipsRewritingIdenticalState",
          TestPersistenceServiceSkipsRewritingIdenticalState);
  AddTest(tests, "PersistedStateRecord/PersistenceServiceGuardsCorruptPrimaryFromBackupOverwrite",
          TestPersistenceServiceGuardsCorruptPrimaryFromBackupOverwrite);
  AddTest(tests, "PersistedStateRecord/ProjectConfigMigratesLegacyEditorPrefTags",
          TestPersistedStateProjectConfigMigratesLegacyEditorPrefTags);
  AddTest(tests, "PersistedStateRecord/MruRoundTrip", TestPersistedStateMruRecordRoundTrip);
  AddTest(tests, "PersistedStateRecord/MruEnforcesCaps",
          TestPersistedStateMruRecordEnforcesCaps);
  AddTest(tests, "PersistedStateRecord/MruRequiresSchema",
          TestPersistedStateMruRecordRequiresSchema);
  AddTest(tests, "PersistedStateRecord/RejectsAdversarialLengthWithoutOom",
          TestPersistedStateRejectsAdversarialLengthWithoutOom);
  AddTest(tests, "PersistedStateRecord/UserAndProjectConfigRoundTrip",
          TestPersistedStateUserAndProjectConfigRecordRoundTrip);
  AddTest(tests, "PersistedStateRecord/ConfigRecordDropsOnlyTheInvalidSettingId",
          TestPersistedStateConfigRecordDropsOnlyTheInvalidSettingId);
  AddTest(tests, "PersistedStateRecord/ConfigRecordsRoundTripRandomContent",
          TestPersistedStateConfigRecordsRoundTripRandomContent);
  AddTest(tests, "PersistedState/ConfigDedupesDuplicateSettingIds",
          TestPersistedStateConfigDedupesDuplicateSettingIds);
  AddTest(tests, "PersistedState/ConfigDedupesDisabledIds",
          TestPersistedStateConfigDedupesDisabledIds);
  AddTest(tests, "PersistedState/CommitDraftBodyBudget",
          TestPersistedStateCommitDraftBodyBudget);
  AddTest(tests, "PersistedStateRecord/ProjectSessionDecodeHonorsTabCap",
          TestPersistedStateProjectSessionDecodeHonorsTabCap);
  AddTest(tests, "PersistedStateRecord/ProjectSessionSkipsOverCapGroupsBeforeDecoding",
          TestPersistedStateProjectSessionSkipsOverCapGroupsBeforeDecoding);
  AddTest(tests, "PersistedStateRecord/ProjectSessionRoundTripOmitsChatRegistry",
          TestPersistedStateProjectSessionRoundTripOmitsChatRegistry);
  AddTest(tests, "PersistedStateRecord/ProjectSessionAcceptsLegacyChatRegistryTag",
          TestPersistedStateProjectSessionAcceptsLegacyChatRegistryTag);
  AddTest(tests, "PersistedStateRecord/ProjectSessionRoundTripsTreeState",
          TestPersistedStateProjectSessionRoundTripsTreeState);
  AddTest(tests, "PersistedStateRecord/DecodersSkipUnknownTags",
          TestPersistedStateRecordDecodersSkipUnknownTags);
  AddTest(tests, "PersistedStateRecord/ProjectSessionDefaultsMissingOutgoingBaseChoiceToAuto",
          TestPersistedStateProjectSessionDefaultsMissingOutgoingBaseChoiceToAuto);
  AddTest(tests, "PersistedStateRecord/DebugStateRoundTrip", TestPersistedStateDebugStateRoundTrip);
  AddTest(tests, "PersistedStateRecord/DebugStateBackwardCompatNoWatch",
          TestPersistedStateDebugStateBackwardCompatNoWatch);
  AddTest(tests, "PersistedStateRecord/DebugStateSpecialCharacters",
          TestPersistedStateDebugStateSpecialCharacters);
  AddTest(tests, "PersistedStateRecord/DebugStateRequiresSchema",
          TestPersistedStateDebugStateRequiresSchema);
  AddTest(tests, "PersistedStateRecord/DebugStateEnforcesDecodeCaps",
          TestPersistedStateDebugStateEnforcesDecodeCaps);
  AddTest(tests, "PersistedStateRecord/CompareTabReviewFieldsRoundTrip",
          TestPersistedStateCompareTabReviewFieldsRoundTrip);
}

}  // namespace microide::tests
