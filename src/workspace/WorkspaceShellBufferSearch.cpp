#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

// Applies a regex replace-all across `viewport` as ONE undo group. Re-scans the
// live buffer (so the match set is fresh even if the document was edited while the
// find widget stayed open), expands every match's replacement against the ORIGINAL
// line text up-front, then applies the edits bottom-to-top (descending position) so
// each not-yet-applied match keeps valid coordinates. A per-match expansion failure
// (bad replacement escape) aborts before any edit is applied — nothing is written.
void ApplyBufferRegexReplaceAll(editor::TextViewport& viewport,
                                const std::string& query,
                                std::string_view replacement) {
  if (query.empty()) {
    return;
  }
  const std::uint32_t options = UsesCaseSensitiveLiteralMatch(query) ? 0u : PCRE2_CASELESS;
  const util::CompiledRegex pattern(query, options);
  if (!pattern.valid()) {
    return;
  }
  const std::vector<editor::SelectionRange> matches =
      FindRegexSearchMatches(viewport.lines(), pattern);
  if (matches.empty()) {
    return;
  }

  // Pre-expand every replacement against the original buffer so a rightward edit on
  // the same line cannot perturb a later expansion's lookaround context.
  std::vector<std::string> expansions;
  expansions.reserve(matches.size());
  for (const editor::SelectionRange& match : matches) {
    if (match.start.line >= viewport.lines().LineCount()) {
      return;
    }
    std::optional<std::string> expanded = pattern.ExpandMatchAt(
        viewport.lines().LineView(match.start.line), match.start.column, replacement);
    if (!expanded.has_value()) {
      return;  // bad replacement escape -> abort with no edits
    }
    expansions.push_back(std::move(*expanded));
  }

  viewport.BeginUndoGroup();
  for (std::size_t i = matches.size(); i-- > 0;) {
    viewport.ReplaceRange(matches[i], expansions[i]);
  }
  viewport.EndUndoGroup();
}

}  // namespace

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
    // Regex mode: smart-case (an uppercase letter in the query forces
    // case-sensitivity, matching project search), full-scan every keystroke. An
    // invalid pattern yields no matches (the widget shows 0/0). The literal refine
    // cache is bypassed and invalidated so switching back to literal rescans.
    const std::uint32_t options = UsesCaseSensitiveLiteralMatch(query) ? 0u : PCRE2_CASELESS;
    const util::CompiledRegex pattern(query, options);
    buffer_search.matches =
        pattern.valid() ? FindRegexSearchMatches(buffer, pattern)
                        : std::vector<editor::SelectionRange>{};
    incremental.valid = false;
  } else {
    const bool can_refine = incremental.valid &&
                            incremental.viewport == static_cast<const void*>(viewport) &&
                            incremental.content_revision == content_revision &&
                            !incremental.query.empty() &&
                            QueryExtendsCaseInsensitive(incremental.query, query);
    buffer_search.matches = can_refine
                                ? RefineLiteralSearchMatches(buffer, query, buffer_search.matches)
                                : FindLiteralSearchMatches(buffer, query);
    incremental.valid = true;
    incremental.viewport = viewport;
    incremental.content_revision = content_revision;
    incremental.query = query;
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
    // Regex mode: expand the replacement for THIS match (capture groups resolve in
    // full line context, so lookarounds work), then apply it to the match span.
    const std::string& query = buffer_search.query.text();
    const std::uint32_t options = UsesCaseSensitiveLiteralMatch(query) ? 0u : PCRE2_CASELESS;
    const util::CompiledRegex pattern(query, options);
    if (!pattern.valid() || match.start.line >= viewport->lines().LineCount()) {
      return;
    }
    const std::optional<std::string> expanded = pattern.ExpandMatchAt(
        viewport->lines().LineView(match.start.line), match.start.column,
        buffer_search.replace_text.text());
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
                                 buffer_search.replace_text.text());
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
          buffer_search.matches.size() < kMaxBufferSearchMatches;
      const bool applied =
          matches_fresh &&
          viewport->ReplaceAllRanges(buffer_search.matches, buffer_search.replace_text.text())
              .has_value();
      if (!applied) {
        viewport->ReplaceAll(buffer_search.query.text(), buffer_search.replace_text.text());
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
  for (std::size_t i = 0; i < model->ranges().size() && i < model->collapsed_flags().size(); ++i) {
    if (!model->collapsed_flags()[i]) {
      continue;
    }
    const auto& range = model->ranges()[i];
    if (match.start.line <= range.opener_line || match.start.line > range.closer_line) {
      continue;
    }
    if (model->Expand(range.opener_line)) {
      if (buffer_search.temporarily_expanded_fold_openers.empty()) {
        buffer_search.temporarily_expanded_fold_tab_path = viewport->path().lexically_normal();
      }
      if (std::find(buffer_search.temporarily_expanded_fold_openers.begin(),
                    buffer_search.temporarily_expanded_fold_openers.end(),
                    range.opener_line) ==
          buffer_search.temporarily_expanded_fold_openers.end()) {
        buffer_search.temporarily_expanded_fold_openers.push_back(range.opener_line);
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
      !buffer_search.temporarily_expanded_fold_openers.empty();
  if (should_restore) {
    editor::TextViewport* viewport = ActiveEditorViewport();
    editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
    if (viewport != nullptr && model != nullptr &&
        viewport->path().lexically_normal() == buffer_search.temporarily_expanded_fold_tab_path) {
      bool changed = false;
      for (auto it = buffer_search.temporarily_expanded_fold_openers.rbegin();
           it != buffer_search.temporarily_expanded_fold_openers.rend(); ++it) {
        changed = model->Collapse(*it) || changed;
      }
      if (changed) {
        RequestEditorSurfaceRedraw();
      }
    }
  }

  buffer_search.temporarily_expanded_fold_openers.clear();
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
