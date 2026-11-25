#include "architecture/TerminalArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <fstream>
#include <map>
#include <regex>
#include <string_view>
#include <algorithm>

namespace microide::tests::architecture {

namespace {

std::size_t CountFileLines(const std::filesystem::path& path) {
  return CountCodeLinesInFile(path);
}

}

RuleResult CheckTerminalSessionTuSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession.cpp translation unit size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 600;
  const std::filesystem::path path = repo_root / "src/terminal/TerminalSession.cpp";
  const std::size_t lines = CountFileLines(path);
  if (lines > kCap) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "TerminalSession.cpp should stay at or below " + std::to_string(kCap) +
                   " code lines (comments and blank lines excluded); extract helpers "
                   "instead of growing the facade",
    });
  }
  return result;
}

RuleResult CheckTerminalHelperTuSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "terminal helper translation unit size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 800;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/terminal")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    if (entry.path().filename() == "TerminalSession.cpp") {
      continue;
    }
    const std::size_t lines = CountFileLines(entry.path());
    if (lines > kCap) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "terminal helper translation units should stay at or below 800 code "
                     "lines (comments and blank lines excluded)",
      });
    }
  }
  return result;
}

RuleResult CheckTerminalParserHelpersNoForbiddenDeps(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "terminal parser helpers avoid SDL/workspace/plugin/render headers";
  result.hard_fail = true;
  const std::array<std::string_view, 3> helper_paths = {
      "src/terminal/TerminalBase64.cpp",
      "src/terminal/TerminalCsiParser.cpp",
      "src/terminal/TerminalOscClipboard.cpp",
  };
  const std::array<std::string_view, 6> forbidden = {
      "SDL3/SDL.h",
      "workspace/WorkspaceShell",
      "plugin/PluginHost",
      "workspace/RenderViewModelBuilder",
      "render/TextRenderer",
      "workspace/Workspace",
  };
  for (const std::string_view relative : helper_paths) {
    const std::filesystem::path path = repo_root / relative;
    const std::string text = ReadRuleTarget(result, path);
    for (const std::string_view needle : forbidden) {
      if (text.find(needle) != std::string::npos) {
        result.violations.push_back(Violation{
            .path = path,
            .line = 1,
            .message = std::string(relative) + " must not include " + std::string(needle),
        });
      }
    }
  }
  return result;
}

RuleResult CheckTerminalSessionNoExtractedImpl(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession.cpp keeps extracted helper implementations out";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/terminal/TerminalSession.cpp";
  const std::string text = ReadRuleTarget(result, path);
  constexpr auto kLineAnchored = std::regex::ECMAScript | std::regex::multiline;
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"((?:std::optional<int>|^int)\s+Base64Value\s*\()", kLineAnchored),
      "TerminalSession.cpp must not contain base64 decoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"((?:std::optional<std::string>|^std::optional)\s+DecodeBase64\s*\()",
                 kLineAnchored),
      "TerminalSession.cpp must not contain base64 decoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(^std::vector<int>\s+ParseCsiParameters\s*\()", kLineAnchored),
      "TerminalSession.cpp must not contain CSI parameter parser implementation");
  AppendCodeMaskRegexViolations(
      result, path, text, std::regex(R"(^int\s+MouseModifierBits\s*\()", kLineAnchored),
      "TerminalSession.cpp must not contain mouse encoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text, std::regex(R"(^bool\s+EncodeTerminalMouseEvent\s*\()", kLineAnchored),
      "TerminalSession.cpp must not contain mouse encoder implementation");
  return result;
}

RuleResult CheckTerminalSessionHeaderSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession.h header size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 335;
  const std::filesystem::path path = repo_root / "src/terminal/TerminalSession.h";
  const std::size_t lines = CountFileLines(path);
  if (lines > kCap) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "TerminalSession.h should stay at or below " + std::to_string(kCap) +
                   " code lines (comments and blank lines excluded); move internal helpers "
                   "out of the public header",
    });
  }
  return result;
}

RuleResult CheckTerminalSessionPrivateMethodCount(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession.h private method count";
  result.hard_fail = true;
  constexpr std::size_t kCap = 66;
  const std::filesystem::path path = repo_root / "src/terminal/TerminalSession.h";
  const std::string text = ReadRuleTarget(result, path);
  const std::regex locked_helper_pattern(R"((?:void|bool|std::size_t)\s+\w+Locked\s*\()");
  const std::size_t count =
      static_cast<std::size_t>(std::distance(
          std::sregex_iterator(text.begin(), text.end(), locked_helper_pattern),
          std::sregex_iterator()));
  if (count > kCap) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "TerminalSession.h should keep at most " + std::to_string(kCap) +
                   " private Locked helpers; extract focused helpers instead",
    });
  }
  return result;
}

RuleResult CheckTerminalInternalHeadersStayInTerminalDir(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "terminal internal headers stay under src/terminal";
  result.hard_fail = true;
  const std::array<std::string_view, 2> internal_headers = {
      "TerminalInternalConstants.h",
      "TerminalSessionInputEncoding.h",
  };
  const std::array<std::filesystem::path, 3> scan_roots = {
      repo_root / "src",
      repo_root / "tests",
      repo_root / "tools",
  };
  for (const std::filesystem::path& root : scan_roots) {
    if (!RequireRuleTarget(result, root)) {
      continue;
    }
    static constexpr std::array<std::string_view, 3> kExtensions = {".cpp", ".h", ".inc"};
    for (const std::filesystem::path& path : SourceFilesUnder(root, kExtensions)) {
      const std::string relative = path.lexically_relative(repo_root).generic_string();
      if (relative.rfind("src/terminal/", 0) == 0) {
        continue;
      }
      const std::string text = ReadRuleTarget(result, path);
      for (const std::string_view header : internal_headers) {
        const std::string needle = std::string("terminal/") + std::string(header);
        if (text.find(needle) != std::string::npos) {
          result.violations.push_back(Violation{
              .path = path,
              .line = 1,
              .message = relative + " must not include internal terminal header " +
                         std::string(header),
          });
        }
      }
    }
  }
  return result;
}

RuleResult CheckTerminalSessionSplitTranslationUnits(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession split translation units stay named";
  result.hard_fail = true;
  const std::array<std::string_view, 9> allowed = {
      "TerminalSessionOutput.cpp",
      "TerminalSessionCsi.cpp",
      "TerminalSessionSgr.cpp",
      "TerminalSessionOsc.cpp",
      "TerminalSessionModes.cpp",
      "TerminalSessionInput.cpp",
      "TerminalSessionScreen.cpp",
      "TerminalSessionInputEncoding.cpp",
      "TerminalSessionHost.cpp",
  };
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/terminal")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (filename.rfind("TerminalSession", 0) != 0 || filename == "TerminalSession.cpp") {
      continue;
    }
    const bool allowed_name =
        std::find(allowed.begin(), allowed.end(), filename) != allowed.end();
    if (!allowed_name) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message =
              "unexpected TerminalSession*.cpp split; add an architecture rule before introducing "
              "new terminal session translation units",
      });
    }
  }
  return result;
}

RuleResult CheckArchitectureInvariantsDispatcherSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "ArchitectureInvariantsTests.cpp dispatcher size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 320;
  const std::filesystem::path path = repo_root / "tests/ArchitectureInvariantsTests.cpp";
  const std::size_t lines = CountFileLines(path);
  if (lines > kCap) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "ArchitectureInvariantsTests.cpp should stay a dispatcher at or below " +
                   std::to_string(kCap) +
                   " code lines (comments and blank lines excluded); add new rules under "
                   "tests/architecture/ instead",
    });
  }
  return result;
}

RuleResult CheckArchitectureRulesTuSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "architecture rule translation unit size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 800;
  const std::filesystem::path rules_dir = repo_root / "tests/architecture";
  for (const auto& entry : std::filesystem::directory_iterator(rules_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::size_t lines = CountFileLines(entry.path());
    if (lines > kCap) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "architecture rule translation units should stay at or below " +
                     std::to_string(kCap) + " code lines (comments and blank lines excluded)",
      });
    }
  }
  return result;
}

RuleResult CheckWorkspaceArchitectureRulesDispatcherSize(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "WorkspaceArchitectureRules.cpp dispatcher size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 120;
  const std::filesystem::path path = repo_root / "tests/architecture/WorkspaceArchitectureRules.cpp";
  const std::size_t lines = CountFileLines(path);
  if (lines > kCap) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "WorkspaceArchitectureRules.cpp should stay a dispatcher at or below " +
                   std::to_string(kCap) +
                   " code lines (comments and blank lines excluded); add rule bodies under "
                   "tests/architecture/*ArchitectureRules.cpp",
    });
  }
  return result;
}

RuleResult CheckDescriptorCreationIsCloseOnExec(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "descriptor creation must request close-on-exec atomically";
  result.hard_fail = true;

  struct Form {
    std::string_view name;
    std::string_view flag;
    std::string_view advice;
  };
  const std::array<Form, 10> forms = {
      Form{"posix_openpt", "O_CLOEXEC", "pass O_CLOEXEC in posix_openpt()'s flags"},
      Form{"openpty", "O_CLOEXEC",
           "use posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC) and open(slave, O_CLOEXEC); "
           "openpty() cannot set close-on-exec atomically"},
      Form{"forkpty", "O_CLOEXEC",
           "use posix_openpt(O_CLOEXEC) + fork; forkpty() cannot set close-on-exec atomically"},
      Form{"socket", "SOCK_CLOEXEC", "pass SOCK_CLOEXEC in socket()'s type argument"},
      Form{"accept4", "SOCK_CLOEXEC",
           "pass SOCK_CLOEXEC to accept4() (and never plain accept())"},
      Form{"accept", "SOCK_CLOEXEC",
           "use accept4() with SOCK_CLOEXEC; plain accept() cannot set close-on-exec atomically"},
      Form{"openat", "O_CLOEXEC", "pass O_CLOEXEC in openat()'s flags"},
      Form{"open", "O_CLOEXEC", "pass O_CLOEXEC in open()'s flags"},
      Form{"pipe2", "O_CLOEXEC", "pass O_CLOEXEC to pipe2()"},
      Form{"inotify_init1", "IN_CLOEXEC", "pass IN_CLOEXEC to inotify_init1()"},
  };
  const std::regex creation_pattern(
      R"((^|[^\w:])(::)?(posix_openpt|openpty|forkpty|socket|accept4|accept|openat|open|pipe2|inotify_init1)\s*\()");

  std::size_t scanned_creation_sites = 0;
  for (const std::filesystem::path& dir : {repo_root / "src"}) {
    if (!RequireRuleTarget(result, dir)) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::filesystem::path extension = entry.path().extension();
      if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
        continue;
      }
      const std::string text = ReadText(entry.path());
      const std::vector<bool> is_code = BuildCodeMask(text);
      {
        for (std::sregex_iterator it(text.begin(), text.end(), creation_pattern), last;
             it != last; ++it) {
          const std::size_t open_paren =
              static_cast<std::size_t>(it->position() + it->length()) - 1;
          if (open_paren >= is_code.size() || !is_code[open_paren]) {
            continue;
          }
          const std::string matched_name = it->str(3);
          const auto form_it = std::find_if(forms.begin(), forms.end(), [&](const Form& candidate) {
            return candidate.name == matched_name;
          });
          if (form_it == forms.end()) {
            continue;
          }
          const Form& form = *form_it;
          ++scanned_creation_sites;
          std::size_t depth = 0;
          std::size_t close_paren = open_paren;
          for (std::size_t i = open_paren; i < text.size(); ++i) {
            if (text[i] == '(') {
              ++depth;
            } else if (text[i] == ')') {
              if (--depth == 0) {
                close_paren = i;
                break;
              }
            }
          }
          const std::string call = text.substr(open_paren, close_paren - open_paren + 1);
          if (call.find(form.flag) != std::string::npos) {
            continue;
          }
          result.violations.push_back(Violation{
              .path = entry.path(),
              .line = LineNumberAt(text, open_paren),
              .message = std::string("descriptor created without close-on-exec: ") +
                         std::string(form.advice) +
                         " — an unflagged descriptor is inherited by every child process the "
                         "editor spawns",
          });
        }
      }
    }
  }

  if (scanned_creation_sites == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/platform",
        .line = 1,
        .message = "rule target moved — no descriptor-creating calls found to scan; re-anchor "
                   "CheckDescriptorCreationIsCloseOnExec",
    });
  }
  return result;
}

RuleResult CheckSettingDefaultsMatchRegistry(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "bool setting reads use the registry's default";
  result.hard_fail = true;

  const std::filesystem::path registry_path =
      repo_root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp";
  const std::string registry_text = ReadRuleTarget(result, registry_path);
  if (registry_text.empty()) {
    result.violations.push_back(Violation{
        .path = registry_path,
        .line = 1,
        .message = "rule target moved — the settings registry could not be read; re-anchor "
                   "CheckSettingDefaultsMatchRegistry",
    });
    return result;
  }

  std::map<std::string, bool> bool_defaults;
  {
    const std::regex spec_pattern(R"RX(\.id\s*=\s*"([a-z][a-z0-9_.]*)")RX");
    for (std::sregex_iterator it(registry_text.begin(), registry_text.end(), spec_pattern), last;
         it != last; ++it) {
      const std::string key = (*it)[1].str();
      const std::size_t start = static_cast<std::size_t>(it->position());
      std::size_t end = registry_text.find(".id =", start + 1);
      if (end == std::string::npos) {
        end = registry_text.size();
      }
      const std::string body = registry_text.substr(start, end - start);
      if (body.find("SettingType::Bool") == std::string::npos) {
        continue;
      }
      bool_defaults[key] = body.find(".default_bool = true") != std::string::npos;
    }
  }
  if (bool_defaults.size() < 20) {
    result.violations.push_back(Violation{
        .path = registry_path,
        .line = 1,
        .message = "parsed only " + std::to_string(bool_defaults.size()) +
                   " bool settings; the registry's declaration shape changed and this lint has "
                   "gone vacuous — fix the scan rather than deleting the rule",
    });
    return result;
  }

  const std::regex read_pattern(R"(\bSettingFlagEnabled\s*\()");
  const std::regex literal_pattern(R"RX("([a-z][a-z0-9_.]*)")RX");
  std::size_t scanned_reads = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "WorkspaceSettingsRegistry.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), read_pattern), last; it != last; ++it) {
      const std::size_t open_paren = static_cast<std::size_t>(it->position() + it->length()) - 1;
      if (open_paren >= is_code.size() || !is_code[open_paren]) {
        continue;
      }
      std::size_t depth = 0;
      std::size_t close_paren = open_paren;
      for (std::size_t i = open_paren; i < text.size(); ++i) {
        if (text[i] == '(') {
          ++depth;
        } else if (text[i] == ')') {
          if (--depth == 0) {
            close_paren = i;
            break;
          }
        }
      }
      const std::string call = text.substr(open_paren, close_paren - open_paren + 1);
      std::smatch key_match;
      if (!std::regex_search(call, key_match, literal_pattern)) {
        continue;
      }
      const std::string key = key_match[1].str();
      const auto declared = bool_defaults.find(key);
      if (declared == bool_defaults.end()) {
        continue;
      }
      ++scanned_reads;
      const bool caller_default = call.find("true", key_match.position(0) + key.size()) !=
                                  std::string::npos;
      if (caller_default == declared->second) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, open_paren),
          .message = "setting `" + key + "` is registered with default " +
                     (declared->second ? "true" : "false") + " but read here with default " +
                     (caller_default ? "true" : "false") +
                     " — the two must agree, or the read silently disagrees with the Settings "
                     "overlay whenever the getter cannot resolve the key",
      });
    }
  }
  if (scanned_reads == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "found no bool setting reads to check; this lint's scan broke and it has gone "
                   "vacuous",
    });
  }
  return result;
}

RuleResult CheckSettingsReadAreRegistered(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "settings read in code must be declared in the settings registry";
  result.hard_fail = true;

  const std::filesystem::path registry_path =
      repo_root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp";
  const std::string registry_text = ReadRuleTarget(result, registry_path);
  if (registry_text.empty()) {
    result.violations.push_back(Violation{
        .path = registry_path,
        .line = 1,
        .message = "rule target moved — the settings registry could not be read; re-anchor "
                   "CheckSettingsReadAreRegistered",
    });
    return result;
  }
  std::vector<std::string> declared;
  const std::regex declared_pattern(R"RX(\.id\s*=\s*"([a-z][a-z0-9_.]*)")RX");
  for (std::sregex_iterator it(registry_text.begin(), registry_text.end(), declared_pattern), last;
       it != last; ++it) {
    declared.push_back((*it)[1].str());
  }
  if (declared.empty()) {
    result.violations.push_back(Violation{
        .path = registry_path,
        .line = 1,
        .message = "rule target moved — no setting declarations found; re-anchor "
                   "CheckSettingsReadAreRegistered",
    });
    return result;
  }

  const std::regex read_pattern(
      R"(\b(SettingFlagEnabled|SettingEnabled|SettingOn|SettingBoolIsOn)\s*\()");
  const std::regex literal_pattern(R"RX("([a-z][a-z0-9_.]*)")RX");
  std::size_t scanned_reads = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "WorkspaceSettingsRegistry.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), read_pattern), last; it != last; ++it) {
      const std::size_t open_paren = static_cast<std::size_t>(it->position() + it->length()) - 1;
      if (open_paren >= is_code.size() || !is_code[open_paren]) {
        continue;
      }
      std::size_t depth = 0;
      std::size_t close_paren = open_paren;
      for (std::size_t i = open_paren; i < text.size(); ++i) {
        if (text[i] == '(') {
          ++depth;
        } else if (text[i] == ')') {
          if (--depth == 0) {
            close_paren = i;
            break;
          }
        }
      }
      const std::string call = text.substr(open_paren, close_paren - open_paren + 1);
      std::smatch key_match;
      if (!std::regex_search(call, key_match, literal_pattern)) {
        continue;
      }
      const std::string key = key_match[1].str();
      if (key.find('.') == std::string::npos) {
        continue;
      }
      ++scanned_reads;
      if (std::find(declared.begin(), declared.end(), key) != declared.end()) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, open_paren),
          .message = "setting `" + key +
                     "` is read here but not declared in WorkspaceSettingsRegistry — it will read "
                       "as its default forever and stay invisible in the Settings overlay",
      });
    }
  }

  if (scanned_reads == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "rule target moved — no literal-keyed settings reads found to scan; re-anchor "
                   "CheckSettingsReadAreRegistered",
    });
  }
  return result;
}

RuleResult CheckGridRowTotalsAreVisualRows(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "text-grid row totals must be visual rows, not logical line counts";
  result.hard_fail = true;

  const std::regex call_pattern(
      R"(\b(ComputeScrollSurfaceLayout|ComputeTextGridInteractionLayout)\s*\()");
  std::size_t scanned_calls = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "WorkspaceLayout.cpp" ||
        entry.path().filename() == "WorkspaceLayout.h") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), call_pattern), last; it != last; ++it) {
      const std::size_t open_paren = static_cast<std::size_t>(it->position() + it->length()) - 1;
      if (open_paren >= is_code.size() || !is_code[open_paren]) {
        continue;
      }
      std::size_t depth = 0;
      std::size_t close_paren = open_paren;
      for (std::size_t i = open_paren; i < text.size(); ++i) {
        if (text[i] == '(') {
          ++depth;
        } else if (text[i] == ')') {
          if (--depth == 0) {
            close_paren = i;
            break;
          }
        }
      }
      ++scanned_calls;
      bool bare_line_count = false;
      for (std::size_t i = open_paren; i + 12 <= close_paren + 1; ++i) {
        if (i >= is_code.size() || !is_code[i]) {
          continue;
        }
        if (text.compare(i, 12, "line_count()") != 0) {
          continue;
        }
        if (i >= 7 && text.compare(i - 7, 7, "visual_") == 0) {
          continue;
        }
        bare_line_count = true;
        break;
      }
      if (!bare_line_count) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, open_paren),
          .message = "row total is `line_count()`, but the scroll it is paired with is a VISUAL "
                     "row — use visual_line_count() (or the surface's own visual-row count), or "
                     "the layout clamps the scroll into a space it does not belong to",
      });
    }
  }

  if (scanned_calls == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "rule target moved — no grid-layout calls found to scan; re-anchor "
                   "CheckGridRowTotalsAreVisualRows",
    });
  }
  return result;
}

const std::vector<NamedRule>& TerminalArchitectureRuleList() {
  static const std::vector<NamedRule> rules = {
      {"CheckGridRowTotalsAreVisualRows", CheckGridRowTotalsAreVisualRows},
      {"CheckTerminalSessionTuSize", CheckTerminalSessionTuSize},
      {"CheckTerminalSessionHeaderSize", CheckTerminalSessionHeaderSize},
      {"CheckTerminalSessionPrivateMethodCount", CheckTerminalSessionPrivateMethodCount},
      {"CheckTerminalHelperTuSize", CheckTerminalHelperTuSize},
      {"CheckTerminalParserHelpersNoForbiddenDeps", CheckTerminalParserHelpersNoForbiddenDeps},
      {"CheckTerminalSessionNoExtractedImpl", CheckTerminalSessionNoExtractedImpl},
      {"CheckTerminalInternalHeadersStayInTerminalDir", CheckTerminalInternalHeadersStayInTerminalDir},
      {"CheckTerminalSessionSplitTranslationUnits", CheckTerminalSessionSplitTranslationUnits},
      {"CheckArchitectureInvariantsDispatcherSize", CheckArchitectureInvariantsDispatcherSize},
      {"CheckArchitectureRulesTuSize", CheckArchitectureRulesTuSize},
      {"CheckWorkspaceArchitectureRulesDispatcherSize", CheckWorkspaceArchitectureRulesDispatcherSize},
      {"CheckDescriptorCreationIsCloseOnExec", CheckDescriptorCreationIsCloseOnExec},
      {"CheckSettingsReadAreRegistered", CheckSettingsReadAreRegistered},
      {"CheckRegisteredSettingsAreRead", CheckRegisteredSettingsAreRead},
      {"CheckSettingDefaultsMatchRegistry", CheckSettingDefaultsMatchRegistry},
  };
  return rules;
}

std::vector<RuleResult> RunTerminalArchitectureRules(const std::filesystem::path& repo_root) {
  std::vector<RuleResult> results;
  results.reserve(TerminalArchitectureRuleList().size());
  for (const NamedRule& rule : TerminalArchitectureRuleList()) {
    results.push_back(rule.fn(repo_root));
  }
  return results;
}

RuleResult CheckRegisteredSettingsAreRead(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "declared settings must be read by something";
  result.hard_fail = true;

  const std::filesystem::path registry = repo_root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp";
  const std::string registry_text = ReadRuleTarget(result, registry);
  const std::vector<bool> registry_is_code = BuildCodeMask(registry_text);
  const std::regex id_pattern(R"re(\.id\s*=\s*"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)")re");
  std::vector<std::string> declared;
  for (std::sregex_iterator it(registry_text.begin(), registry_text.end(), id_pattern), last;
       it != last; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    if (start < registry_is_code.size() && !registry_is_code[start]) {
      continue;
    }
    declared.push_back(it->str(1));
  }
  if (declared.size() < 20) {
    result.violations.push_back(Violation{
        .path = registry,
        .line = 1,
        .message = "parsed only " + std::to_string(declared.size()) +
                   " setting ids from the registry; the declaration shape changed and this lint "
                   "has gone vacuous — fix the scan rather than deleting the rule",
    });
    return result;
  }

  std::string other_sources;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "WorkspaceSettingsRegistry.cpp") {
      continue;
    }
    other_sources += ReadText(entry.path());
    other_sources += '\n';
  }

  for (const std::string& id : declared) {
    if (other_sources.find('"' + id + '"') != std::string::npos) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = registry,
        .line = 1,
        .message = "setting `" + id +
                   "` is declared but read by nothing — the Settings overlay shows it and "
                   "persists it while it does nothing. Wire it to a consumer, or drop the "
                   "declaration until the feature exists",
    });
  }
  return result;
}

}
