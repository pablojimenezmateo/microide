#include "architecture/TerminalArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <fstream>
#include <regex>
#include <string_view>
#include <algorithm>

namespace microide::tests::architecture {

namespace {

std::size_t CountFileLines(const std::filesystem::path& path) {
  return CountCodeLinesInFile(path);
}

}  // namespace

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
    const std::string text = ReadText(path);
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
  const std::string text = ReadText(path);
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"((?:std::optional<int>|^int)\s+Base64Value\s*\()"),
      "TerminalSession.cpp must not contain base64 decoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"((?:std::optional<std::string>|^std::optional)\s+DecodeBase64\s*\()"),
      "TerminalSession.cpp must not contain base64 decoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(^std::vector<int>\s+ParseCsiParameters\s*\()"),
      "TerminalSession.cpp must not contain CSI parameter parser implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(^int\s+MouseModifierBits\s*\()"),
      "TerminalSession.cpp must not contain mouse encoder implementation");
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(^bool\s+EncodeTerminalMouseEvent\s*\()"),
      "TerminalSession.cpp must not contain mouse encoder implementation");
  return result;
}

RuleResult CheckTerminalSessionHeaderSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TerminalSession.h header size";
  result.hard_fail = true;
  constexpr std::size_t kCap = 300;
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
  constexpr std::size_t kCap = 40;
  const std::filesystem::path path = repo_root / "src/terminal/TerminalSession.h";
  const std::string text = ReadText(path);
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
    if (!std::filesystem::exists(root)) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::filesystem::path path = entry.path();
      if (path.extension() != ".cpp" && path.extension() != ".h" &&
          path.extension() != ".inc") {
        continue;
      }
      const std::string relative = path.lexically_relative(repo_root).generic_string();
      if (relative.rfind("src/terminal/", 0) == 0) {
        continue;
      }
      const std::string text = ReadText(path);
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
  const std::array<std::string_view, 8> allowed = {
      "TerminalSessionOutput.cpp",
      // Escape handling is split by sequence family: CSI dispatch, SGR styling,
      // OSC, and DEC private/keyboard modes (was the monolithic
      // TerminalSessionEscape.cpp).
      "TerminalSessionCsi.cpp",
      "TerminalSessionSgr.cpp",
      "TerminalSessionOsc.cpp",
      "TerminalSessionModes.cpp",
      "TerminalSessionInput.cpp",
      "TerminalSessionScreen.cpp",
      "TerminalSessionInputEncoding.cpp",
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

std::vector<RuleResult> RunTerminalArchitectureRules(const std::filesystem::path& repo_root) {
  return {
      CheckTerminalSessionTuSize(repo_root),
      CheckTerminalSessionHeaderSize(repo_root),
      CheckTerminalSessionPrivateMethodCount(repo_root),
      CheckTerminalHelperTuSize(repo_root),
      CheckTerminalParserHelpersNoForbiddenDeps(repo_root),
      CheckTerminalSessionNoExtractedImpl(repo_root),
      CheckTerminalInternalHeadersStayInTerminalDir(repo_root),
      CheckTerminalSessionSplitTranslationUnits(repo_root),
      CheckArchitectureInvariantsDispatcherSize(repo_root),
      CheckArchitectureRulesTuSize(repo_root),
      CheckWorkspaceArchitectureRulesDispatcherSize(repo_root),
  };
}

}  // namespace microide::tests::architecture
