#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

// A placeholder recorded inside another placeholder's default text
// (`${1:foo ${2:bar}}`): `ranges_by_tab[tab][index]` lies within
// `ranges_by_tab[parent_tab][parent_index]`. Editing the inner grows the outer;
// editing the outer discards the inner (VS Code's rule: a placeholder that is
// typed over loses its nested placeholders).
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

  void Reset(TextViewport* viewport_restore_secondary_to);
};

struct SnippetParseResult {
  static constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);
  std::string expanded;
  struct Occurrence {
    int tab_stop = 0;
    std::size_t start_off = 0;
    std::size_t end_off = 0;
    bool is_final = false;
    std::vector<std::string> choices;
    // Index into `occurrences` of the placeholder whose default text this one
    // sits in, or kNoParent at the top level.
    std::size_t parent = kNoParent;
  };
  std::vector<Occurrence> occurrences;
};

// Answers a `$NAME` variable: the value when the variable is known (possibly
// empty, in which case a `${NAME:default}` default is used), nullopt when it is
// not, in which case VS Code's rule applies — the name (or the default) is
// inserted and becomes a placeholder after the numbered ones.
using SnippetVariableResolver =
    std::function<std::optional<std::string>(std::string_view name)>;

// Parse a VS Code snippet body: `$1`, `${1}`, `${1:default}` (defaults nest),
// `${1|a,b|}` choices, `$VAR` / `${VAR}` / `${VAR:default}` variables, and
// `\$` `\}` `\\` escapes. A transform (`${1/re/fmt/}`, `${VAR/re/fmt/}`) is
// parsed and its regex step skipped: the tab stop or variable value is inserted
// untransformed. Caps make a hostile body fail to an empty result.
SnippetParseResult ParseSnippetBody(std::string_view body,
                                    const SnippetVariableResolver& resolve_variable = {});

// The editor's variable set: the TM_* file/line variables from the viewport and
// trigger range, the CURRENT_* clock variables, RANDOM/UUID, and the comment
// tokens from the viewport's language contract. Exposed so a test can pin it.
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

// Commit grouped undo and restore secondary carets (call when exiting session).
void CommitSnippetSession(TextViewport& viewport, SnippetSessionState& session);

bool SnippetNavigateTab(TextViewport& viewport, SnippetSessionState& session, bool backward);
bool SnippetHandleEscape(TextViewport& viewport, SnippetSessionState& session);
void SnippetOnCaretMoved(TextViewport& viewport, SnippetSessionState& session);

bool SnippetTryInsertText(TextViewport& viewport, SnippetSessionState& session, std::string_view text);
bool SnippetTryBackspace(TextViewport& viewport, SnippetSessionState& session);
bool SnippetTryDeleteForward(TextViewport& viewport, SnippetSessionState& session);

}  // namespace microide::editor
