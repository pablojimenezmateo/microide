#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/FlatDedupSet.h"

namespace microide::workspace::assist_merge {

struct TwoSourceState {
  bool lsp_authoritative = false;
  bool lsp_pending = true;
  bool plugin_pending = true;
  bool lsp_failed = false;

  bool AnyPending() const { return lsp_pending || plugin_pending; }
};

template <typename Key, typename = void>
struct IsStdHashable : std::false_type {};
template <typename Key>
struct IsStdHashable<
    Key, std::void_t<decltype(std::declval<std::hash<Key>>()(std::declval<const Key&>()))>>
    : std::true_type {};

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
  if (other_pending) {
    return NavChoice::Pending;
  }
  if (other_has) {
    return lsp_authoritative ? NavChoice::UsePlugin : NavChoice::UseLsp;
  }
  return NavChoice::None;
}

}
