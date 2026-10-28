#pragma once

#include <string_view>

#include "workspace/assist/AssistHost.h"

namespace microide::workspace {

// The active editor tab's snippet session: prefix expansion on Tab, placeholder
// navigation, and the edits that land inside a placeholder.
class SnippetAssist {
 public:
  explicit SnippetAssist(AssistHost& host) : host_(host) {}

  bool TryTab(bool shift_tab);
  bool TryEscape();
  void NotifyCaretMoved();
  // An edit landed. Ends an active snippet session unless the edit was the
  // engine's own -- every other edit moves the text its placeholder ranges point
  // at (see SnippetSessionState::tracked_content_revision).
  void NotifyBufferChanged();
  void ClearAfterUndo();
  bool TryInsertText(editor::TextViewport* viewport, std::string_view text);
  bool TryBackspace(editor::TextViewport* viewport);
  bool TryDeleteForward(editor::TextViewport* viewport);

 private:
  // Expand a snippet whose `prefix` matches the identifier immediately left of the
  // caret (Tab with no active session). Returns false on no/ambiguous match so the
  // caller falls through to inserting a literal tab.
  bool TryPrefixExpansion(TabEntry::EditorTabState& tab, editor::TextViewport& viewport);
  // The active tab's session, when snippets are on and one is running.
  TabEntry::EditorTabState* ActiveSessionTab() const;
  // Run one session edit, then mark folds dirty and apply the edit's side effects.
  template <typename Edit>
  bool EditInSession(editor::TextViewport* viewport, Edit&& edit);
  // A session change that moved only the caret/selection.
  void AfterSessionNavigation(TabEntry::EditorTabState& tab);

  AssistHost& host_;
};

}  // namespace microide::workspace
