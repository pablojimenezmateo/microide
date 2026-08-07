#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/FlatDedupSet.h"

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
  // The LSP request completed as a transport failure (timeout / server-gone /
  // protocol error) rather than an authoritative answer. When both sources end up
  // empty, this distinguishes "the server said there is nothing here" from "the
  // server never answered", so the UI does not report a false "No X found".
  bool lsp_failed = false;

  bool AnyPending() const { return lsp_pending || plugin_pending; }
  bool AllResolved() const { return !lsp_pending && !plugin_pending; }
};

// Detects whether `Key` can be used with std::hash (so the hash-set fast path
// below only instantiates for hashable keys; anything else keeps the linear
// scan, which never needs hashing).
template <typename Key, typename = void>
struct IsStdHashable : std::false_type {};
template <typename Key>
struct IsStdHashable<
    Key, std::void_t<decltype(std::declval<std::hash<Key>>()(std::declval<const Key&>()))>>
    : std::true_type {};

// Ranked, de-duplicated union of two already-transformed result lists. The
// authoritative source is passed as `primary`; its items come first and win key
// ties, and `secondary` contributes only items whose key has not already
// appeared. Per-source order is preserved. `key_of(item)` yields a comparable
// de-dup key (e.g. a completion label or code-action title).
//
// An overlay's worth of results is short, so a linear seen-scan (no hashing,
// cache-friendly) wins there. But providers are only bounded by harvest caps
// (LSP up to 5,000 rows, plugins up to 20,000 completions), so a verbose or
// hostile source set could make the linear scan O(total^2) — tens of millions
// of comparisons. Once the combined size crosses a small threshold, switch to a
// hash set so de-dup stays O(total).
template <typename Item, typename KeyOf>
std::vector<Item> RankedUnion(const std::vector<Item>& primary,
                              const std::vector<Item>& secondary, KeyOf key_of) {
  using Key = std::decay_t<decltype(key_of(std::declval<const Item&>()))>;
  const std::size_t total = primary.size() + secondary.size();
  std::vector<Item> merged;
  merged.reserve(total);

  if constexpr (IsStdHashable<Key>::value) {
    constexpr std::size_t kHashThreshold = 128;
    if (total >= kHashThreshold) {
      // A flat open-addressed set, not std::unordered_set: the node-per-key cost
      // was the whole cost of this branch (6,002 allocations to merge two
      // 6,000-item lists, one per distinct key), and the table is sized once from
      // `total` so this is two allocations regardless of list length.
      util::FlatDedupSet<Key> seen(total);
      const auto append = [&](const std::vector<Item>& source) {
        for (const Item& item : source) {
          if (!seen.Insert(key_of(item))) {
            continue;
          }
          merged.push_back(item);
        }
      };
      append(primary);
      append(secondary);
      return merged;
    }
  }

  std::vector<Key> seen;
  seen.reserve(total);
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
