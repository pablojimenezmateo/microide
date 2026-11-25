#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

struct SnippetNestedLink {
  int tab = 0;
  std::size_t index = 0;
  int parent_tab = 0;
  std::size_t parent_index = 0;
};

struct SnippetSessionState {
  bool active = false;
  std::unordered_map<int, std::vector<SelectionRange>> ranges_by_tab;
  std::vector<int> navigate_order;
  std::size_t navigate_index = 0;
  std::unordered_map<int, std::vector<std::string>> choices_by_tab;
  std::unordered_map<int, std::size_t> choice_index_by_tab;
  std::vector<TextPosition> saved_secondary_carets;
  std::vector<SnippetNestedLink> nested_links;
  std::uint64_t tracked_content_revision = 0;

  void Reset(TextViewport* viewport_restore_secondary_to);
};

struct SnippetParseResult {
  static constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);
  std::string expanded;
  struct Occurrence {
    int tab_stop = 0;
    std::size_t start_off = 0;
    std::size_t end_off = 0;
    std::vector<std::string> choices;
    std::size_t parent = kNoParent;
  };
  std::vector<Occurrence> occurrences;
};

using SnippetVariableResolver =
    std::function<std::optional<std::string>(std::string_view name)>;

SnippetParseResult ParseSnippetBody(std::string_view body,
                                    const SnippetVariableResolver& resolve_variable = {});

std::optional<std::string> ResolveEditorSnippetVariable(const TextViewport& viewport,
                                                        const SelectionRange& trigger,
                                                        std::string_view name);

TextPosition PositionAfterOffsetInExpanded(TextPosition trigger_start,
                                           std::string_view expanded_flat,
                                           std::size_t offset);

bool ExpandSnippetAtSelection(TextViewport& viewport,
                              SnippetSessionState& session,
                              const SelectionRange& trigger_range,
                              std::string_view snippet_body);

void CommitSnippetSession(TextViewport& viewport, SnippetSessionState& session);

bool SnippetNavigateTab(TextViewport& viewport, SnippetSessionState& session, bool backward);
bool SnippetHandleEscape(TextViewport& viewport, SnippetSessionState& session);
void SnippetOnCaretMoved(TextViewport& viewport, SnippetSessionState& session);
void SnippetOnBufferChanged(TextViewport& viewport, SnippetSessionState& session);

bool SnippetTryInsertText(TextViewport& viewport, SnippetSessionState& session, std::string_view text);
bool SnippetTryBackspace(TextViewport& viewport, SnippetSessionState& session);
bool SnippetTryDeleteForward(TextViewport& viewport, SnippetSessionState& session);

}
