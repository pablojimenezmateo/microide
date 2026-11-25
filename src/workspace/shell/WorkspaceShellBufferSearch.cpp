#include "workspace/shell/WorkspaceShell.h"

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util/PathMatch.h"
#include "workspace/BufferFindScope.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

util::CompiledRegex CompileBufferSearchRegex(const std::string& query, bool case_sensitive) {
  const std::uint32_t options =
      util::SearchRegexCompileOptions(query, case_sensitive) | PCRE2_MULTILINE;
  return util::CompiledRegex(query, options, {}, util::AsciiTwin::Build);
}

BufferSearchOptions OptionsOf(const BufferSearchState& state) {
  return BufferSearchOptions{
      .case_sensitive = state.match_case,
      .whole_word = state.whole_word,
      .scope = state.scope.has_value() ? std::optional(state.scope->range) : std::nullopt,
  };
}

editor::SelectionRange WholeDocumentRange(const editor::TextViewport& viewport) {
  const std::size_t line_count = viewport.lines().LineCount();
  const std::size_t last = line_count == 0 ? 0 : line_count - 1;
  return editor::SelectionRange{
      .start = editor::TextPosition{0, 0},
      .end = editor::TextPosition{last, line_count == 0 ? 0 : viewport.lines().LineLength(last)},
  };
}

std::string BuildWholeBufferContent(const editor::TextViewport& viewport) {
  std::string content;
  viewport.lines().AppendWholeText(content);
  return content;
}

std::size_t WholeBufferOffset(const editor::TextBuffer& lines, const editor::TextPosition& position) {
  std::size_t offset = 0;
  for (std::size_t i = 0; i < position.line; ++i) {
    offset += lines.LineLength(i) + 1;
  }
  return offset + position.column;
}

bool ReplaceMatchesInScope(editor::TextViewport& viewport,
                           const editor::SelectionRange& scope,
                           const std::vector<editor::SelectionRange>& matches,
                           std::string_view replacement,
                           const util::CompiledRegex* pattern) {
  if (matches.empty()) {
    return true;
  }
  const editor::TextBuffer& lines = viewport.lines();
  const std::string content = BuildWholeBufferContent(viewport);
  const std::size_t scope_begin = WholeBufferOffset(lines, scope.start);
  const std::size_t scope_end = WholeBufferOffset(lines, scope.end);
  std::string rebuilt;
  std::size_t copied_to = scope_begin;
  for (const editor::SelectionRange& match : matches) {
    const std::size_t begin = WholeBufferOffset(lines, match.start);
    const std::size_t end = WholeBufferOffset(lines, match.end);
    rebuilt.append(content, copied_to, begin - copied_to);
    if (pattern != nullptr) {
      const std::optional<std::string> expanded = pattern->ExpandMatchAt(content, begin, replacement);
      if (!expanded.has_value()) {
        return false;
      }
      rebuilt.append(*expanded);
    } else {
      rebuilt.append(replacement);
    }
    copied_to = end;
  }
  rebuilt.append(content, copied_to, scope_end - copied_to);
  return viewport.ReplaceRange(scope, rebuilt);
}

void ApplyBufferRegexReplaceAll(editor::TextViewport& viewport,
                                const std::string& query,
                                std::string_view replacement,
                                bool case_sensitive) {
  if (query.empty()) {
    return;
  }
  const util::CompiledRegex pattern = CompileBufferSearchRegex(query, case_sensitive);
  if (!pattern.valid()) {
    return;
  }
  const editor::SelectionRange whole = WholeDocumentRange(viewport);
  const std::string content = BuildWholeBufferContent(viewport);
  std::string rebuilt;
  const int rc = pattern.SubstituteInto(content, replacement, rebuilt);
  if (rc <= 0) {
    return;
  }
  viewport.ReplaceRange(whole, rebuilt);
}

}

void WorkspaceShell::ToggleBufferSearchOption(BufferFindToggle toggle) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  switch (toggle) {
    case BufferFindToggle::MatchCase:
      buffer_search.match_case = !buffer_search.match_case;
      break;
    case BufferFindToggle::WholeWord:
      buffer_search.whole_word = !buffer_search.whole_word;
      break;
    case BufferFindToggle::Regex:
      buffer_search.regex = !buffer_search.regex;
      break;
    case BufferFindToggle::InSelection:
      if (buffer_search.scope.has_value()) {
        buffer_search.scope.reset();
      } else if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
        FollowFindScopes(buffer_search, *viewport);
        const auto selection = viewport->selection_range();
        const bool widget_selection =
            selection.has_value() && BufferSearchMatchesAreFresh() &&
            buffer_search.selected_index < buffer_search.matches.size() &&
            editor::TextViewport::NormalizeRange(*selection) == buffer_search.matches[buffer_search.selected_index];
        if (widget_selection && buffer_search.opened_over.has_value()) {
          buffer_search.scope = buffer_search.opened_over;
        } else {
          buffer_search.scope = FindScopeAt(*viewport);
        }
      }
      break;
    case BufferFindToggle::Count:
      return;
  }
  RefreshBufferSearch();
}

void WorkspaceShell::RefreshBufferSearch() { RefreshBufferSearch(true); }

bool WorkspaceShell::BufferSearchMatchesAreFresh() const {
  const auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  const editor::TextViewport* viewport = ActiveEditorViewport();
  return buffer_search.matches_viewport == static_cast<const void*>(viewport) &&
         (viewport == nullptr ||
          (buffer_search.matches_document == viewport->document_identity() &&
           buffer_search.matches_content_revision == viewport->content_revision()));
}

void WorkspaceShell::RefreshBufferSearchIfStale() {
  const OverlayMode mode = context_.current_project_state.overlay.mode;
  if (mode != OverlayMode::BufferSearch && mode != OverlayMode::BufferReplace) {
    return;
  }
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    FollowFindScopes(buffer_search, *viewport);
  }
  if (buffer_search.query.text().empty() || BufferSearchMatchesAreFresh()) {
    return;
  }
  util::PerformanceTrace::Scope scope("WorkspaceShell::RefreshBufferSearchIfStale");
  util::AddPerformanceCounter(util::PerfCounterId::SearchBufferRefreshesAfterEdit);
  RefreshBufferSearch(false);
}

void WorkspaceShell::RefreshBufferSearch(bool reveal) {
  util::PerformanceTrace::Scope scope("WorkspaceShell::RefreshBufferSearch");
  util::AddPerformanceCounter(util::PerfCounterId::SearchBufferRefreshes);
  editor::TextViewport* viewport = ActiveEditorViewport();
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (viewport == nullptr) {
    buffer_search.matches.clear();
    ++buffer_search.matches_revision;
    buffer_search.matches_viewport = nullptr;
    buffer_search.matches_document = nullptr;
    buffer_search.matches_content_revision = 0;
    buffer_search.selected_index = 0;
    buffer_search.incremental = {};
    return;
  }

  const std::string& query = buffer_search.query.text();
  const editor::TextBuffer& buffer = viewport->lines();
  const std::uint64_t content_revision = viewport->content_revision();
  FollowFindScopes(buffer_search, *viewport);
  const std::optional<editor::SelectionRange> find_scope =
      buffer_search.scope.has_value() ? std::optional(buffer_search.scope->range) : std::nullopt;

  auto& incremental = buffer_search.incremental;
  if (buffer_search.regex) {
    const util::CompiledRegex pattern = CompileBufferSearchRegex(query, buffer_search.match_case);
    buffer_search.matches = pattern.valid()
                                ? FindRegexSearchMatches(buffer, pattern, OptionsOf(buffer_search))
                                : std::vector<editor::SelectionRange>{};
    incremental.valid = false;
  } else {
    const bool options_unchanged = incremental.match_case == buffer_search.match_case &&
                                   incremental.whole_word == buffer_search.whole_word &&
                                   incremental.scope == find_scope;
    const bool query_extends =
        buffer_search.match_case
            ? query.size() >= incremental.query.size() && query.starts_with(incremental.query)
            : QueryExtendsCaseInsensitive(incremental.query, query);
    const bool can_refine = incremental.valid && options_unchanged &&
                            incremental.viewport == static_cast<const void*>(viewport) &&
                            incremental.document == viewport->document_identity() &&
                            incremental.content_revision == content_revision &&
                            !incremental.query.empty() && query_extends;
    buffer_search.matches =
        can_refine ? RefineLiteralSearchMatches(buffer, query, buffer_search.matches,
                                                OptionsOf(buffer_search))
                   : FindLiteralSearchMatches(buffer, query, OptionsOf(buffer_search));
    incremental.valid = true;
    incremental.viewport = viewport;
    incremental.document = viewport->document_identity();
    incremental.content_revision = content_revision;
    incremental.query = query;
    incremental.match_case = buffer_search.match_case;
    incremental.whole_word = buffer_search.whole_word;
    incremental.scope = find_scope;
  }
  ++buffer_search.matches_revision;
  buffer_search.matches_viewport = viewport;
  buffer_search.matches_document = viewport->document_identity();
  buffer_search.matches_content_revision = content_revision;
  util::AddPerformanceCounter(util::PerfCounterId::SearchBufferRefreshLinesScanned,
                              buffer.LineCount());

  buffer_search.selected_index = 0;
  if (!buffer_search.matches.empty()) {
    const editor::TextPosition seed =
        viewport->selection_range().has_value()
            ? viewport->selection_range()->start
            : editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
    const auto at_or_after = std::lower_bound(
        buffer_search.matches.begin(), buffer_search.matches.end(), seed,
        [](const editor::SelectionRange& match, const editor::TextPosition& position) {
          return match.start.line < position.line ||
                 (match.start.line == position.line && match.start.column < position.column);
        });
    if (at_or_after != buffer_search.matches.end()) {
      buffer_search.selected_index =
          static_cast<std::size_t>(at_or_after - buffer_search.matches.begin());
    }
    if (reveal) {
      RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
    }
  }
  ResetOverlayScroll();
  RequestPreparedFrame();
  RequestPreparedFrame();
}

void WorkspaceShell::OpenBufferSearchFromProjectSearchResult() {
  auto& project_search = context_.current_project_state.overlay.workflow.project_search;
  if (project_search.results.empty() ||
      project_search.selected_index >= project_search.results.size()) {
    return;
  }
  std::string query = project_search.query.text();
  if (query.empty()) {
    return;
  }
  const auto& result = project_search.results[project_search.selected_index];
  const std::size_t target_line = result.line;
  const std::size_t target_column = result.column;

  ShowOverlay(OverlayMode::BufferSearch);
  context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
  ResetBufferSearchFoldRevealState(false);

  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  buffer_search.query.SetText(std::move(query));
  buffer_search.replace_text.SetText("");
  RefreshBufferSearch();

  if (buffer_search.matches.empty()) {
    return;
  }

  std::size_t selected = buffer_search.matches.size() - 1;
  for (std::size_t i = 0; i < buffer_search.matches.size(); ++i) {
    const auto& start = buffer_search.matches[i].start;
    if (start.line > target_line ||
        (start.line == target_line && start.column >= target_column)) {
      selected = i;
      break;
    }
  }
  buffer_search.selected_index = selected;
  RevealBufferSearchMatch(buffer_search.matches[selected]);
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    RevealOverlaySelection(ComputeOverlayRect(layout->overlay_anchor));
  }
  RequestPreparedFrame();
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (!BufferSearchMatchesAreFresh() && !buffer_search.query.text().empty()) {
    RefreshBufferSearch(false);
    if (delta > 0) {
      --delta;
    }
    if (delta == 0 && !buffer_search.matches.empty()) {
      RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
      RequestPreparedFrame();
      return;
    }
  }
  if (buffer_search.matches.empty() || delta == 0) {
    return;
  }

  const int count = static_cast<int>(buffer_search.matches.size());
  const int current = static_cast<int>(buffer_search.selected_index);
  buffer_search.selected_index =
      static_cast<std::size_t>(((current + delta) % count + count) % count);
  RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  if (context_.current_project_state.overlay.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->overlay_anchor));
    }
  }
  RequestPreparedFrame();
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (!BufferSearchMatchesAreFresh()) {
    RefreshBufferSearch(false);
  }
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.matches.empty() ||
      buffer_search.selected_index >= buffer_search.matches.size()) {
    return;
  }

  buffer_search.preserve_temporarily_expanded_folds = true;
  const auto match = buffer_search.matches[buffer_search.selected_index];
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return;
  }

  bool replaced = false;
  if (buffer_search.regex) {
    const std::string& query = buffer_search.query.text();
    const util::CompiledRegex pattern = CompileBufferSearchRegex(query, buffer_search.match_case);
    if (!pattern.valid() || match.start.line >= viewport->lines().LineCount()) {
      return;
    }
    const std::string content = BuildWholeBufferContent(*viewport);
    const std::optional<std::string> expanded = pattern.ExpandMatchAt(
        content, WholeBufferOffset(viewport->lines(), match.start), buffer_search.replace_text.text());
    if (!expanded.has_value()) {
      return;
    }
    replaced = viewport->ReplaceRange(match, *expanded);
  } else {
    replaced = viewport->ReplaceRange(match, buffer_search.replace_text.text());
  }
  if (!replaced) {
    if (viewport->read_only()) {
      return;
    }
    MoveBufferSearchSelection(1);
    return;
  }

  RefreshBufferSearch();
  if (!buffer_search.matches.empty()) {
    buffer_search.selected_index =
        std::min(buffer_search.selected_index, buffer_search.matches.size() - 1);
    RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  }
  RequestPreparedFrame();
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (!BufferSearchMatchesAreFresh()) {
    RefreshBufferSearch(false);
  }
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.query.text().empty()) {
    return;
  }

  buffer_search.preserve_temporarily_expanded_folds = true;
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport != nullptr && buffer_search.scope.has_value()) {
    const util::CompiledRegex pattern =
        buffer_search.regex ? CompileBufferSearchRegex(buffer_search.query.text(), buffer_search.match_case)
                            : util::CompiledRegex{};
    if (!buffer_search.regex || pattern.valid()) {
      (void)ReplaceMatchesInScope(*viewport, buffer_search.scope->range, buffer_search.matches,
                                  buffer_search.replace_text.text(),
                                  buffer_search.regex ? &pattern : nullptr);
    }
  } else if (viewport != nullptr) {
    if (buffer_search.regex) {
      ApplyBufferRegexReplaceAll(*viewport, buffer_search.query.text(),
                                 buffer_search.replace_text.text(), buffer_search.match_case);
    } else {
      const auto& incremental = buffer_search.incremental;
      const bool matches_fresh =
          incremental.valid && incremental.viewport == static_cast<const void*>(viewport) &&
          incremental.content_revision == viewport->content_revision() &&
          incremental.query == buffer_search.query.text() &&
          incremental.match_case == buffer_search.match_case &&
          incremental.whole_word == buffer_search.whole_word &&
          buffer_search.matches.size() < kMaxBufferSearchMatches;
      const bool applied =
          matches_fresh &&
          viewport->ReplaceAllRanges(buffer_search.matches, buffer_search.replace_text.text())
              .has_value();
      if (!applied) {
        if (buffer_search.match_case || buffer_search.whole_word) {
          const auto rescanned = FindLiteralSearchMatches(viewport->lines(),
                                                          buffer_search.query.text(),
                                                          OptionsOf(buffer_search));
          viewport->ReplaceAllRanges(rescanned, buffer_search.replace_text.text());
        } else {
          viewport->ReplaceAll(buffer_search.query.text(), buffer_search.replace_text.text());
        }
      }
    }
  }
  RefreshBufferSearch();
  RequestPreparedFrame();
}

void WorkspaceShell::RevealBufferSearchMatch(const editor::SelectionRange& match) {
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return;
  }
  viewport->JumpCursorTo(match.start.line, match.start.column);
  viewport->JumpCursorTo(match.end.line, match.end.column, true);

  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
  if (model == nullptr) {
    return;
  }

  bool changed = false;
  std::vector<editor::FoldRange> hiding;
  for (const editor::FoldRange& range : model->collapsed_ranges()) {
    if (match.start.line > range.opener_line && match.start.line <= range.closer_line) {
      hiding.push_back(range);
    }
  }
  for (const editor::FoldRange& range : hiding) {
    if (model->Expand(range.opener_line)) {
      if (buffer_search.temporarily_expanded_folds.empty()) {
        buffer_search.temporarily_expanded_fold_tab_path = viewport->path().lexically_normal();
      }
      const auto already = std::find_if(buffer_search.temporarily_expanded_folds.begin(),
                                        buffer_search.temporarily_expanded_folds.end(),
                                        [&](const editor::FoldRange& entry) {
                                          return entry.opener_line == range.opener_line;
                                        });
      if (already == buffer_search.temporarily_expanded_folds.end()) {
        buffer_search.temporarily_expanded_folds.push_back(range);
      }
      changed = true;
    }
  }

  if (changed) {
    RequestPreparedFrame();
  }
}

void WorkspaceShell::ResetBufferSearchFoldRevealState(bool preserve_expanded_folds) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  const bool should_restore =
      !preserve_expanded_folds && !buffer_search.preserve_temporarily_expanded_folds &&
      !buffer_search.temporarily_expanded_folds.empty();
  if (should_restore) {
    editor::TextViewport* viewport = ActiveEditorViewport();
    editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
    if (viewport != nullptr && model != nullptr &&
        util::SameAsNormalizedPath(viewport->path(),
                                   buffer_search.temporarily_expanded_fold_tab_path)) {
      bool changed = false;
      for (auto it = buffer_search.temporarily_expanded_folds.rbegin();
           it != buffer_search.temporarily_expanded_folds.rend(); ++it) {
        changed = model->CollapseRange(*it) || changed;
      }
      if (changed) {
        RequestPreparedFrame();
      }
    }
  }

  buffer_search.temporarily_expanded_folds.clear();
  buffer_search.temporarily_expanded_fold_tab_path.clear();
  buffer_search.preserve_temporarily_expanded_folds = false;
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!context_.current_project_state.overlay.visible ||
      (context_.current_project_state.overlay.mode != OverlayMode::BufferSearch &&
       context_.current_project_state.overlay.mode != OverlayMode::BufferReplace) ||
      context_.current_project_state.overlay.workflow.buffer_search.matches.empty() ||
      context_.current_project_state.overlay.workflow.buffer_search.selected_index >=
          context_.current_project_state.overlay.workflow.buffer_search.matches.size()) {
    return std::nullopt;
  }
  return context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
}

}
