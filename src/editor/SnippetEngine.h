#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

struct SnippetSessionState {
  bool active = false;
  std::unordered_map<int, std::vector<SelectionRange>> ranges_by_tab;
  std::vector<int> navigate_order;
  std::size_t navigate_index = 0;
  std::unordered_map<int, std::vector<std::string>> choices_by_tab;
  std::unordered_map<int, std::size_t> choice_index_by_tab;
  std::vector<TextPosition> saved_secondary_carets;

  void Reset(TextViewport* viewport_restore_secondary_to);
};

struct SnippetParseResult {
  std::string expanded;
  struct Occurrence {
    int tab_stop = 0;
    std::size_t start_off = 0;
    std::size_t end_off = 0;
    bool is_final = false;
    std::vector<std::string> choices;
  };
  std::vector<Occurrence> occurrences;
};

SnippetParseResult ParseSnippetBody(std::string_view body);

TextPosition PositionAfterOffsetInExpanded(TextPosition trigger_start,
                                           std::string_view expanded_flat,
                                           std::size_t offset);

bool ExpandSnippetAtSelection(TextViewport& viewport,
                              SnippetSessionState& session,
                              const SelectionRange& trigger_range,
                              std::string_view snippet_body);

// Commit grouped undo and restore secondary carets (call when exiting session).
void CommitSnippetSession(TextViewport& viewport, SnippetSessionState& session);

bool SnippetNavigateTab(TextViewport& viewport, SnippetSessionState& session, bool backward);
bool SnippetHandleEscape(TextViewport& viewport, SnippetSessionState& session);
void SnippetOnCaretMoved(TextViewport& viewport, SnippetSessionState& session);

bool SnippetTryInsertText(TextViewport& viewport, SnippetSessionState& session, std::string_view text);
bool SnippetTryBackspace(TextViewport& viewport, SnippetSessionState& session);
bool SnippetTryDeleteForward(TextViewport& viewport, SnippetSessionState& session);

}  // namespace microide::editor
