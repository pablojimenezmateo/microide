#include "workspace/WorkspaceLanguageContract.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include "plugin/PluginHost.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/TransparentStringHash.h"

namespace microide::workspace {

namespace {

LanguageContract MakeCStyle(std::string id, std::string line_comment = "//") {
  LanguageContract c;
  c.language_id = std::move(id);
  c.bracket_pairs = {{"{", "}"}, {"(", ")"}, {"[", "]"}};
  c.auto_close_pairs = {{"{", "}"}, {"(", ")"}, {"[", "]"},
                        {"\"", "\""}, {"'", "'"}};
  c.surround_pairs = {{"{", "}"}, {"(", ")"}, {"[", "]"},
                      {"\"", "\""}, {"'", "'"}, {"`", "`"}};
  c.line_comment = std::move(line_comment);
  c.block_comment = {"/*", "*/"};
  c.indent_after_open_patterns = {"{", "(", "["};
  c.dedent_on_close_chars = {"}", ")", "]"};
  c.outline_regex_patterns = {
      R"(^\s*(?:class|struct|enum\s+class)\s+([A-Za-z_][\w]*))",
      R"(^\s*(?:virtual\s+)?(?:void|int|bool|char|double|float|auto|inline\s+void|static\s+void|unsigned\s+long)\s+([A-Za-z_][\w]*)\s*\()",
  };
  return c;
}

LanguageContract MakeIndentStyle(std::string id,
                                 std::string line_comment,
                                 LanguageBracketPair block_comment = {}) {
  const bool is_python = id == "python";
  LanguageContract c;
  c.language_id = std::move(id);
  c.bracket_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
  c.auto_close_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"},
                        {"\"", "\""}, {"'", "'"}};
  c.surround_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"},
                      {"\"", "\""}, {"'", "'"}};
  c.line_comment = std::move(line_comment);
  c.block_comment = block_comment;
  c.indent_after_open_patterns = {":", "(", "[", "{"};
  c.dedent_on_close_chars = {")", "]", "}"};
  if (is_python) {
    c.outline_regex_patterns = {
        R"(^\s*def\s+([A-Za-z_][\w]*)\s*\()",
        R"(^\s*class\s+([A-Za-z_][\w]*)\s*(?:\(|:))",
    };
  }
  return c;
}

std::unordered_map<std::string, LanguageContract> BuildDefaults() {
  std::unordered_map<std::string, LanguageContract> map;

  auto add = [&](LanguageContract c) {
    std::string id = c.language_id;
    map.emplace(std::move(id), std::move(c));
  };

  add(MakeCStyle("c"));
  add(MakeCStyle("cpp"));
  add(MakeCStyle("javascript"));
  add(MakeCStyle("typescript"));
  add(MakeCStyle("rust"));
  add(MakeCStyle("go"));
  add(MakeCStyle("java"));
  add(MakeCStyle("kotlin"));
  add(MakeCStyle("swift"));
  add(MakeCStyle("scala"));
  add(MakeCStyle("dart"));
  add(MakeCStyle("css"));
  add(MakeCStyle("scss"));
  add(MakeCStyle("less"));

  add(MakeIndentStyle("python", "#"));
  add(MakeIndentStyle("yaml", "#"));
  add(MakeIndentStyle("ruby", "#"));
  add(MakeIndentStyle("elixir", "#"));
  add(MakeIndentStyle("nim", "#"));

  {
    auto c = MakeCStyle("lua", "--");
    c.block_comment = {"--[[", "]]"};
    add(std::move(c));
  }
  {
    auto c = MakeIndentStyle("haskell", "--");
    c.block_comment = {"{-", "-}"};
    add(std::move(c));
  }
  {
    auto c = MakeIndentStyle("ada", "--");
    add(std::move(c));
  }
  {
    auto c = MakeIndentStyle("sql", "--");
    c.block_comment = {"/*", "*/"};
    add(std::move(c));
  }

  {
    auto c = MakeCStyle("shell", "#");
    c.block_comment = {};
    c.indent_after_open_patterns = {"do", "then", "{", "(", "["};
    add(std::move(c));
  }
  {
    auto c = MakeCStyle("bash", "#");
    c.block_comment = {};
    c.indent_after_open_patterns = {"do", "then", "{", "(", "["};
    add(std::move(c));
  }
  {
    auto c = MakeCStyle("powershell", "#");
    c.block_comment = {"<#", "#>"};
    add(std::move(c));
  }

  {
    auto c = MakeCStyle("json");
    c.line_comment = {};
    c.block_comment = {};
    add(std::move(c));
  }
  {
    LanguageContract c;
    c.language_id = "markdown";
    // Prose: parens/brackets routinely span lines, so bracket-based folding
    // produces bogus fold markers on ordinary paragraphs. Leave this empty;
    // markdown still folds on indentation (nested lists / indented blocks).
    c.bracket_pairs = {};
    c.auto_close_pairs = {{"(", ")"}, {"[", "]"},
                          {"\"", "\""}, {"`", "`"}};
    c.surround_pairs = {{"(", ")"}, {"[", "]"},
                        {"\"", "\""}, {"`", "`"}, {"*", "*"}, {"_", "_"}};
    c.line_comment = {};
    c.block_comment = {"<!--", "-->"};
    add(std::move(c));
  }
  {
    LanguageContract c;
    c.language_id = "html";
    c.bracket_pairs = {{"<", ">"}, {"(", ")"}, {"[", "]"}, {"{", "}"}};
    c.auto_close_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"},
                          {"\"", "\""}, {"'", "'"}};
    c.surround_pairs = c.auto_close_pairs;
    c.line_comment = {};
    c.block_comment = {"<!--", "-->"};
    add(std::move(c));
  }
  {
    LanguageContract c;
    c.language_id = "xml";
    c.bracket_pairs = {{"<", ">"}, {"(", ")"}, {"[", "]"}, {"{", "}"}};
    c.auto_close_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"},
                          {"\"", "\""}, {"'", "'"}};
    c.surround_pairs = c.auto_close_pairs;
    c.line_comment = {};
    c.block_comment = {"<!--", "-->"};
    add(std::move(c));
  }
  {
    auto c = MakeCStyle("toml", "#");
    c.block_comment = {};
    add(std::move(c));
  }

  return map;
}

}  // namespace

struct WorkspaceLanguageContract::Impl {
  std::unordered_map<std::string, LanguageContract> table;

  // True while `table` is exactly `BuildDefaults()` — no plugin contribution and
  // no user override layered on it. Refresh reads it to skip a rebuild that
  // would reproduce the same table (see Refresh).
  bool table_is_pure_defaults = true;

  // Memo behind ResolveEditorView. Mutable because resolution is a pure query;
  // `views_revision == 0` means "never populated", which is distinguishable
  // because WorkspaceLanguageContract's revision starts at 1.
  mutable std::unordered_map<std::string, std::shared_ptr<const editor::LanguageContractView>,
                             util::TransparentStringHash, std::equal_to<>>
      editor_views;
  mutable std::size_t editor_views_revision = 0;
  mutable bool editor_views_auto_close = false;
  mutable bool editor_views_surround = false;
  mutable bool editor_views_smart_indent = false;
};

WorkspaceLanguageContract::WorkspaceLanguageContract() : impl_(std::make_unique<Impl>()) {
  impl_->table = BuildDefaults();
  revision_ = 1;
}

WorkspaceLanguageContract::~WorkspaceLanguageContract() = default;

namespace {

LanguageContract& EnsureContract(std::unordered_map<std::string, LanguageContract>& table,
                                 std::string_view language_id) {
  std::string key = util::ToLowerAscii(language_id);
  auto it = table.find(key);
  if (it == table.end()) {
    LanguageContract empty;
    empty.language_id = key;
    auto [inserted_it, _] = table.emplace(std::move(key), std::move(empty));
    return inserted_it->second;
  }
  return it->second;
}

void MergeBracketSet(LanguageContract& dst, const plugin::PluginHost::ContributedBracketSet& src) {
  for (const auto& [open, close] : src.bracket_pairs) {
    dst.bracket_pairs.push_back(LanguageBracketPair{open, close});
  }
  for (const auto& [open, close] : src.auto_close_pairs) {
    dst.auto_close_pairs.push_back(LanguageBracketPair{open, close});
  }
  for (const auto& [open, close] : src.surround_pairs) {
    dst.surround_pairs.push_back(LanguageBracketPair{open, close});
  }
}

void MergeComments(LanguageContract& dst,
                   const plugin::PluginHost::ContributedCommentMarkers& src) {
  if (!src.line_comment.empty()) {
    dst.line_comment = src.line_comment;
  }
  if (!src.block_comment_open.empty()) {
    dst.block_comment.open = src.block_comment_open;
  }
  if (!src.block_comment_close.empty()) {
    dst.block_comment.close = src.block_comment_close;
  }
}

void MergeIndentRules(LanguageContract& dst,
                      const plugin::PluginHost::ContributedIndentRules& src) {
  for (const auto& pat : src.indent_after_open_patterns) {
    dst.indent_after_open_patterns.push_back(pat);
  }
  for (const auto& ch : src.dedent_on_close_chars) {
    dst.dedent_on_close_chars.push_back(ch);
  }
}

void MergeSnippet(LanguageContract& dst, const plugin::PluginHost::ContributedSnippet& src) {
  LanguageSnippet snippet;
  snippet.prefix = src.prefix;
  snippet.label = src.label.empty() ? src.prefix : src.label;
  snippet.body = src.body;
  dst.snippets.push_back(std::move(snippet));
}

std::vector<std::string> SplitCsv(std::string_view text) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == ',') {
      if (!current.empty()) parts.push_back(std::move(current));
      current.clear();
    } else if (c != ' ' && c != '\t') {
      current.push_back(c);
    }
  }
  if (!current.empty()) parts.push_back(std::move(current));
  return parts;
}

std::optional<LanguageBracketPair> ParseUserBracketPair(std::string_view token) {
  const auto bar = token.find('|');
  if (bar == std::string_view::npos || bar == 0 || bar + 1 >= token.size()) {
    return std::nullopt;
  }
  return LanguageBracketPair{std::string(token.substr(0, bar)),
                             std::string(token.substr(bar + 1))};
}

// The five user/project setting overrides, parsed once per Refresh. Kept as a
// value so Refresh can ask whether any of them are set BEFORE deciding to
// rebuild the table: with no overrides and no plugin contributions the rebuilt
// table is byte-identical to the defaults, and building it costs ~5,000
// allocations (TD-2026-08-06-159).
struct UserOverrides {
  std::vector<LanguageBracketPair> add_pairs;
  std::vector<LanguageBracketPair> disabled_pairs;
  std::optional<std::string> line_comment;
  std::vector<std::string> add_patterns;
  std::vector<std::string> disabled_snippet_prefixes;

  bool empty() const {
    return add_pairs.empty() && disabled_pairs.empty() && !line_comment.has_value() &&
           add_patterns.empty() && disabled_snippet_prefixes.empty();
  }
};

UserOverrides ParseUserOverrides(const WorkspaceLanguageContract::SettingGetter& get_setting) {
  UserOverrides overrides;
  if (!get_setting) return overrides;

  if (auto value = get_setting("editor.brackets.user_pairs"); value && !value->empty()) {
    for (const auto& token : SplitCsv(*value)) {
      if (auto pair = ParseUserBracketPair(token)) {
        overrides.add_pairs.push_back(std::move(*pair));
      }
    }
  }
  if (auto value = get_setting("editor.brackets.user_disabled"); value && !value->empty()) {
    for (const auto& token : SplitCsv(*value)) {
      if (auto pair = ParseUserBracketPair(token)) {
        overrides.disabled_pairs.push_back(std::move(*pair));
      }
    }
  }
  if (auto value = get_setting("editor.comments.user_line"); value && !value->empty()) {
    overrides.line_comment = *value;
  }
  if (auto value = get_setting("editor.indents.user_open_patterns"); value && !value->empty()) {
    overrides.add_patterns = SplitCsv(*value);
  }
  if (auto value = get_setting("editor.snippets.user_disabled"); value && !value->empty()) {
    overrides.disabled_snippet_prefixes = SplitCsv(*value);
  }
  return overrides;
}

void ApplyUserOverrides(std::unordered_map<std::string, LanguageContract>& table,
                        const UserOverrides& overrides) {
  if (overrides.empty()) return;

  const std::vector<LanguageBracketPair>& add_pairs = overrides.add_pairs;
  const std::vector<LanguageBracketPair>& disabled_pairs = overrides.disabled_pairs;
  const std::optional<std::string>& line_comment_override = overrides.line_comment;
  const std::vector<std::string>& add_patterns = overrides.add_patterns;
  const std::vector<std::string>& disabled_snippet_prefixes = overrides.disabled_snippet_prefixes;

  const auto pair_equals = [](const LanguageBracketPair& lhs,
                               const LanguageBracketPair& rhs) {
    return lhs.open == rhs.open && lhs.close == rhs.close;
  };

  for (auto& [key, contract] : table) {
    for (const auto& add : add_pairs) {
      contract.bracket_pairs.push_back(add);
      contract.auto_close_pairs.push_back(add);
      contract.surround_pairs.push_back(add);
    }
    for (const auto& disabled : disabled_pairs) {
      const auto strip = [&](std::vector<LanguageBracketPair>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const LanguageBracketPair& p) {
                  return pair_equals(p, disabled);
                }),
                v.end());
      };
      strip(contract.bracket_pairs);
      strip(contract.auto_close_pairs);
      strip(contract.surround_pairs);
    }
    if (line_comment_override.has_value()) {
      contract.line_comment = *line_comment_override;
    }
    for (const auto& pat : add_patterns) {
      contract.indent_after_open_patterns.push_back(pat);
    }
    if (!disabled_snippet_prefixes.empty()) {
      contract.snippets.erase(
          std::remove_if(
              contract.snippets.begin(), contract.snippets.end(),
              [&](const LanguageSnippet& s) {
                return std::find(disabled_snippet_prefixes.begin(),
                                 disabled_snippet_prefixes.end(),
                                 s.prefix) != disabled_snippet_prefixes.end();
              }),
          contract.snippets.end());
    }
  }
}

}  // namespace

void WorkspaceLanguageContract::Refresh(const plugin::PluginHost& plugin_host) {
  Refresh(plugin_host, SettingGetter{});
}

void WorkspaceLanguageContract::Refresh(const plugin::PluginHost& plugin_host,
                                        const SettingGetter& get_setting) {
  // Built-in defaults form the base layer. Plugin contributions append to the
  // matching language entry (or create a new one when no built-in exists).
  // User / project setting overrides are layered on top via `get_setting`.
  // The revision counter advances on every Refresh that changes the table so
  // downstream caches invalidate cleanly.
  const bool has_contributions = !plugin_host.ContributedBrackets().empty() ||
                                 !plugin_host.ContributedComments().empty() ||
                                 !plugin_host.ContributedIndents().empty() ||
                                 !plugin_host.ContributedSnippets().empty();
  const UserOverrides overrides = ParseUserOverrides(get_setting);

  // A Refresh with nothing layered on top produces exactly BuildDefaults(), so
  // when the table already IS that, rebuilding it is ~5,000 allocations that
  // change nothing — and the revision bump would additionally drop the shared
  // editor-view cache and re-apply preferences to every open tab. Every project
  // switch runs this path (the shared plugin host is torn down on switch-away,
  // so it contributes nothing at reactivation time). TD-2026-08-06-159.
  if (!has_contributions && overrides.empty() && impl_->table_is_pure_defaults) {
    return;
  }
  impl_->table = BuildDefaults();
  impl_->table_is_pure_defaults = !has_contributions && overrides.empty();

  for (const auto& set : plugin_host.ContributedBrackets()) {
    if (set.language_id.empty()) continue;
    MergeBracketSet(EnsureContract(impl_->table, set.language_id), set);
  }
  for (const auto& markers : plugin_host.ContributedComments()) {
    if (markers.language_id.empty()) continue;
    MergeComments(EnsureContract(impl_->table, markers.language_id), markers);
  }
  for (const auto& rules : plugin_host.ContributedIndents()) {
    if (rules.language_id.empty()) continue;
    MergeIndentRules(EnsureContract(impl_->table, rules.language_id), rules);
  }
  for (const auto& snippet : plugin_host.ContributedSnippets()) {
    if (snippet.language_id.empty()) continue;
    MergeSnippet(EnsureContract(impl_->table, snippet.language_id), snippet);
  }

  ApplyUserOverrides(impl_->table, overrides);

  ++revision_;
}

namespace {

// Map runtime-syntax filetype names to language-contract keys. The runtime
// detector returns names like "c++"/"objective-c"/"csharp" (which match
// language-id conventions in `RuntimeSyntaxRegistry`), while the language
// contract defaults are keyed by short ids like "cpp". Without this alias
// step, `.cpp` files would resolve to a null contract and auto-close /
// surround / smart-indent would silently no-op.
std::string_view CanonicalContractKey(std::string_view key) {
  if (key == "c++") return "cpp";
  if (key == "objective-c") return "c";
  if (key == "csharp") return "cpp";
  return key;
}

}  // namespace

const LanguageContract* WorkspaceLanguageContract::Find(std::string_view language_id) const {
  if (language_id.empty()) return nullptr;
  std::string key = util::ToLowerAscii(language_id);
  if (const std::string_view canonical = CanonicalContractKey(key); canonical != key) {
    key.assign(canonical);
  }
  const auto it = impl_->table.find(key);
  return it == impl_->table.end() ? nullptr : &it->second;
}

LanguageContractView WorkspaceLanguageContract::ResolveView(std::string_view language_id) const {
  LanguageContractView view;
  view.language_id = language_id;
  view.contract = Find(language_id);
  return view;
}

std::shared_ptr<const editor::LanguageContractView> WorkspaceLanguageContract::ResolveEditorView(
    std::string_view language_id,
    bool auto_close_enabled,
    bool surround_enabled,
    bool smart_indent_enabled) const {
  util::AddPerformanceCounter(util::PerfCounterId::LanguageContractViewQueries);
  if (impl_->editor_views_revision != revision_ ||
      impl_->editor_views_auto_close != auto_close_enabled ||
      impl_->editor_views_surround != surround_enabled ||
      impl_->editor_views_smart_indent != smart_indent_enabled) {
    // The toggles are baked into the view, so a toggle change invalidates every
    // cached view -- clear rather than try to patch them in place.
    impl_->editor_views.clear();
    impl_->editor_views_revision = revision_;
    impl_->editor_views_auto_close = auto_close_enabled;
    impl_->editor_views_surround = surround_enabled;
    impl_->editor_views_smart_indent = smart_indent_enabled;
  } else if (const auto it = impl_->editor_views.find(language_id);
             it != impl_->editor_views.end()) {
    return it->second;
  }

  util::AddPerformanceCounter(util::PerfCounterId::LanguageContractViewBuilds);
  auto view = std::make_shared<editor::LanguageContractView>();
  if (const LanguageContract* contract = Find(language_id); contract != nullptr) {
    view->auto_close_pairs.reserve(contract->auto_close_pairs.size());
    for (const auto& pair : contract->auto_close_pairs) {
      view->auto_close_pairs.push_back(editor::LanguagePair{pair.open, pair.close});
    }
    view->surround_pairs.reserve(contract->surround_pairs.size());
    for (const auto& pair : contract->surround_pairs) {
      view->surround_pairs.push_back(editor::LanguagePair{pair.open, pair.close});
    }
    view->indent_after_open_patterns = contract->indent_after_open_patterns;
    view->dedent_on_close_chars = contract->dedent_on_close_chars;
    view->line_comment = contract->line_comment;
    view->block_comment_open = contract->block_comment.open;
    view->block_comment_close = contract->block_comment.close;
    view->inhibit_pairs_in_strings = contract->inhibit_pairs_in_strings;
    view->inhibit_pairs_in_comments = contract->inhibit_pairs_in_comments;
  }
  view->auto_close_enabled = auto_close_enabled;
  view->surround_enabled = surround_enabled;
  view->smart_indent_enabled = smart_indent_enabled;
  // Keyed by the raw detected id, not the canonical contract key: two ids that
  // alias to one contract ("c++" and "cpp") get one entry each, which is a
  // duplicate view but keeps the lookup a single hash with no normalization
  // allocation on the hot path.
  return impl_->editor_views.emplace(std::string(language_id), std::move(view)).first->second;
}

}  // namespace microide::workspace
