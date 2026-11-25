#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::tests::architecture {

namespace {

std::vector<std::pair<std::string, std::size_t>> CodeIdentifiers(const std::string& text,
                                                                 const std::vector<bool>& mask, std::size_t begin,
                                                                 std::size_t end) {
  std::vector<std::pair<std::string, std::size_t>> found;
  static const std::regex identifier(R"([A-Za-z_][A-Za-z0-9_]*)");
  const auto first = text.begin() + static_cast<std::ptrdiff_t>(begin);
  const auto last = text.begin() + static_cast<std::ptrdiff_t>(std::min(end, text.size()));
  for (std::sregex_iterator it(first, last, identifier), none; it != none; ++it) {
    const std::size_t at = begin + static_cast<std::size_t>(it->position());
    if (at < mask.size() && !mask[at]) {
      continue;
    }
    const std::size_t after = at + static_cast<std::size_t>(it->length());
    if (text.compare(after, 2, "::") == 0) {
      continue;
    }
    found.emplace_back(it->str(), at);
  }
  return found;
}

bool IsLiveSourceType(const std::string& name) {
  static const std::regex live(
      R"(^(\w+Store|\w+Service|\w+Coordinator|\w+Registry|WorkspaceContext|WorkspaceShell|ProjectWorkspaceState|PluginRuntime\w*)$)");
  return std::regex_match(name, live);
}

bool IsPreparedInputsType(const std::string& name) {
  static const std::regex prepared(R"(^(\w+Inputs|\w+Paint|\w+ViewModel|PreparedFrameState)$)");
  return std::regex_match(name, prepared);
}

struct HeaderText {
  std::filesystem::path path;
  std::string text;
  std::vector<bool> mask;
};
const std::vector<HeaderText>& SourceHeaders(const std::filesystem::path& repo_root,
                                             std::vector<HeaderText>& cache, bool& loaded) {
  if (!loaded) {
    loaded = true;
    static constexpr std::array<std::string_view, 1> kHeaders = {".h"};
    for (const std::filesystem::path& path : SourceFilesUnder(repo_root / "src", kHeaders)) {
      std::string text = ReadText(path);
      std::vector<bool> mask = BuildCodeMask(text);
      cache.push_back(HeaderText{.path = path, .text = std::move(text), .mask = std::move(mask)});
    }
  }
  return cache;
}

struct TypeDefinition {
  const HeaderText* header = nullptr;
  std::size_t open = 0;
};
TypeDefinition FindTypeDefinition(const std::vector<HeaderText>& headers, const std::string& name) {
  const std::regex definition("\\b(struct|class)\\s+" + name + R"(\b[^;{()]*\{)");
  for (const HeaderText& header : headers) {
    if (header.text.find(name) == std::string::npos) {
      continue;
    }
    for (std::sregex_iterator it(header.text.begin(), header.text.end(), definition), none; it != none; ++it) {
      const std::size_t at = static_cast<std::size_t>(it->position());
      if (at < header.mask.size() && !header.mask[at]) {
        continue;
      }
      return TypeDefinition{.header = &header, .open = at + static_cast<std::size_t>(it->length()) - 1};
    }
  }
  return {};
}

std::size_t MatchingBrace(const std::string& text, std::size_t open) {
  std::size_t depth = 1;
  std::size_t cursor = open + 1;
  while (cursor < text.size() && depth > 0) {
    depth += text[cursor] == '{' ? 1 : text[cursor] == '}' ? -1 : 0;
    ++cursor;
  }
  return cursor;
}

void CheckPaintParametersReachNoLiveSource(RuleResult& result, const std::vector<HeaderText>& headers,
                                           const std::filesystem::path& path, const std::string& text,
                                           const std::vector<bool>& mask, const char* function, std::size_t start) {
  const std::size_t params_open = text.find('(', start);
  if (params_open == std::string::npos) {
    return;
  }
  std::size_t depth = 1;
  std::size_t params_close = params_open + 1;
  while (params_close < text.size() && depth > 0) {
    depth += text[params_close] == '(' ? 1 : text[params_close] == ')' ? -1 : 0;
    ++params_close;
  }
  const std::size_t line = LineNumberAt(text, start);
  std::set<std::string> visited;
  std::vector<std::pair<std::string, std::string>> pending;
  const auto consider = [&](const std::string& name, const std::string& via) {
    if (IsLiveSourceType(name)) {
      result.violations.push_back(Violation{
          .path = path,
          .line = line,
          .message = std::string(function) + " is handed " + name + (via.empty() ? "" : " (through " + via + ")") +
                     " -- a derived-damage surface paints from inputs frame prep resolved and fingerprinted, "
                     "never from a store or service it can ask"});
    } else if (IsPreparedInputsType(name) && visited.insert(name).second) {
      pending.emplace_back(name, via.empty() ? name : via + " -> " + name);
    }
  };
  for (const auto& [name, at] : CodeIdentifiers(text, mask, params_open + 1, params_close)) {
    consider(name, "");
  }
  while (!pending.empty()) {
    const auto [name, via] = pending.back();
    pending.pop_back();
    const TypeDefinition definition = FindTypeDefinition(headers, name);
    if (definition.header == nullptr) {
      result.violations.push_back(Violation{
          .path = path,
          .line = line,
          .message = std::string(function) + " is handed " + name +
                     ", whose definition the rule cannot find under src -- update "
                     "CheckDerivedSurfacePaintReadsOnlyItsSnapshot"});
      continue;
    }
    const HeaderText& header = *definition.header;
    for (const auto& [field_type, at] : CodeIdentifiers(header.text, header.mask, definition.open + 1,
                                                        MatchingBrace(header.text, definition.open))) {
      consider(field_type, via);
    }
  }
}

struct MemberBody {
  const HeaderText* file = nullptr;
  std::size_t open = 0;
  std::size_t close = 0;
};
using MemberIndex = std::multimap<std::string, MemberBody>;

const MemberIndex& ShellMemberIndex(const std::filesystem::path& repo_root, std::vector<HeaderText>& files,
                                    MemberIndex& index, bool& loaded) {
  if (loaded) {
    return index;
  }
  loaded = true;
  static constexpr std::array<std::string_view, 3> kSources = {".cpp", ".inc", ".h"};
  for (const std::filesystem::path& path : SourceFilesUnder(repo_root / "src/workspace", kSources)) {
    std::string text = ReadText(path);
    std::vector<bool> mask = BuildCodeMask(text);
    files.push_back(HeaderText{.path = path, .text = std::move(text), .mask = std::move(mask)});
  }
  static const std::regex definition(R"(\bWorkspaceShell::(\w+)\s*\()");
  for (const HeaderText& file : files) {
    for (std::sregex_iterator it(file.text.begin(), file.text.end(), definition), none; it != none; ++it) {
      const std::size_t at = static_cast<std::size_t>(it->position());
      if (at < file.mask.size() && !file.mask[at]) {
        continue;
      }
      std::size_t cursor = at + static_cast<std::size_t>(it->length());
      int depth = 1;
      while (cursor < file.text.size() && depth > 0) {
        depth += file.text[cursor] == '(' ? 1 : file.text[cursor] == ')' ? -1 : 0;
        ++cursor;
      }
      const std::size_t brace = file.text.find_first_of("{;", cursor);
      if (brace == std::string::npos || file.text[brace] != '{') {
        continue;
      }
      index.emplace((*it)[1].str(), MemberBody{.file = &file, .open = brace, .close = MatchingBrace(file.text, brace)});
    }
  }
  return index;
}

void CheckPaintCalleesReadNoLiveSource(RuleResult& result, const MemberIndex& index, const std::regex& forbidden,
                                       const std::filesystem::path& path, const std::string& text,
                                       const std::vector<bool>& mask, std::size_t open, std::size_t close,
                                       const char* function) {
  static const std::regex call(R"((?:^|[^\w.>:])(\w+)\s*\()");
  std::set<std::string> visited;
  struct Pending {
    const std::string* text;
    const std::vector<bool>* mask;
    std::size_t open;
    std::size_t close;
    std::string via;
  };
  std::vector<Pending> pending{{&text, &mask, open, close, ""}};
  while (!pending.empty()) {
    const Pending body = pending.back();
    pending.pop_back();
    const auto begin = body.text->begin() + static_cast<std::ptrdiff_t>(body.open);
    const auto end = body.text->begin() + static_cast<std::ptrdiff_t>(body.close);
    for (std::sregex_iterator it(begin, end, call), none; it != none; ++it) {
      const std::size_t at = body.open + static_cast<std::size_t>(it->position(1));
      if (at < body.mask->size() && !(*body.mask)[at]) {
        continue;
      }
      const std::string name = (*it)[1].str();
      if (!visited.insert(name).second) {
        continue;
      }
      const auto [first, last] = index.equal_range(name);
      for (auto member = first; member != last; ++member) {
        const MemberBody& callee = member->second;
        const std::string via = body.via.empty() ? name : body.via + " -> " + name;
        const std::string& callee_text = callee.file->text;
        for (std::sregex_iterator read(callee_text.begin() + static_cast<std::ptrdiff_t>(callee.open),
                                       callee_text.begin() + static_cast<std::ptrdiff_t>(callee.close), forbidden),
             no_read;
             read != no_read; ++read) {
          const std::size_t read_at = callee.open + static_cast<std::size_t>(read->position());
          if (read_at < callee.file->mask.size() && !callee.file->mask[read_at]) {
            continue;
          }
          result.violations.push_back(Violation{
              .path = path,
              .line = LineNumberAt(text, open),
              .message = std::string(function) + " calls " + via + ", which reads " + read->str() + " (" +
                         callee.file->path.filename().string() + ":" +
                         std::to_string(LineNumberAt(callee_text, read_at)) +
                         ") -- a derived-damage paint and everything it calls read only what prep fingerprinted"});
          break;
        }
        pending.push_back(Pending{&callee_text, &callee.file->mask, callee.open, callee.close, via});
      }
    }
  }
}

}

RuleResult CheckDerivedSurfacePaintReadsOnlyItsSnapshot(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "derived-damage surface paint reads only its snapshot";
  result.hard_fail = true;
  struct Migrated {
    const char* file;
    const char* function;
  };
  static constexpr Migrated kMigrated[] = {
      {"src/workspace/render/WorkspaceShellRenderStatusBar.cpp", "void WorkspaceShell::RenderStatusBar("},
      {"src/workspace/render/WorkspaceShellRenderBottomPanel.cpp", "void PaintBottomPanelHeader("},
      {"src/workspace/render/WorkspaceShellRenderBottomPanel.cpp", "void PaintBottomPanelOutputBody("},
      {"src/workspace/render/WorkspaceShellRenderBottomPanel.cpp", "void PaintTerminalNoSessionHint("},
      {"src/workspace/render/WorkspaceShellRenderBottomPanel.cpp", "void PaintTerminalPaneHint("},
      {"src/workspace/render/WorkspaceShellRenderBottomPanel.cpp", "void PaintTerminalChrome("},
      {"src/workspace/render/BottomPanelTerminalGridPaint.cpp", "void PaintTerminalGridPane("},
      {"src/workspace/render/WorkspaceShellRenderSettingsOverlay.cpp", "void WorkspaceShell::RenderSettingsOverlay("},
      {"src/workspace/render/NotificationToastPaint.cpp", "void PaintNotificationToasts("},
      {"src/workspace/render/HoverCardPaint.cpp", "void PaintCard("},
      {"src/workspace/render/WorkspaceShellRenderPrompts.cpp", "void WorkspaceShell::RenderPromptSurface("},
      {"src/workspace/render/WorkspaceShellRenderPrompts.cpp", "void WorkspaceShell::RenderDirtyPromptSurface("},
      {"src/workspace/render/WorkspaceShellRenderFrame.cpp", "void WorkspaceShell::RenderFrameBase("},
      {"src/workspace/debug/DebugToolbarSurface.cpp", "void WorkspaceShell::RenderDebugToolbar("},
      {"src/workspace/render/WorkspaceShellRenderChrome.cpp", "void WorkspaceShell::RenderWindowChrome("},
      {"src/workspace/render/WorkspaceShellRenderFrame.cpp", "void WorkspaceShell::PaintEditorAreaChrome("},
      {"src/workspace/render/WorkspaceShellRenderFrame.cpp", "void WorkspaceShell::RenderActiveWorkspaceSurface("},
      {"src/workspace/render/WorkspaceShellRenderOverlay.cpp", "void WorkspaceShell::RenderOverlaySurface("},
      {"src/workspace/render/WorkspaceShellRenderOverlay.cpp", "void WorkspaceShell::RenderFindWidget("},
      {"src/workspace/render/DebugPaneRender.cpp", "void PaintDebugPaneBody("},
      {"src/workspace/render/GitSidebarPaint.cpp", "void PaintGitPanel("},
      {"src/workspace/render/TreeSidebarPaint.cpp", "void PaintTreeSidebar("},
      {"src/workspace/render/SidebarListPaint.cpp", "void PaintSidebarList("},
      {"src/workspace/render/SearchSidebarPaint.cpp", "void PaintSearchSidebar("},
      {"src/workspace/render/SidebarModeRowPaint.cpp", "void PaintSidebarModeRow("},
      {"src/workspace/render/MenuPopupPaint.cpp", "void PaintMenuPopups("},
      {"src/workspace/render/WorkspaceShellRenderFrame.cpp", "void WorkspaceShell::PaintEditorPane("},
      {"src/workspace/render/WorkspaceShellRenderCompare.cpp", "void WorkspaceShell::RenderCompareSurface("},
      {"src/workspace/render/WorkspaceShellRenderCompare.cpp", "void WorkspaceShell::RenderCompareScrollbars("},
      {"src/workspace/render/WorkspaceShellRenderMerge.cpp", "void WorkspaceShell::RenderMergeSurface("},
      {"src/workspace/render/WorkspaceShellRenderMerge.cpp", "void WorkspaceShell::RenderMergeScrollbars("},
  };
  std::vector<HeaderText> headers;
  bool headers_loaded = false;
  std::vector<HeaderText> member_files;
  MemberIndex members;
  bool members_loaded = false;
  const std::regex forbidden(
      R"(\bcontext_\b|\b[a-z_]+_service_\b|\bPointerOver\s*\(|\bRenderViewModelBuilder\b|)"
      R"(\bMake[A-Z][A-Za-z]*Service\s*\(|\bSDL_GetTicks|\blast_mouse_[a-z_]*)");
  for (const Migrated& migrated : kMigrated) {
    const std::filesystem::path path = repo_root / migrated.file;
    const std::string text = ReadRuleTarget(result, path);
    if (text.empty()) {
      continue;
    }
    const std::size_t start = text.find(migrated.function);
    const std::size_t open = start == std::string::npos ? std::string::npos : text.find('{', start);
    if (open == std::string::npos) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = std::string("migrated paint function not found: ") + migrated.function +
                     " -- update CheckDerivedSurfacePaintReadsOnlyItsSnapshot"});
      continue;
    }
    std::size_t depth = 1;
    std::size_t cursor = open + 1;
    while (cursor < text.size() && depth > 0) {
      depth += text[cursor] == '{' ? 1 : text[cursor] == '}' ? -1 : 0;
      ++cursor;
    }
    const std::vector<bool> mask = BuildCodeMask(text);
    CheckPaintParametersReachNoLiveSource(result, SourceHeaders(repo_root, headers, headers_loaded), path, text, mask,
                                          migrated.function, start);
    if (std::string_view(migrated.function).starts_with("void WorkspaceShell::")) {
      CheckPaintCalleesReadNoLiveSource(result, ShellMemberIndex(repo_root, member_files, members, members_loaded),
                                        forbidden, path, text, mask, open, cursor, migrated.function);
    }
    const std::string body = text.substr(open, cursor - open);
    for (std::sregex_iterator it(body.begin(), body.end(), forbidden), end; it != end; ++it) {
      const std::size_t at = open + static_cast<std::size_t>(it->position());
      if (at < mask.size() && !mask[at]) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = path,
          .line = LineNumberAt(text, at),
          .message = std::string(migrated.function) + " reads " + it->str() +
                     " -- a derived-damage surface paints only what prep fingerprinted (frame_state_)"});
    }
  }
  return result;
}

}
