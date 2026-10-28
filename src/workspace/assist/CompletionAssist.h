#pragma once

#include <functional>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/AssistProviderMerge.h"

namespace microide::workspace {

// The completion list: explicit (Ctrl+Space), automatic (a trigger character or
// quick suggestions), and the insert-snippet list, which shares its session.
class CompletionAssist {
 public:
  // The shell callbacks only this feature reads (AssistHost::Operations has the
  // shared ones).
  struct Port {
    // Does anything (a language server, a plugin provider) offer completion for
    // this language? Gates quick suggestions, so typing prose in a file nothing
    // completes asks no one.
    std::function<bool(std::string_view)> has_completion_provider;
    // The trigger characters a plugin completion provider registered for this
    // language (`ctx.completion.add{ trigger_characters = "." }`); empty if none.
    std::function<std::string(std::string_view)> plugin_completion_trigger_characters;
  };

  explicit CompletionAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool ShowCompletionOverlay(std::string* error_message = nullptr);
  // The editor took a keystroke while the completion list is up (the list never
  // owns the caret; see CompletionSessionState). Extends the word, refilters the
  // list locally, re-asks the sources only when the server called its list
  // incomplete, and closes the list once the caret has left the word -- a
  // non-word character, a backspace past its start, another line, a selection.
  void FollowCompletionCaret();
  // Text was typed into the editor. Follows an open list; otherwise opens one
  // automatically (TD-2026-10-07-321) on a server trigger character, or on the
  // first character of a word when `editor.quickSuggestions` is on -- once per
  // word, hidden until it has rows.
  void AfterTyping(std::string_view typed);
  bool ApplySelectedCompletion();
  bool ShowInsertSnippetOverlay(std::string* error_message = nullptr);

 private:
  // Per-request state for the LSP-primary *concurrent* provider model: a plugin
  // worker and the language server are queried at once, then their results are
  // merged (ranked, de-duplicated). Both callbacks run on the UI mailbox drain,
  // so no locking is needed.
  struct Merge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CompletionSessionItem> lsp_items;
    std::vector<CompletionSessionItem> plugin_items;
    // Request generation: a callback for an OLDER request (slower to return) must not
    // overwrite the items a newer same-file request already published. (TD-16-65.)
    std::uint64_t generation = 0;
    // The server's `isIncomplete` for `lsp_items`.
    bool lsp_incomplete = false;
    // Caret column the request was made at (CompletionSessionState::request_column).
    std::size_t request_column = 0;
  };

  // Reset the session to an empty list anchored at the word before the caret.
  CompletionSessionState& ResetSession(const editor::TextViewport& viewport, bool automatic,
                                       const char* source);
  // Transform a source's raw results into the shared overlay item type.
  std::vector<CompletionSessionItem> TransformPluginCompletions(
      const std::vector<plugin::PluginHost::CompletionCandidate>& items) const;
  std::vector<CompletionSessionItem> TransformLspCompletions(
      const std::vector<LspClient::CompletionItem>& items,
      lsp_encoding::PositionEncoding encoding) const;
  // Query both completion sources for the word at the caret. `refresh` keeps the
  // list on screen (a re-ask for an incomplete list) instead of opening it empty.
  void RequestCompletions(editor::TextViewport& viewport, bool refresh,
                          LspClient::CompletionTrigger trigger = LspClient::CompletionTrigger::kInvoked,
                          char trigger_character = '\0');
  // Start a hidden, automatic session at the caret (see CompletionSessionState::automatic).
  void StartAutomaticCompletion(editor::TextViewport& viewport, char trigger_character);
  // Narrow `session.items` into `session.visible` by the word between the anchor
  // and the caret. Returns false when the caret is no longer in that word.
  // `keep_selection`: the selected ITEM stays selected if it is still visible
  // (accepting re-filters first, and must accept what is highlighted); otherwise
  // the selection goes to the best match, as typing does.
  bool RefilterCompletion(CompletionSessionState& session, const editor::TextViewport& viewport,
                          bool keep_selection = false);
  // Merge both sources into the (already-open) overlay session. Called on each
  // source's arrival; publishes the ranked union so results appear as soon as
  // either source answers, LSP-first when a server serves the language.
  void PublishMerge(const std::shared_ptr<Merge>& merge, const std::filesystem::path& request_path);

  AssistHost& host_;
  Port port_;
  // Monotonic request generation: a new request bumps it; an async callback
  // carries the generation it was dispatched under and drops its result when a
  // newer request has since superseded it. (TD-2026-07-16-65.)
  std::uint64_t request_generation_ = 0;
  // RankCompletionCandidates' (score, index) buffer, kept so a keystroke's
  // refilter reuses it instead of allocating.
  std::vector<std::pair<int, std::uint32_t>> rank_scratch_;
  // Where the last automatic request's word began, so a word is asked about once
  // however many characters are typed into it.
  std::optional<editor::TextPosition> auto_anchor_;
};

}  // namespace microide::workspace
