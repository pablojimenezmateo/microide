#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/services/AssistService.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/ColumnSelection.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "util/JsonFormat.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {
constexpr const char* kGotoExpects = "Go to Line expects a positive 1-based line[:col], such as 12 or 12:5";
}

namespace {

bool SettingEnabled(const WorkspaceActionContext& context, std::string_view id, bool default_value) {
  return SettingFlagEnabled(context.GetSettingValue(id), default_value);
}

}

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteEdit(ActionId id,
                                                                 const std::vector<std::string>& args,
                                                                 ActionSource source,
                                                                 std::string* rejection_feedback) {
  (void)source;
  const auto reject = RejectInto(rejection_feedback);

  if (EditorEssentialsCapabilitySettingKey(id) != nullptr) {
    context_.ToggleEditorEssentialsCapability(id);
    return DispatchResult::Handled;
  }

  switch (id) {
    case ActionId::Completion: {
      std::string error_message;
      if (!context_.assist().completion().ShowCompletionOverlay(&error_message)) {
        return reject(error_message.empty() ? "No completions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::InsertSnippet: {
      std::string error_message;
      if (!context_.assist().completion().ShowInsertSnippetOverlay(&error_message)) {
        return reject(error_message.empty() ? "No snippets available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CodeActions: {
      std::string error_message;
      if (!context_.assist().code_actions().ShowOverlay(&error_message)) {
        return reject(error_message.empty() ? "No code actions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::FormatDocument: {
      std::string error_message;
      if (!context_.assist().refactor().FormatActiveDocument(&error_message)) {
        return reject(error_message.empty() ? "Formatting unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::FormatJson:
      return ExecuteFormatJson(id, args, source, rejection_feedback);
    case ActionId::RenameSymbol: {
      if (!args.empty()) {
        std::string error_message;
        if (!context_.assist().refactor().RenameSymbol(JoinCommandArguments(args, 0), &error_message)) {
          return reject(error_message.empty() ? "Rename unavailable" : error_message);
        }
        return DispatchResult::Handled;
      }
      context_.OpenRenameSymbolPrompt();
      return DispatchResult::Handled;
    }
    case ActionId::GoToNextDiagnostic:
    case ActionId::GoToPreviousDiagnostic: {
      std::string error_message;
      if (!context_.StepDiagnostic(id == ActionId::GoToNextDiagnostic ? 1 : -1, &error_message)) {
        return reject(error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToNextProblemInFiles:
    case ActionId::GoToPreviousProblemInFiles: {
      const std::optional<editor::PublishedDiagnostic> target =
          context_.ProblemInFiles(id == ActionId::GoToNextProblemInFiles ? 1 : -1);
      if (!target.has_value()) {
        return reject("No problems");
      }
      if (!Execute(ActionId::Reveal,
                   {target->path.string(), std::to_string(target->range.start.line + 1) + ":" +
                                               std::to_string(target->range.start.column + 1)},
                   source)) {
        return DispatchResult::Rejected;
      }
      context_.NotifyDiagnostic(*target);
      return DispatchResult::Handled;
    }
    case ActionId::LanguageMode: {
      if (const editor::TextViewport* viewport = context_.ActiveNavigableViewport(); viewport == nullptr) {
        return reject("Change Language Mode needs an open editor");
      }
      std::string error;
      if (!context_.ChangeLanguageMode(JoinCommandArguments(args, 0), &error)) {
        return reject(error.empty() ? "Cannot change the language" : error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToSymbolInEditor:
      if (const editor::TextViewport* viewport = context_.ActiveNavigableViewport();
          viewport == nullptr || viewport->path().empty()) {
        return reject("Go to Symbol needs an open file");
      }
      context_.OpenDocumentSymbolPicker(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::GoToDefinition: {
      std::string error_message;
      if (!context_.assist().navigation().GoToDefinition(&error_message)) {
        return reject(error_message.empty() ? "Definition unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToTypeDefinition: {
      std::string error_message;
      if (!context_.assist().navigation().GoToTypeDefinition(&error_message)) {
        return reject(error_message.empty() ? "Type definition unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToImplementation: {
      std::string error_message;
      if (!context_.assist().navigation().GoToImplementation(&error_message)) {
        return reject(error_message.empty() ? "Implementation unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToDeclaration: {
      std::string error_message;
      if (!context_.assist().navigation().GoToDeclaration(&error_message)) {
        return reject(error_message.empty() ? "Declaration unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::FindReferences: {
      std::string error_message;
      if (!context_.FindLspReferences(&error_message)) {
        return reject(error_message.empty() ? "References unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CallHierarchy: {
      bool incoming = true;
      if (!args.empty()) {
        if (args[0] == "outgoing") {
          incoming = false;
        } else if (args[0] != "incoming") {
          return reject("Usage: call-hierarchy [incoming|outgoing]");
        }
      }
      std::string error_message;
      if (!context_.ShowCallHierarchy(incoming, &error_message)) {
        return reject(error_message.empty() ? "Call hierarchy unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::WorkspaceSymbol:
      context_.OpenWorkspaceSymbolPicker(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::SignatureHelp: {
      std::string error_message;
      if (!context_.assist().signature_help().Show(&error_message)) {
        return reject(error_message.empty() ? "No signature help available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Goto:
    case ActionId::Jump: {
      if (context_.ActiveTabIsCompare() || context_.ActiveTabIsMerge()) {
        return DispatchResult::Handled;
      }
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        if (id == ActionId::Goto && args.empty()) {
          context_.OpenGoToLinePrompt();
          return DispatchResult::Handled;
        }
        return reject(id == ActionId::Goto ? kGotoExpects
                                           : "jump expects a signed line[:col], such as +3 or -2:1");
      }

      if (id == ActionId::Goto && request->requested_line <= 0) {
        return reject(kGotoExpects);
      }

      context_.ExecuteLineNavigation(*request, id == ActionId::Jump);
      return DispatchResult::Handled;
    }
    case ActionId::SelectAll:
      context_.SelectAll();
      return DispatchResult::Handled;
    case ActionId::Undo:
      context_.Undo();
      return DispatchResult::Handled;
    case ActionId::Redo:
      context_.Redo();
      return DispatchResult::Handled;
    case ActionId::CopySelection: {
      bool whole_line = false;
      const std::string text = context_.CopySelectionText(&whole_line);
      if (!text.empty()) {
        context_.WriteClipboardText(text);
        context_.WritePrimarySelectionText(text);
        context_.RememberLineClipboardCopy(text, whole_line);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = context_.LastTerminalCommandText();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = context_.SelectionTextWithContext();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
        context_.WritePrimarySelectionText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyReference:
    case ActionId::TerminalInsertReference: {
      const bool into_terminal = id == ActionId::TerminalInsertReference;
      const std::optional<std::string> reference = context_.ActiveEditorReference(into_terminal);
      if (!reference.has_value()) {
        return reject("The active tab has no file to refer to");
      }
      if (!into_terminal) {
        context_.WriteClipboardText(*reference);
        return DispatchResult::Handled;
      }
      if (!context_.TypeIntoActiveTerminal(*reference + " ")) {
        return reject("No terminal is running to take the reference");
      }
      return DispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      context_.CutSelection();
      return DispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      context_.PasteClipboard();
      return DispatchResult::Handled;
    }
    case ActionId::InsertText: {
      std::string text;
      for (const std::string& arg : args) {
        if (!text.empty()) text.push_back(' ');
        text += arg;
      }
      if (text.empty()) {
        return DispatchResult::Handled;
      }
      const editor::TextViewport* target = context_.ActiveNavigableViewport();
      const std::uint64_t refused_before = target != nullptr ? target->refused_edits() : 0;
      context_.InsertText(std::move(text));
      if (target != nullptr && context_.ActiveNavigableViewport() == target &&
          target->refused_edits() > refused_before) {
        return reject("Cannot edit in read-only editor");
      }
      return DispatchResult::Handled;
    }
    case ActionId::PressKey: {
      SDL_Keycode key = SDLK_UNKNOWN;
      SDL_Keymod modifiers = SDL_KMOD_NONE;
      if (args.empty() || !ParseKeyChord(args[0], &key, &modifiers)) {
        return reject("usage: key <chord> [count], e.g. key enter, key shift+tab 2");
      }
      int count = 1;
      if (args.size() > 1) {
        const std::optional<int> parsed = util::ParseInt(args[1]);
        if (!parsed.has_value() || *parsed < 1 || *parsed > 1000) {
          return reject("key: count must be 1-1000");
        }
        count = *parsed;
      }
      for (int i = 0; i < count; ++i) {
        context_.PressKey(key, modifiers);
      }
      return DispatchResult::Handled;
    }
    case ActionId::ShowHover:
      if (!context_.ShowHoverAtCaret()) {
        return reject("No hover at the caret");
      }
      return DispatchResult::Handled;
    case ActionId::Pointer:
      return ExecutePointer(id, args, source, rejection_feedback);
    case ActionId::Compose:
      return ExecuteCompose(args, rejection_feedback);
    case ActionId::TestsDiscover: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      std::string error_message;
      if (!context_.DiscoverTestsForActiveBuffer(&error_message)) {
        return reject(error_message.empty() ? "Test discovery failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::JumpToMatchingBracket: {
      auto* viewport = context_.ActiveNavigableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      auto match = editor::FindBracketMatch(*viewport, viewport->cursor_line(),
                                            viewport->cursor_column());
      if (!match) return DispatchResult::Handled;
      if (match->caret_at_opener) {
        viewport->JumpCursorTo(match->close_line, match->close_column, false);
      } else {
        viewport->JumpCursorTo(match->open_line, match->open_column, false);
      }
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::ColumnSelectUp:
    case ActionId::ColumnSelectDown:
    case ActionId::ColumnSelectLeft:
    case ActionId::ColumnSelectRight: {
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) {
        return DispatchResult::Handled;
      }
      const editor::ColumnSelectDirection direction =
          id == ActionId::ColumnSelectUp     ? editor::ColumnSelectDirection::Up
          : id == ActionId::ColumnSelectDown ? editor::ColumnSelectDirection::Down
          : id == ActionId::ColumnSelectLeft ? editor::ColumnSelectDirection::Left
                                             : editor::ColumnSelectDirection::Right;
      const editor::ColumnSelectionState before = viewport->column_selection();
      const editor::TextPosition caret{viewport->cursor_line(), viewport->cursor_visual_column()};
      const std::size_t lo =
          before.active ? std::min(before.anchor.line, before.cursor.line) : caret.line;
      const std::size_t hi =
          before.active ? std::max(before.anchor.line, before.cursor.line) : caret.line;
      const editor::ColumnSelectionState after = editor::StepColumnSelection(
          before, direction, caret, viewport->line_count(),
          viewport->MaxVisualWidthInSpan(lo, hi));
      viewport->SetBoxSelectionVisual(after.anchor.line, after.anchor.column, after.cursor.line,
                                      after.cursor.column);
      viewport->SetColumnSelection(after);
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
    case ActionId::MoveLineUp:
    case ActionId::MoveLineDown:
    case ActionId::DuplicateLine:
    case ActionId::CopyLineUp:
    case ActionId::InsertLineBelow:
    case ActionId::InsertLineAbove:
    case ActionId::DeleteLine:
    case ActionId::IndentLines:
    case ActionId::OutdentLines:
    case ActionId::JoinLines:
    case ActionId::SortLinesAscending:
    case ActionId::SortLinesDescending:
      return ExecuteSortLines(id, args, source, rejection_feedback);
    case ActionId::ExpandSelection:
    case ActionId::ShrinkSelection: {
      auto* viewport = context_.ActiveNavigableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      if (id == ActionId::ExpandSelection ? viewport->ExpandSelection() : viewport->ShrinkSelection()) {
        context_.NotifyEditorCaretMoved();
      }
      return DispatchResult::Handled;
    }
    case ActionId::AddCursorAbove:
    case ActionId::AddCursorBelow:
    case ActionId::AddCursorsToLineEnds: {
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      const bool changed =
          id == ActionId::AddCursorsToLineEnds
              ? viewport->AddCaretsAtSelectedLineEnds()
              : viewport->AddCaretVertical(id == ActionId::AddCursorBelow ? 1 : -1);
      if (changed) {
        context_.NotifyEditorCaretMoved();
      }
      return DispatchResult::Handled;
    }
    case ActionId::AddCursorAtNextMatch:
    case ActionId::AddCursorAtAllMatches:
      return ExecuteAddCursorAtMatch(id, args, source, rejection_feedback);
    case ActionId::Fold:
    case ActionId::Unfold:
    case ActionId::ToggleFoldAtCursor:
    case ActionId::FoldAll:
    case ActionId::UnfoldAll: {
      auto* model = context_.EnsureActiveFoldingModelFresh();
      if (model == nullptr) return DispatchResult::Handled;
      auto* viewport = context_.ActiveNavigableViewport();
      bool changed = false;
      if (id == ActionId::FoldAll) {
        if (viewport == nullptr) return DispatchResult::Handled;
        model->ResolveAllFolds(viewport->lines(), viewport);
        changed = model->CollapseAllResolved();
      } else if (id == ActionId::UnfoldAll) {
        if (model->has_any_collapsed_fold()) {
          model->ExpandAll();
          changed = true;
        }
      } else {
        if (viewport == nullptr) return DispatchResult::Handled;
        const std::optional<editor::FoldRange> target =
            model->InnermostFoldContaining(viewport->cursor_line());
        if (!target) return DispatchResult::Handled;
        if (id == ActionId::Fold) {
          changed = model->Collapse(target->opener_line);
        } else if (id == ActionId::Unfold) {
          changed = model->Expand(target->opener_line);
        } else {
          changed = model->ToggleFold(target->opener_line);
        }
      }
      if (changed) {
        context_.NotifyEditorViewportChanged(false);
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteAddCursorAtMatch(ActionId id,
                                                                             const std::vector<std::string>& ,
                                                                             ActionSource ,
                                                                             std::string* ) {
  if (!SettingEnabled(context_, "editor.multicursor.add_at_match.enabled", true)) {
    return DispatchResult::Handled;
  }
  auto* viewport = context_.ActiveEditableViewport();
  if (viewport == nullptr)
    return DispatchResult::Handled;
  if (!viewport->has_selection()) {
    viewport->SelectWordAtCursor();
    if (id == ActionId::AddCursorAtNextMatch) {
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
  }
  auto sel = viewport->selection_range();
  if (!sel || sel->start.line != sel->end.line || sel->start.column == sel->end.column) {
    return DispatchResult::Handled;
  }
  const editor::TextBuffer& lines = viewport->lines();
  if (sel->start.line >= lines.LineCount())
    return DispatchResult::Handled;
  const std::string_view line = lines.LineView(sel->start.line);
  std::size_t a = std::min(sel->start.column, sel->end.column);
  std::size_t b = std::max(sel->start.column, sel->end.column);
  if (b > line.size())
    return DispatchResult::Handled;
  const std::string needle(line.substr(a, b - a));
  if (needle.empty())
    return DispatchResult::Handled;
  const std::string_view needle_view = needle;
  const bool case_sensitive = SettingEnabled(context_, "editor.search.case_sensitive", false);
  if (id == ActionId::AddCursorAtNextMatch) {
    const std::span<const editor::TextViewportUndoHistory::SecondaryCaret> taken =
        viewport->secondary_caret_range_view();
    std::size_t seed_line = sel->start.line;
    std::size_t seed_start = a;
    std::size_t seed_end = b;
    const editor::TextPosition primary_start{sel->start.line, a};
    const auto precedes = [](const editor::TextPosition& lhs, const editor::TextPosition& rhs) {
      return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
    };
    const editor::TextViewportUndoHistory::SecondaryCaret* last_added = nullptr;
    bool wrapped = false;
    for (const auto& caret : taken) {
      if (!caret.selection_anchor.has_value()) {
        continue;
      }
      const bool before_primary = precedes(caret.position, primary_start);
      if (last_added == nullptr || (before_primary && !wrapped) ||
          (before_primary == wrapped && precedes(last_added->position, caret.position))) {
        last_added = &caret;
        wrapped = before_primary;
      }
    }
    if (last_added != nullptr && last_added->selection_anchor->line == last_added->position.line) {
      seed_line = last_added->position.line;
      seed_start = std::min(last_added->selection_anchor->column, last_added->position.column);
      seed_end = std::max(last_added->selection_anchor->column, last_added->position.column);
    }
    std::optional<editor::SelectionRange> fresh;
    for (std::size_t hops = 0; hops <= taken.size(); ++hops) {
      const auto next =
          FindNextLiteralMatchAfterSeedWrapOnce(lines, seed_line, seed_start, seed_end, needle_view, case_sensitive);
      if (!next.has_value() || (next->line == sel->start.line && next->column == a)) {
        break;
      }
      const editor::SelectionRange range{
          editor::TextPosition{next->line, next->column},
          editor::TextPosition{next->line, next->column + needle_view.size()},
      };
      const bool already_a_caret =
          std::any_of(taken.begin(), taken.end(), [&](const editor::TextViewportUndoHistory::SecondaryCaret& caret) {
            return caret.position == range.end && caret.selection_anchor == range.start;
          });
      if (!already_a_caret) {
        fresh = range;
        break;
      }
      seed_line = range.start.line;
      seed_start = range.start.column;
      seed_end = range.end.column;
    }
    if (fresh.has_value()) {
      viewport->AddSecondaryCaretWithRange(*fresh);
    }
  } else {
    AddCursorMatchScan scan = CollectAddCursorMatchRanges(lines, sel->start.line, a, needle_view, case_sensitive);
    const std::size_t match_count = scan.ranges.size();
    const bool truncated = scan.truncated;
    viewport->SetSecondaryCaretsWithRanges(scan.ranges);
    if (truncated) {
      context_.Notify(NotificationService::Tone::Warning,
                      "Added cursors at the first " + std::to_string(match_count) + " matches (more remain)");
    }
  }
  context_.Focus(FocusRequestTarget::Editor);
  context_.NotifyEditorCaretMoved();
  return DispatchResult::Handled;
}

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteSortLines(ActionId id,
                                                                      const std::vector<std::string>& ,
                                                                      ActionSource ,
                                                                      std::string* ) {
  auto* viewport = context_.ActiveEditableViewport();
  if (viewport == nullptr)
    return DispatchResult::Handled;
  if (const char* gate = ShapingActionGateSettingKey(id); gate != nullptr && !SettingEnabled(context_, gate, true)) {
    return DispatchResult::Handled;
  }
  bool changed = false;
  switch (id) {
    case ActionId::ToggleLineComment: {
      const editor::LanguageContractView& lc = viewport->language_contract_view();
      const std::string_view line_marker =
          lc.line_comment.empty() ? std::string_view("//") : std::string_view(lc.line_comment);
      changed = editor::ToggleLineComment(*viewport, line_marker);
      break;
    }
    case ActionId::ToggleBlockComment: {
      const editor::LanguageContractView& lc = viewport->language_contract_view();
      const bool has_block = !lc.block_comment_open.empty() && !lc.block_comment_close.empty();
      const std::string_view open = has_block ? std::string_view(lc.block_comment_open) : std::string_view("/*");
      const std::string_view close = has_block ? std::string_view(lc.block_comment_close) : std::string_view("*/");
      changed = editor::ToggleBlockComment(*viewport, open, close);
      break;
    }
    case ActionId::MoveLineUp:
      changed = editor::MoveLineUp(*viewport);
      break;
    case ActionId::MoveLineDown:
      changed = editor::MoveLineDown(*viewport);
      break;
    case ActionId::DuplicateLine:
      changed = editor::CopyLines(*viewport, true);
      break;
    case ActionId::CopyLineUp:
      changed = editor::CopyLines(*viewport, false);
      break;
    case ActionId::InsertLineBelow:
      changed = editor::InsertLineBelow(*viewport);
      break;
    case ActionId::InsertLineAbove:
      changed = editor::InsertLineAbove(*viewport);
      break;
    case ActionId::DeleteLine:
      changed = editor::DeleteLine(*viewport);
      break;
    case ActionId::IndentLines:
      changed = editor::IndentSelection(*viewport);
      break;
    case ActionId::OutdentLines:
      changed = editor::OutdentSelection(*viewport);
      break;
    case ActionId::JoinLines:
      changed = editor::JoinLinesAtCarets(*viewport);
      break;
    case ActionId::SortLinesAscending:
      changed = editor::SortLines(*viewport, true);
      break;
    case ActionId::SortLinesDescending:
      changed = editor::SortLines(*viewport, false);
      break;
    default:
      break;
  }
  if (changed) {
    context_.NotifyEditorViewportChanged(true);
  }
  return DispatchResult::Handled;
}

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteCompose(const std::vector<std::string>& args,
                                                                    std::string* rejection_feedback) {
  const auto reject = RejectInto(rejection_feedback);
  static constexpr std::string_view kUsage = "usage: compose edit <start> <length> <text>, compose commit <text>, or "
                                             "compose cancel";
  const std::string verb = args.empty() ? std::string() : util::ToLowerAscii(args[0]);
  const std::size_t text_from = verb == "edit" ? 3 : 1;
  std::string text;
  for (std::size_t i = text_from; i < args.size(); ++i) {
    text += (i > text_from ? " " : "") + args[i];
  }
  const auto composition = [&](const std::string& shown, Sint32 start, Sint32 length) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = shown.c_str();
    event.edit.start = start;
    event.edit.length = length;
    context_.InjectInputEvent(event, SDL_KMOD_NONE);
  };
  if (verb == "edit") {
    const std::optional<int> start = args.size() > 2 ? util::ParseInt(args[1]) : std::nullopt;
    const std::optional<int> length = args.size() > 2 ? util::ParseInt(args[2]) : std::nullopt;
    if (!start.has_value() || !length.has_value() || text.empty()) {
      return reject(std::string(kUsage));
    }
    composition(text, *start, *length);
    return DispatchResult::Handled;
  }
  if (verb == "commit" && !text.empty()) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text.c_str();
    context_.InjectInputEvent(event, SDL_KMOD_NONE);
    composition(std::string(), 0, 0);
    return DispatchResult::Handled;
  }
  if (verb == "cancel" && args.size() == 1) {
    composition(std::string(), 0, 0);
    return DispatchResult::Handled;
  }
  return reject(std::string(kUsage));
}

ActionCoordinator::DispatchResult ActionCoordinator::ExecutePointer(ActionId ,
                                                                    const std::vector<std::string>& args,
                                                                    ActionSource ,
                                                                    std::string* rejection_feedback) {
  const auto reject = RejectInto(rejection_feedback);
  static constexpr std::string_view kUsage =
      "usage: pointer [mods+]<move|down|up|click|wheel> <x> <y> [button|notches] [clicks], "
      "or pointer drop <x> <y> <path>";
  if (args.size() < 3) {
    return reject(std::string(kUsage));
  }
  const std::string_view chord = args[0];
  const std::size_t plus = chord.rfind('+');
  const std::string verb = util::ToLowerAscii(plus == std::string_view::npos ? chord : chord.substr(plus + 1));
  SDL_Keymod modifiers = SDL_KMOD_NONE;
  if (plus != std::string_view::npos) {
    SDL_Keycode ignored = SDLK_UNKNOWN;
    if (!ParseKeyChord(std::string(chord.substr(0, plus + 1)) + "a", &ignored, &modifiers)) {
      return reject(std::string(kUsage));
    }
  }
  const std::optional<float> x = util::ParseFloat(args[1]);
  const std::optional<float> y = util::ParseFloat(args[2]);
  if (!x.has_value() || !y.has_value()) {
    return reject("pointer: x and y are numbers (layout units)");
  }
  const auto parse_button = [](std::string_view name) -> std::optional<Uint8> {
    const std::string lower = util::ToLowerAscii(name);
    if (lower == "left" || lower == "1")
      return SDL_BUTTON_LEFT;
    if (lower == "middle" || lower == "2")
      return SDL_BUTTON_MIDDLE;
    if (lower == "right" || lower == "3")
      return SDL_BUTTON_RIGHT;
    return std::nullopt;
  };
  std::optional<Uint8> button;
  if (args.size() > 3 && verb != "wheel" && verb != "drop") {
    button = parse_button(args[3]);
    if (!button.has_value()) {
      return reject("pointer: button is left, middle or right");
    }
  }
  int clicks = 1;
  if (args.size() > 4) {
    const std::optional<int> parsed = util::ParseInt(args[4]);
    if (!parsed.has_value() || *parsed < 1 || *parsed > 3) {
      return reject("pointer: clicks must be 1-3");
    }
    clicks = *parsed;
  }
  const auto move = [&](SDL_MouseButtonFlags held) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = *x;
    event.motion.y = *y;
    event.motion.state = held;
    context_.InjectInputEvent(event, modifiers);
  };
  const auto press = [&](Uint8 which, bool down, int count) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = which;
    event.button.down = down;
    event.button.clicks = static_cast<Uint8>(count);
    event.button.x = *x;
    event.button.y = *y;
    context_.InjectInputEvent(event, modifiers);
  };
  if (verb == "move") {
    move(button.has_value() ? SDL_BUTTON_MASK(*button) : 0);
  } else if (verb == "down" || verb == "up" || verb == "click") {
    const Uint8 which = button.value_or(SDL_BUTTON_LEFT);
    move(verb == "up" ? SDL_BUTTON_MASK(which) : 0);
    if (verb == "click") {
      for (int count = 1; count <= clicks; ++count) {
        press(which, true, count);
        press(which, false, count);
      }
    } else {
      press(which, verb == "down", clicks);
    }
  } else if (verb == "drop") {
    if (args.size() < 4 || args[3].empty()) {
      return reject("usage: pointer drop <x> <y> <path>");
    }
    move(0);
    SDL_Event event{};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.x = *x;
    event.drop.y = *y;
    event.drop.data = args[3].c_str();
    context_.InjectInputEvent(event, modifiers);
  } else if (verb == "wheel") {
    const std::optional<int> notches = args.size() > 3 ? util::ParseInt(args[3]) : std::optional<int>(1);
    if (!notches.has_value() || *notches == 0 || *notches < -100 || *notches > 100) {
      return reject("pointer: wheel notches are -100..100, not 0 (positive scrolls down)");
    }
    move(0);
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.y = static_cast<float>(-*notches);
    event.wheel.integer_y = -*notches;
    event.wheel.mouse_x = *x;
    event.wheel.mouse_y = *y;
    context_.InjectInputEvent(event, modifiers);
  } else {
    return reject(std::string(kUsage));
  }
  return DispatchResult::Handled;
}

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteFormatJson(ActionId ,
                                                                       const std::vector<std::string>& args,
                                                                       ActionSource ,
                                                                       std::string* ) {
  if (!args.empty()) {
    if (!context_.HasProjectRoot()) {
      context_.Notify(NotificationService::Tone::Error, "No active project");
      return DispatchResult::Handled;
    }
    const std::optional<OpenPathRequest> request = BuildOpenPathRequest(args, context_.ProjectRoot());
    if (!request.has_value()) {
      context_.Notify(NotificationService::Tone::Error, "format-json requires a valid path");
      return DispatchResult::Handled;
    }
    std::string open_error;
    if (!context_.OpenPath(request->path, &open_error)) {
      context_.Notify(NotificationService::Tone::Error, open_error.empty() ? "Could not open file" : open_error);
      return DispatchResult::Handled;
    }
  }
  auto* viewport = context_.ActiveEditableViewport();
  if (viewport == nullptr) {
    context_.Notify(NotificationService::Tone::Warning, "No active buffer to format");
    return DispatchResult::Handled;
  }
  std::string source;
  viewport->lines().AppendWholeText(source);
  const std::string indent_unit =
      viewport->soft_tabs() ? std::string(viewport->indent_width(), ' ') : std::string("\t");
  const util::JsonFormatResult formatted = util::FormatJson(source, indent_unit);
  if (!formatted.ok) {
    std::size_t line = 1;
    std::size_t col = 1;
    for (std::size_t i = 0; i < formatted.error_offset && i < source.size(); ++i) {
      if (source[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    context_.Notify(NotificationService::Tone::Error,
                    "Invalid JSON (" + std::to_string(line) + ":" + std::to_string(col) + "): " + formatted.error);
    return DispatchResult::Handled;
  }
  if (formatted.text == source) {
    return DispatchResult::Handled;
  }
  std::vector<std::string> new_lines;
  {
    std::size_t start = 0;
    const std::string& text = formatted.text;
    for (std::size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        new_lines.emplace_back(text.substr(start, i - start));
        start = i + 1;
      }
    }
  }
  const std::size_t caret_line = viewport->cursor_line();
  const std::size_t caret_column = viewport->cursor_column();
  if (viewport->ReplaceLines(0, viewport->line_count(), std::move(new_lines),
                             true)) {
    viewport->JumpCursorTo(std::min(caret_line, viewport->line_count() - 1), caret_column);
    context_.NotifyEditorViewportChanged(true);
  }
  return DispatchResult::Handled;
}

}
