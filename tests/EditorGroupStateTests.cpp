#include "TestSupport.h"

#include "workspace/WorkspaceProjectState.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::EditorGroup;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::TabEntry;

// focused_group() must be a side-effect-free read: a stale focused_group_index
// is clamped on read rather than silently mutated, and the const and non-const
// overloads must resolve to the same group. The old non-const overload rewrote
// focused_group_index (and could grow editor_groups), so const/non-const callers
// could disagree. This guards that unification.
void TestFocusedGroupAccessorsAgreeAndDoNotMutate() {
  ProjectWorkspaceState state;
  Expect(state.editor_groups.size() == 1, "a project starts with one editor group");
  state.editor_groups.emplace_back();
  Expect(state.editor_groups.size() == 2, "a second group can be added");

  // Drive the index out of bounds the way a botched restore might.
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

  // The read must not have healed the index as a side effect.
  Expect(state.focused_group_index == 7, "focused_group() does not mutate the index");
  Expect(state.editor_groups.size() == 2, "focused_group() does not grow editor_groups");

  // A valid index resolves to exactly that group.
  state.focused_group_index = 1;
  Expect(&state.focused_group() == &state.editor_groups[1],
         "a valid index resolves to the matching group");
}

// has_active_tab()/active_tab() back the de-duplicated active-tab lookups that
// replaced repeated focused_group().open_tabs[active_tab_index] chains.
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

}  // namespace

void RegisterEditorGroupStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorGroupState/FocusedGroupAccessorsAgreeAndDoNotMutate",
          TestFocusedGroupAccessorsAgreeAndDoNotMutate);
  AddTest(tests, "EditorGroupState/ActiveTabHelpers", TestEditorGroupActiveTabHelpers);
}

}  // namespace microide::tests
