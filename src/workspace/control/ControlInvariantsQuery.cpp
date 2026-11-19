#include "workspace/control/ControlInvariantsQuery.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/EditorSplitTree.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {
namespace {

// The tab a violation is about. Its title goes in quotes: a driver that keys a
// finding on the detail (tools/qa/findings.py) masks what is quoted, and
// unquoted the one broken rule was a finding per file it was seen on.
std::string TabName(std::size_t group, std::size_t tab, const TabEntry& entry) {
  return "group " + std::to_string(group) + " tab " + std::to_string(tab) + " '" + entry.title + "'";
}

// A position inside `view`'s buffer: an existing line, a column no further than
// its end.
bool Inside(const editor::TextViewport& view, std::size_t line, std::size_t column) {
  return line < view.line_count() && column <= view.lines().LineLength(line);
}

std::string At(std::size_t line, std::size_t column) {
  return std::to_string(line + 1) + ":" + std::to_string(column + 1);
}

// How a position misses `view`'s buffer: no such line, or a column past the end
// of a line it has. Two different faults (a caret kept across a reload that
// shortened the file, against one kept across an edit of its line), and "outside
// its 29-line buffer" for a caret on line 1 said neither.
std::string Outside(const editor::TextViewport& view, std::size_t line, std::size_t column) {
  if (line >= view.line_count()) {
    return At(line, column) + ", past the last of its " + std::to_string(view.line_count()) + " lines";
  }
  return At(line, column) + ", past the end of that line (" + std::to_string(view.lines().LineLength(line)) +
         " long)";
}

}  // namespace

WorkspaceInvariantReport CheckWorkspaceInvariants(const ProjectWorkspaceState& project,
                                                  const ControlInvariantsQueryOptions& options) {
  WorkspaceInvariantReport report;
  const auto violate = [&report](std::string rule, std::string detail) {
    report.violations.push_back({std::move(rule), std::move(detail)});
  };
  const std::vector<EditorGroup>& groups = project.editor_groups;
  report.groups = groups.size();
  if (groups.empty()) {
    violate("groups", "the project has no editor group");
  }
  if (groups.size() > kMaxEditorGroups) {
    violate("groups", std::to_string(groups.size()) + " editor groups, over the cap of " +
                          std::to_string(kMaxEditorGroups));
  }
  if (project.editor_split.leaf_count() != groups.size()) {
    violate("groups", "the split tree has " + std::to_string(project.editor_split.leaf_count()) +
                          " leaves for " + std::to_string(groups.size()) + " editor groups");
  }
  if (!groups.empty() && project.focused_group_index >= groups.size()) {
    violate("groups", "the focused group is " + std::to_string(project.focused_group_index) + " of " +
                          std::to_string(groups.size()));
  }

  std::set<std::uint64_t> stable_ids;
  // Untitled number -> the buffer the first tab with it shows.
  std::map<std::uint32_t, const editor::TextViewport*> untitled;
  // File -> the first tab holding it as a live buffer, and that tab's name.
  std::map<std::string, std::pair<const editor::TextViewport*, std::string>> files;
  // The domain is LiveBufferViewOfPath's: a loaded editor tab and a compare tab's
  // editable side ARE the file; a view still loading, a read-only compare side and
  // a merge result are not.
  const auto one_buffer = [&files, &violate](const editor::TextViewport& view, std::string name) {
    if (view.path().empty()) {
      return;
    }
    const auto [first, inserted] =
        files.try_emplace(view.path().lexically_normal().string(), &view, name);
    if (!inserted && !first->second.first->SharesDocumentWith(view)) {
      violate("buffers", name + " is a second buffer of the file " + first->second.second + " holds");
    }
  };
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const EditorGroup& group = groups[g];
    if (!group.open_tabs.empty() && group.active_tab_index >= group.open_tabs.size()) {
      violate("tabs", "group " + std::to_string(g) + " has active tab " +
                          std::to_string(group.active_tab_index) + " of " +
                          std::to_string(group.open_tabs.size()));
    }
    std::set<std::uint64_t> recent;
    for (const std::uint64_t id : group.recent_tab_ids) {
      if (!recent.insert(id).second) {
        violate("tabs", "group " + std::to_string(g) + " lists tab id " + std::to_string(id) +
                            " twice in its recent tabs");
      }
    }
    for (std::size_t t = 0; t < group.open_tabs.size(); ++t) {
      const TabEntry& tab = group.open_tabs[t];
      ++report.tabs;
      if (tab.stable_id != 0 && !stable_ids.insert(tab.stable_id).second) {
        violate("tabs", TabName(g, t, tab) + " shares its stable id " + std::to_string(tab.stable_id) +
                            " with another tab");
      }
      switch (tab.kind) {
        case TabEntry::Kind::Compare:
          if (!tab.compare.has_value()) {
            violate("tabs", TabName(g, t, tab) + " is a compare tab with no compare state");
          } else if (tab.compare->right_editable && !tab.compare->load_pending) {
            one_buffer(tab.compare->right_viewport, TabName(g, t, tab));
          }
          continue;
        case TabEntry::Kind::Merge:
          if (!tab.merge.has_value()) {
            violate("tabs", TabName(g, t, tab) + " is a merge tab with no merge state");
          }
          continue;
        case TabEntry::Kind::Editor:
          break;
      }
      if (!tab.editor_state.has_value()) {
        if (!tab.deferred_handle.has_value()) {
          violate("tabs", TabName(g, t, tab) + " is an editor tab with neither a viewport nor a deferred handle");
        }
        continue;
      }
      const EditorTabState& state = *tab.editor_state;
      if (state.content_pending()) {
        continue;  // the viewport is not the file yet: nothing of it to hold
      }
      const editor::TextViewport& view = state.viewport;
      ++report.editors;
      one_buffer(view, TabName(g, t, tab));
      if (tab.path.empty()) {
        if (tab.untitled_number == 0) {
          violate("untitled", TabName(g, t, tab) + " is an unsaved buffer with no Untitled number");
        } else if (const auto [first, inserted] = untitled.emplace(tab.untitled_number, &view);
                   !inserted && !first->second->SharesDocumentWith(view)) {
          violate("untitled", "Untitled-" + std::to_string(tab.untitled_number) +
                                  " names two different buffers (the second is " + TabName(g, t, tab) + ")");
        }
      }
      if (view.line_count() == 0) {
        violate("carets", TabName(g, t, tab) + " has a buffer of no lines");
        continue;
      }
      if (view.CaretsLagDocument()) {
        // The document moved since this view last ran -- another view of it
        // edited it, and that view may be closed by now: this view's carets are
        // reconciled on its next use, and every read of them before that sees
        // them reconciled (TextViewport::SyncCaretsWithSharedDocument), so the
        // stored positions may name columns the document no longer has. A rule
        // that lag needs a live sharer fired on every closed sibling (soak
        // 8cc3fd5f5a87).
      } else if (!Inside(view, view.cursor_line(), view.cursor_column())) {
        violate("carets", TabName(g, t, tab) + " has its caret at " +
                              Outside(view, view.cursor_line(), view.cursor_column()));
      }
      if (const auto& anchor = view.selection_anchor();
          !view.CaretsLagDocument() && anchor.has_value() && !Inside(view, anchor->line, anchor->column)) {
        violate("carets", TabName(g, t, tab) + " has its selection anchor at " +
                              Outside(view, anchor->line, anchor->column));
      }
      for (const editor::TextPosition& caret : view.secondary_carets()) {
        if (!view.CaretsLagDocument() && !Inside(view, caret.line, caret.column)) {
          violate("carets", TabName(g, t, tab) + " has a secondary caret at " +
                                Outside(view, caret.line, caret.column));
          break;
        }
      }
      const bool focused = g == project.clamped_focused_group_index() && t == group.active_tab_index;
      if (options.deep && focused && view.line_count() <= options.max_deep_lines) {
        report.deep = true;
        if (std::string stale = view.VerifyDerivedCachesForTesting(); !stale.empty()) {
          violate("caches", TabName(g, t, tab) + ": " + stale);
        }
      }
    }
  }
  return report;
}

util::JsonValue BuildControlInvariantsJson(const ProjectWorkspaceState* project,
                                           const ControlInvariantsQueryOptions& options) {
  util::JsonObject object;
  if (project == nullptr) {
    object["ok"] = util::JsonValue(true);
    object["violations"] = util::JsonValue(util::JsonArray{});
    return util::JsonValue(std::move(object));
  }
  const WorkspaceInvariantReport report = CheckWorkspaceInvariants(*project, options);
  util::JsonArray violations;
  for (const WorkspaceInvariantViolation& violation : report.violations) {
    util::JsonObject entry;
    entry["rule"] = util::JsonValue(violation.rule);
    entry["detail"] = util::JsonValue(violation.detail);
    violations.push_back(util::JsonValue(std::move(entry)));
  }
  object["ok"] = util::JsonValue(report.violations.empty());
  object["violations"] = util::JsonValue(std::move(violations));
  util::JsonObject checked;
  checked["groups"] = util::JsonValue(static_cast<std::int64_t>(report.groups));
  checked["tabs"] = util::JsonValue(static_cast<std::int64_t>(report.tabs));
  checked["editors"] = util::JsonValue(static_cast<std::int64_t>(report.editors));
  checked["deep"] = util::JsonValue(report.deep);
  object["checked"] = util::JsonValue(std::move(checked));
  return util::JsonValue(std::move(object));
}

}  // namespace microide::workspace
