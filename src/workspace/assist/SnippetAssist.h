#pragma once

#include <string_view>

#include "workspace/assist/AssistHost.h"

namespace microide::workspace {

class SnippetAssist {
 public:
  explicit SnippetAssist(AssistHost& host) : host_(host) {}

  bool TryTab(bool shift_tab);
  bool TryEscape();
  void NotifyCaretMoved();
  void NotifyBufferChanged();
  void ClearAfterUndo();
  bool TryInsertText(editor::TextViewport* viewport, std::string_view text);
  bool TryBackspace(editor::TextViewport* viewport);
  bool TryDeleteForward(editor::TextViewport* viewport);

 private:
  bool TryPrefixExpansion(TabEntry::EditorTabState& tab, editor::TextViewport& viewport);
  TabEntry::EditorTabState* ActiveSessionTab() const;
  template <typename Edit>
  bool EditInSession(editor::TextViewport* viewport, Edit&& edit);
  void AfterSessionNavigation(TabEntry::EditorTabState& tab);

  AssistHost& host_;
};

}
