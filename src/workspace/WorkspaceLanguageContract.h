#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/LanguageContractView.h"

namespace microide::compare {
struct DiffSyntax;
}

namespace microide::plugin {
class PluginHost;
}

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

struct LanguageContract {
  std::string language_id;
  std::vector<LanguageBracketPair> bracket_pairs;
  std::vector<LanguageBracketPair> auto_close_pairs;
  std::vector<LanguageBracketPair> surround_pairs;
  std::string line_comment;
  LanguageBracketPair block_comment;
  std::vector<std::string> indent_after_open_patterns;
  std::vector<std::string> dedent_on_close_chars;
  bool inhibit_pairs_in_strings = true;
  bool inhibit_pairs_in_comments = true;
  std::vector<std::string> outline_regex_patterns;
  std::vector<LanguageSnippet> snippets;
};

struct LanguageContractView {
  std::string_view language_id;
  const LanguageContract* contract = nullptr;
};

class WorkspaceLanguageContract {
 public:
  WorkspaceLanguageContract();
  ~WorkspaceLanguageContract();

  using SettingGetter = std::function<std::optional<std::string>(std::string_view)>;

  void Refresh(const plugin::PluginHost& plugin_host);
  void Refresh(const plugin::PluginHost& plugin_host, const SettingGetter& get_setting);

  const LanguageContract* Find(std::string_view language_id) const;

  LanguageContractView ResolveView(std::string_view language_id) const;

  std::shared_ptr<const editor::LanguageContractView> ResolveEditorView(
      std::string_view language_id,
      bool auto_close_enabled,
      bool surround_enabled,
      bool smart_indent_enabled) const;

  std::shared_ptr<const compare::DiffSyntax> ResolveDiffSyntax(std::string_view language_id) const;

  static std::string SettingsFingerprint(const SettingGetter& get_setting);

  std::size_t revision() const { return revision_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::size_t revision_ = 0;
};

}
