#include "TestSupport.h"

#include "workspace/state/WorkspaceProjectState.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::EditorGroup;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::TabEntry;

void TestFocusedGroupAccessorsAgreeAndDoNotMutate() {
  ProjectWorkspaceState state;
  Expect(state.editor_groups.size() == 1, "a project starts with one editor group");
  state.editor_groups.emplace_back();
  Expect(state.editor_groups.size() == 2, "a second group can be added");

  state.focused_group_index = 7;

  const ProjectWorkspaceState& const_state = state;
  EditorGroup& mutable_group = state.focused_group();
  const EditorGroup& const_group = const_state.focused_group();
  Expect(&mutable_group == &const_state.editor_groups[0],
         "a stale index clamps to group 0 on the mutable read");
  Expect(&const_group == &const_state.editor_groups[0],
         "a stale index clamps to group 0 on the const read");
  Expect(&mutable_group == &const_group,
         "const and non-const focused_group() resolve to the same group");

  Expect(state.focused_group_index == 7, "focused_group() does not mutate the index");
  Expect(state.editor_groups.size() == 2, "focused_group() does not grow editor_groups");

  state.focused_group_index = 1;
  Expect(&state.focused_group() == &state.editor_groups[1],
         "a valid index resolves to the matching group");
}

void TestEditorGroupActiveTabHelpers() {
  EditorGroup group;
  Expect(!group.has_active_tab(), "an empty group has no active tab");

  group.open_tabs.emplace_back();
  group.open_tabs.emplace_back();
  group.active_tab_index = 1;
  Expect(group.has_active_tab(), "an in-bounds active index reports an active tab");
  Expect(&group.active_tab() == &group.open_tabs[1], "active_tab() returns the indexed tab");

  group.active_tab_index = 5;
  Expect(!group.has_active_tab(), "an out-of-bounds active index reports no active tab");
}

}

void RegisterEditorGroupStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorGroupState/FocusedGroupAccessorsAgreeAndDoNotMutate",
          TestFocusedGroupAccessorsAgreeAndDoNotMutate);
  AddTest(tests, "EditorGroupState/ActiveTabHelpers", TestEditorGroupActiveTabHelpers);
}

}
