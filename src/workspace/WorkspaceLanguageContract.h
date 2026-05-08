#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

struct LanguageBracketPair {
  std::string open;
  std::string close;
};

struct LanguageSnippet {
  std::string prefix;
  std::string label;
  std::string body;
};

// Per-language metadata that drives bracket matching, auto-close, surround,
// smart indent, comment toggling, snippet expansion, and the regex outline
// fallback. Hosts merge defaults + plugin contributions + user overrides into
// the resolved table; consumers read through `LanguageContractView`.
struct LanguageContract {
  std::string language_id;
  std::vector<LanguageBracketPair> bracket_pairs;
  std::vector<LanguageBracketPair> auto_close_pairs;
  std::vector<LanguageBracketPair> surround_pairs;
  std::string line_comment;
  LanguageBracketPair block_comment;
  // Patterns evaluated against the trimmed previous line; if any matches, the
  // newline path appends one indent unit. Patterns are literal substrings the
  // line ends with (e.g. "{", "(", "[", ":").
  std::vector<std::string> indent_after_open_patterns;
  // Single characters that, if typed when the current line is indent-only and
  // the leading indent exceeds zero, cause the leading indent to drop one unit
  // before the character is inserted.
  std::vector<std::string> dedent_on_close_chars;
  // True if pair operations should be inhibited inside string literals.
  bool inhibit_pairs_in_strings = true;
  // True if pair operations should be inhibited inside comments.
  bool inhibit_pairs_in_comments = true;
  // Optional regex outline patterns used when no LSP is available; each entry
  // is a regex matched against a single line; the captured group 1 is the
  // symbol name. Empty when not provided.
  std::vector<std::string> outline_regex_patterns;
  std::vector<LanguageSnippet> snippets;
};

// Non-owning view of a resolved contract; pointers are stable across the
// caller's frame because `WorkspaceLanguageContract::ResolveView` returns a
// view into the registry's owned storage.
struct LanguageContractView {
  std::string_view language_id;
  // Null when the contract has no entry; consumers SHALL fall back to "no
  // pairs / no smart indent" behavior in that case.
  const LanguageContract* contract = nullptr;
};

// Host-owned service that builds a per-language `LanguageContract` table by
// merging built-in defaults, plugin contributions, and user/project overrides.
// Stable views are issued through `ResolveView`; the table is rebuilt on
// `Refresh`, which bumps the revision counter so consumers can detect change.
class WorkspaceLanguageContract {
 public:
  WorkspaceLanguageContract();
  ~WorkspaceLanguageContract();

  // Optional setting getter consumed during Refresh; returns the configured
  // string for `editor.brackets.user_pairs`, `editor.brackets.user_disabled`,
  // `editor.comments.user_line`, `editor.indents.user_open_patterns`, and
  // `editor.snippets.user_disabled`. Pass an empty function (or a no-op) when
  // no overrides are wired (tests / pre-shell construction).
  using SettingGetter = std::function<std::optional<std::string>(std::string_view)>;

  // Recomputes the resolved table; safe to call on plugin reload, language id
  // change, or settings change.
  void Refresh(const plugin::PluginHost& plugin_host);
  void Refresh(const plugin::PluginHost& plugin_host, const SettingGetter& get_setting);

  // Strict accessor; returns nullptr when no contract exists. Use this when
  // the caller wants to short-circuit on "no language".
  const LanguageContract* Find(std::string_view language_id) const;

  // Always returns a view; `view.contract` is null when no entry exists.
  LanguageContractView ResolveView(std::string_view language_id) const;

  // Monotonically increasing on every successful Refresh; consumers cache
  // their derived state keyed on this counter.
  std::size_t revision() const { return revision_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::size_t revision_ = 0;
};

}  // namespace microide::workspace
