#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

// Compiles the in-file find query as a regex. Case sensitivity comes from the
// widget's `Aa` toggle, exactly as it does in literal mode — regex used to be
// smart-case while literal was always insensitive, so flipping `.*` silently
// changed whether `Alpha` matched `alpha`. PCRE2_MULTILINE so `^`/`$` anchor at
// every line boundary and `\n` matches line breaks: the query runs over the whole
// '\n'-joined buffer, not per line, so multi-line patterns work.
util::CompiledRegex CompileBufferSearchRegex(const std::string& query, bool case_sensitive) {
  const std::uint32_t options =
      util::SearchRegexCompileOptions(query, case_sensitive) | PCRE2_MULTILINE;
  return util::CompiledRegex(query, options);
}

BufferSearchOptions OptionsOf(const BufferSearchState& state) {
  return BufferSearchOptions{.case_sensitive = state.match_case, .whole_word = state.whole_word};
}

// Whole-document range [(0,0) .. (last_line, last_line_length)].
editor::SelectionRange WholeDocumentRange(const editor::TextViewport& viewport) {
  const std::size_t line_count = viewport.lines().LineCount();
  const std::size_t last = line_count == 0 ? 0 : line_count - 1;
  return editor::SelectionRange{
      .start = editor::TextPosition{0, 0},
      .end = editor::TextPosition{last, line_count == 0 ? 0 : viewport.lines().LineLength(last)},
  };
}

// The whole buffer as one '\n'-joined string (the buffer is internally
// CRLF-normalized, so this contains only '\n' line breaks).
std::string BuildWholeBufferContent(const editor::TextViewport& viewport) {
  // The piece tree already stores exactly this: the document '\n'-joined. One
  // walk with a memcpy per piece, instead of two tree descents per line plus a
  // per-line cache entry for every line that spans pieces (which, on a heavily
  // edited buffer, retained a second full copy of the document until the next
  // mutation).
  std::string content;
  viewport.lines().AppendWholeText(content);
  return content;
}

// Applies a regex replace-all across `viewport` as ONE undo entry. Substitutes over
// the whole '\n'-joined buffer (so `\n`/multi-line matches and capture groups work)
// and replaces the entire document with the result. A no-match (rc 0) or a bad
// replacement escape (rc < 0) leaves the document untouched.
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
    return;  // 0: nothing matched; < 0: invalid replacement escape — no edit.
  }
  viewport.ReplaceRange(whole, rebuilt);
}

}  // namespace

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
    case BufferFindToggle::Count:
      return;
  }
  RefreshBufferSearch();
}

void WorkspaceShell::RefreshBufferSearch() {
  editor::TextViewport* viewport = ActiveEditorViewport();
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (viewport == nullptr) {
    buffer_search.matches.clear();
    ++buffer_search.matches_revision;
    buffer_search.selected_index = 0;
    buffer_search.incremental = {};
    return;
  }

  const std::string& query = buffer_search.query.text();
  const editor::TextBuffer& buffer = viewport->lines();
  const std::uint64_t content_revision = viewport->content_revision();

  // Find-as-you-type fast path: when the query only grows onto the end of the
  // previously searched query over the same unchanged buffer, every match for the
  // longer query is also a match for the shorter one, so the new set is a subset
  // of the cached `matches`. Refine it in O(prior matches) instead of rescanning
  // the whole document each keystroke. Identity (viewport pointer) + content
  // revision guard against a stale or wrong-buffer cache.
  auto& incremental = buffer_search.incremental;
  if (buffer_search.regex) {
    // Regex mode: smart-case + MULTILINE over the whole buffer (so `\n`/multi-line
    // patterns match), full-scan every keystroke. An invalid pattern yields no
    // matches (the widget shows 0/0). The literal refine cache is bypassed and
    // invalidated so switching back to literal rescans.
    const util::CompiledRegex pattern = CompileBufferSearchRegex(query, buffer_search.match_case);
    buffer_search.matches = pattern.valid()
                                ? FindRegexSearchMatches(buffer, pattern, OptionsOf(buffer_search))
                                : std::vector<editor::SelectionRange>{};
    incremental.valid = false;
  } else {
    // Refining is only sound when the options are unchanged AND whole-word is
    // off: a longer query's whole-word hits are NOT a subset of a shorter
    // prefix's (typing "alpha" over "al" adds standalone matches the prefix scan
    // rejected), so whole-word always takes the cold path.
    const bool options_unchanged = incremental.match_case == buffer_search.match_case &&
                                   incremental.whole_word == buffer_search.whole_word;
    const bool query_extends =
        buffer_search.match_case
            ? query.size() >= incremental.query.size() && query.starts_with(incremental.query)
            : QueryExtendsCaseInsensitive(incremental.query, query);
    const bool can_refine = incremental.valid && options_unchanged &&
                            !buffer_search.whole_word &&
                            incremental.viewport == static_cast<const void*>(viewport) &&
                            incremental.content_revision == content_revision &&
                            !incremental.query.empty() && query_extends;
    buffer_search.matches =
        can_refine ? RefineLiteralSearchMatches(buffer, query, buffer_search.matches,
                                                OptionsOf(buffer_search))
                   : FindLiteralSearchMatches(buffer, query, OptionsOf(buffer_search));
    incremental.valid = true;
    incremental.viewport = viewport;
    incremental.content_revision = content_revision;
    incremental.query = query;
    incremental.match_case = buffer_search.match_case;
    incremental.whole_word = buffer_search.whole_word;
  }
  ++buffer_search.matches_revision;

  buffer_search.selected_index = 0;

  if (!buffer_search.matches.empty()) {
    RevealBufferSearchMatch(buffer_search.matches.front());
  }
  ResetOverlayScroll();
  RequestOverlayRedraw();
  RequestEditorSurfaceRedraw();
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

  // Carry the project-search term into the in-file find surface so the user can
  // keep moving between matches in the file they just opened.
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

  // Start navigation on the match at (or after) the project-search hit rather
  // than the top of the file.
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
    RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.matches.empty() || delta == 0) {
    return;
  }

  // Wrap around both ends (next on the last match jumps to the first, previous on
  // the first jumps to the last) — VSCode-style cyclic navigation.
  const int count = static_cast<int>(buffer_search.matches.size());
  const int current = static_cast<int>(buffer_search.selected_index);
  buffer_search.selected_index =
      static_cast<std::size_t>(((current + delta) % count + count) % count);
  RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  if (context_.current_project_state.overlay.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
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

  if (buffer_search.regex) {
    // Regex mode: expand the replacement for THIS match against the whole '\n'-joined
    // buffer (so capture groups + lookarounds resolve in full context and a multi-
    // line match works), then apply it to the match span.
    const std::string& query = buffer_search.query.text();
    const util::CompiledRegex pattern = CompileBufferSearchRegex(query, buffer_search.match_case);
    if (!pattern.valid() || match.start.line >= viewport->lines().LineCount()) {
      return;
    }
    const std::string content = BuildWholeBufferContent(*viewport);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < match.start.line; ++i) {
      offset += viewport->lines().LineLength(i) + 1;  // + newline
    }
    offset += match.start.column;
    const std::optional<std::string> expanded =
        pattern.ExpandMatchAt(content, offset, buffer_search.replace_text.text());
    if (!expanded.has_value() || !viewport->ReplaceRange(match, *expanded)) {
      return;
    }
  } else if (!viewport->ReplaceRange(match, buffer_search.replace_text.text())) {
    return;
  }

  RefreshBufferSearch();
  if (!buffer_search.matches.empty()) {
    buffer_search.selected_index =
        std::min(buffer_search.selected_index, buffer_search.matches.size() - 1);
    RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.query.text().empty()) {
    return;
  }

  buffer_search.preserve_temporarily_expanded_folds = true;
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    if (buffer_search.regex) {
      ApplyBufferRegexReplaceAll(*viewport, buffer_search.query.text(),
                                 buffer_search.replace_text.text(), buffer_search.match_case);
    } else {
      // TD-2026-07-17A-028: RefreshBufferSearch already computed the exact match
      // set (same case-insensitive fold ReplaceAll uses) for this query over the
      // current content. When that set is fresh (same viewport + content revision +
      // query) and complete (below the retained-match cap, so not truncated), apply
      // it as one grouped range edit instead of re-scanning + re-folding the whole
      // document. Any inconsistency makes ReplaceAllRanges return nullopt, and we
      // fall back to the self-contained scanning ReplaceAll.
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
        // TextViewport::ReplaceAll is the case-insensitive whole-needle scan, so it
        // can only stand in for the default options. Under Aa/ab, rescan here and
        // apply the ranges rather than silently replacing more than was highlighted.
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
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RevealBufferSearchMatch(const editor::SelectionRange& match) {
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return;
  }
  viewport->MoveCursorTo(match.start.line, match.start.column);

  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
  if (model == nullptr) {
    return;
  }

  bool changed = false;
  // Walk the collapsed set, not the resolved ranges: a fold hiding this match can
  // be anywhere in the document, including outside the window the last refresh
  // resolved. Collect first, then expand -- expanding mutates the set.
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
    RequestEditorSurfaceRedraw();
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
        viewport->path().lexically_normal() == buffer_search.temporarily_expanded_fold_tab_path) {
      bool changed = false;
      // CollapseRange, not Collapse(opener): the fold may no longer be inside the
      // resolved window now that the viewport has moved to the match.
      for (auto it = buffer_search.temporarily_expanded_folds.rbegin();
           it != buffer_search.temporarily_expanded_folds.rend(); ++it) {
        changed = model->CollapseRange(*it) || changed;
      }
      if (changed) {
        RequestEditorSurfaceRedraw();
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

}  // namespace microide::workspace
