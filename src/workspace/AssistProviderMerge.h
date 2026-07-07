#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

// Pure, allocation-light decision helpers for the LSP-primary *concurrent*
// provider model. A plugin worker and the language server are queried at the
// same time (never serially); each source reports back independently on the UI
// mailbox drain. These helpers own the ranking / de-dup / navigation-choice
// logic so it stays unit-testable without the subprocess-backed LSP client.
//
// "Authoritative" means a language server serves the buffer's language. In that
// case LSP results rank first and win key ties, and single-result navigations
// wait for the server rather than acting on a plugin result that a slower LSP
// answer would override.
namespace microide::workspace::assist_merge {

// Tracks the two concurrent sources for one request: whether each is still
// outstanding and which one is authoritative. The result payloads live with the
// caller (their item types differ per capability); this only carries the
// bookkeeping the merge decision needs.
struct TwoSourceState {
  bool lsp_authoritative = false;
  bool lsp_pending = true;
  bool plugin_pending = true;

  bool AnyPending() const { return lsp_pending || plugin_pending; }
  bool AllResolved() const { return !lsp_pending && !plugin_pending; }
};

// Ranked, de-duplicated union of two already-transformed result lists. The
// authoritative source is passed as `primary`; its items come first and win key
// ties, and `secondary` contributes only items whose key has not already
// appeared. Per-source order is preserved. `key_of(item)` yields a comparable
// de-dup key (e.g. a completion label or code-action title). Lists are short
// (an overlay's worth), so a linear seen-scan beats hashing.
template <typename Item, typename KeyOf>
std::vector<Item> RankedUnion(const std::vector<Item>& primary,
                              const std::vector<Item>& secondary, KeyOf key_of) {
  using Key = std::decay_t<decltype(key_of(std::declval<const Item&>()))>;
  std::vector<Item> merged;
  merged.reserve(primary.size() + secondary.size());
  std::vector<Key> seen;
  seen.reserve(primary.size() + secondary.size());
  const auto append = [&](const std::vector<Item>& source) {
    for (const Item& item : source) {
      Key key = key_of(item);
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
        continue;
      }
      seen.push_back(std::move(key));
      merged.push_back(item);
    }
  };
  append(primary);
  append(secondary);
  return merged;
}

// Which source a single-result navigation (go-to-definition and friends) should
// use once a decision is reachable. The authoritative source is preferred; the
// other is consulted only after the preferred one has resolved *empty*. While
// the preferred source is still outstanding the answer is `Pending` (we must not
// navigate to a plugin hit that a pending LSP answer would override).
enum class NavChoice { Pending, UseLsp, UsePlugin, None };

inline NavChoice ChooseNavigation(bool lsp_authoritative, bool lsp_pending, bool lsp_has_result,
                                  bool plugin_pending, bool plugin_has_result) {
  const bool preferred_pending = lsp_authoritative ? lsp_pending : plugin_pending;
  const bool preferred_has = lsp_authoritative ? lsp_has_result : plugin_has_result;
  const bool other_pending = lsp_authoritative ? plugin_pending : lsp_pending;
  const bool other_has = lsp_authoritative ? plugin_has_result : lsp_has_result;

  if (preferred_pending) {
    return NavChoice::Pending;
  }
  if (preferred_has) {
    return lsp_authoritative ? NavChoice::UseLsp : NavChoice::UsePlugin;
  }
  // Preferred source resolved empty: fall through to the other one.
  if (other_pending) {
    return NavChoice::Pending;
  }
  if (other_has) {
    return lsp_authoritative ? NavChoice::UsePlugin : NavChoice::UseLsp;
  }
  return NavChoice::None;
}

}  // namespace microide::workspace::assist_merge
