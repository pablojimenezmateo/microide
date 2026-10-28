#include "workspace/assist/SnippetAssist.h"

#include "editor/SnippetEngine.h"

namespace microide::workspace {

TabEntry::EditorTabState* SnippetAssist::ActiveSessionTab() const {
  if (!host_.SnippetsEnabled()) {
    return nullptr;
  }
  TabEntry::EditorTabState* tab = host_.ops().active_editor_tab();
  return tab != nullptr && tab->snippet_session.active ? tab : nullptr;
}

template <typename Edit>
bool SnippetAssist::EditInSession(editor::TextViewport* viewport, Edit&& edit) {
  TabEntry::EditorTabState* tab = viewport != nullptr ? ActiveSessionTab() : nullptr;
  if (tab == nullptr) {
    return false;
  }
  const AssistHost::EditSnapshot snapshot = AssistHost::CaptureEditSnapshot(*viewport);
  if (!edit(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  host_.ApplyEditSideEffects(*viewport, snapshot);
  return true;
}

void SnippetAssist::AfterSessionNavigation(TabEntry::EditorTabState& tab) {
  tab.folding_model->MarkDirty();
  host_.RedrawAfterCaretChange();
}

bool SnippetAssist::TryTab(bool shift_tab) {
  if (!host_.SnippetsEnabled()) {
    return false;
  }
  TabEntry::EditorTabState* tab = host_.ops().active_editor_tab();
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (tab == nullptr || viewport == nullptr) {
    return false;
  }
  // With no active session a forward Tab tries prefix-triggered snippet expansion
  // before the caller falls back to inserting a literal tab.
  if (!tab->snippet_session.active) {
    return !shift_tab && TryPrefixExpansion(*tab, *viewport);
  }
  if (!editor::SnippetNavigateTab(*viewport, tab->snippet_session, shift_tab)) {
    return false;
  }
  AfterSessionNavigation(*tab);
  return true;
}

bool SnippetAssist::TryPrefixExpansion(TabEntry::EditorTabState& tab,
                                       editor::TextViewport& viewport) {
  const LanguageContract* contract = host_.languages().Find(viewport.language_id());
  if (contract == nullptr || contract->snippets.empty()) {
    return false;
  }
  const editor::SelectionRange range = assist::CompletionReplacementRange(viewport);
  if (range.start.line != range.end.line || range.end.column <= range.start.column) {
    return false;
  }
  const std::string_view line = assist::LineViewAt(viewport, range.start.line);
  if (range.end.column > line.size()) {
    return false;
  }
  const std::string_view prefix =
      line.substr(range.start.column, range.end.column - range.start.column);
  // Require a unique exact prefix match; ambiguity falls through to a literal tab.
  const LanguageSnippet* match = nullptr;
  for (const auto& snippet : contract->snippets) {
    if (snippet.prefix == prefix) {
      if (match != nullptr) {
        return false;
      }
      match = &snippet;
    }
  }
  if (match == nullptr) {
    return false;
  }

  const AssistHost::EditSnapshot snapshot = AssistHost::CaptureEditSnapshot(viewport);
  viewport.BeginUndoGroup();
  if (!editor::ExpandSnippetAtSelection(viewport, tab.snippet_session, range, match->body)) {
    if (viewport.UndoGroupActive()) {
      viewport.EndUndoGroup();
    }
    return false;
  }
  tab.folding_model->MarkDirty();
  host_.ApplyEditSideEffects(viewport, snapshot);
  return true;
}

bool SnippetAssist::TryEscape() {
  TabEntry::EditorTabState* tab = ActiveSessionTab();
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (tab == nullptr || viewport == nullptr ||
      !editor::SnippetHandleEscape(*viewport, tab->snippet_session)) {
    return false;
  }
  AfterSessionNavigation(*tab);
  return true;
}

void SnippetAssist::NotifyCaretMoved() {
  TabEntry::EditorTabState* tab = host_.ops().active_editor_tab();
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return;
  }
  editor::SnippetOnCaretMoved(*viewport, tab->snippet_session);
  if (!tab->snippet_session.active) {
    AfterSessionNavigation(*tab);
  }
}

void SnippetAssist::NotifyBufferChanged() {
  TabEntry::EditorTabState* tab = host_.ops().active_editor_tab();
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return;
  }
  editor::SnippetOnBufferChanged(*viewport, tab->snippet_session);
}

void SnippetAssist::ClearAfterUndo() {
  TabEntry::EditorTabState* tab = host_.ops().active_editor_tab();
  if (tab != nullptr && tab->snippet_session.active) {
    tab->snippet_session.Reset(nullptr);
  }
}

bool SnippetAssist::TryInsertText(editor::TextViewport* viewport, std::string_view text) {
  if (text.empty()) {
    return false;
  }
  return EditInSession(viewport, [text](editor::TextViewport& target,
                                        editor::SnippetSessionState& session) {
    return editor::SnippetTryInsertText(target, session, text);
  });
}

bool SnippetAssist::TryBackspace(editor::TextViewport* viewport) {
  return EditInSession(viewport, [](editor::TextViewport& target,
                                    editor::SnippetSessionState& session) {
    return editor::SnippetTryBackspace(target, session);
  });
}

bool SnippetAssist::TryDeleteForward(editor::TextViewport* viewport) {
  return EditInSession(viewport, [](editor::TextViewport& target,
                                    editor::SnippetSessionState& session) {
    return editor::SnippetTryDeleteForward(target, session);
  });
}

}  // namespace microide::workspace
