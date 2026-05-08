#include "workspace/WorkspaceLanguageContract.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include "plugin/PluginHost.h"

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
  return c;
}

LanguageContract MakeIndentStyle(std::string id,
                                 std::string line_comment,
                                 LanguageBracketPair block_comment = {}) {
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
    c.bracket_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
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

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return out;
}

}  // namespace

struct WorkspaceLanguageContract::Impl {
  std::unordered_map<std::string, LanguageContract> table;
};

WorkspaceLanguageContract::WorkspaceLanguageContract() : impl_(std::make_unique<Impl>()) {
  impl_->table = BuildDefaults();
  revision_ = 1;
}

WorkspaceLanguageContract::~WorkspaceLanguageContract() = default;

namespace {

LanguageContract& EnsureContract(std::unordered_map<std::string, LanguageContract>& table,
                                 std::string_view language_id) {
  std::string key = LowerAscii(language_id);
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

void ApplyUserOverrides(std::unordered_map<std::string, LanguageContract>& table,
                        const WorkspaceLanguageContract::SettingGetter& get_setting) {
  if (!get_setting) return;

  std::vector<LanguageBracketPair> add_pairs;
  if (auto value = get_setting("editor.brackets.user_pairs"); value && !value->empty()) {
    for (const auto& token : SplitCsv(*value)) {
      if (auto pair = ParseUserBracketPair(token)) {
        add_pairs.push_back(std::move(*pair));
      }
    }
  }
  std::vector<LanguageBracketPair> disabled_pairs;
  if (auto value = get_setting("editor.brackets.user_disabled"); value && !value->empty()) {
    for (const auto& token : SplitCsv(*value)) {
      if (auto pair = ParseUserBracketPair(token)) {
        disabled_pairs.push_back(std::move(*pair));
      }
    }
  }

  std::optional<std::string> line_comment_override;
  if (auto value = get_setting("editor.comments.user_line"); value && !value->empty()) {
    line_comment_override = *value;
  }

  std::vector<std::string> add_patterns;
  if (auto value = get_setting("editor.indents.user_open_patterns"); value && !value->empty()) {
    add_patterns = SplitCsv(*value);
  }

  std::vector<std::string> disabled_snippet_prefixes;
  if (auto value = get_setting("editor.snippets.user_disabled"); value && !value->empty()) {
    disabled_snippet_prefixes = SplitCsv(*value);
  }

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
  // The revision counter advances on every Refresh so downstream caches
  // invalidate cleanly.
  impl_->table = BuildDefaults();

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

  ApplyUserOverrides(impl_->table, get_setting);

  ++revision_;
}

const LanguageContract* WorkspaceLanguageContract::Find(std::string_view language_id) const {
  if (language_id.empty()) return nullptr;
  std::string key = LowerAscii(language_id);
  const auto it = impl_->table.find(key);
  return it == impl_->table.end() ? nullptr : &it->second;
}

LanguageContractView WorkspaceLanguageContract::ResolveView(std::string_view language_id) const {
  LanguageContractView view;
  view.language_id = language_id;
  view.contract = Find(language_id);
  return view;
}

}  // namespace microide::workspace
