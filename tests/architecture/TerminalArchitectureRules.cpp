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
  // These patterns anchor a definition to the start of a LINE, so they need
  // std::regex::multiline. Without it `^` matches only at offset 0 of the whole
  // file — which for a .cpp is always `#include`, so three of these five
  // sub-checks could never fire and the other two only fired for one specific
  // return-type spelling. kLineAnchored is the single place that flag is set,
  // and ArchitectureRuleFixtures.cpp pins that each pattern still matches the
  // implementation shape it is meant to keep out.
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
  // 41: +1 for HasCustomScrollRegionLocked, the named predicate that gates
  // primary-buffer DECSTBM scroll-region behavior (used by AdvanceCursorRowLocked,
  // Reverse Index, and SU/SD) so the common full-screen path stays untouched.
  // 42: +1 for ResetEmulationStateLocked. This cap exists to stop the header
  // accreting helpers, so a helper that *removes* code earns its slot — that one
  // collapsed three inline ~35-line copies of the session reset (Start,
  // StartPlaceholderForTesting, Stop) that a comment asked readers to keep in
  // lockstep by hand, a net loss of ~90 lines from the .cpp.
  // 43: +1 for ReseedForStartLocked, on the same "earns its slot" terms. The
  // emulation-state reset was factored out but the rest of the start reseed was
  // not, so Start() and StartPlaceholderForTesting() still carried the same
  // label/geometry/bookkeeping block — including a duplicated comment explaining
  // why the geometry has to precede the reset. Folding it also let
  // TerminalSessionTestAccess::Reset drop its hand-kept ~35-field restatement of
  // the same state, which had already drifted (it never cleared
  // pending_clipboard_text_).
  // 44: +1 for CopyLineRangeIntoLocked, again on the "earns its slot" terms. Every
  // range-snapshot entry point (SnapshotLineRange, SnapshotLineRangeCached,
  // SnapshotLineRangeIfChanged, the new SnapshotLineRangeInto) had its own copy
  // loop, and three of them used clear()+assign() — which destroys each
  // TerminalLine's cells vector and rebuilds it, so a per-frame or per-wheel-tick
  // re-snapshot allocated and freed one vector per visible row every time. One
  // buffer-reusing body now backs all of them.
  constexpr std::size_t kCap = 44;
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

// Every descriptor the editor opens must be close-on-exec, set ATOMICALLY at
// creation. The editor spawns a lot of children — terminal shells, LSP servers,
// DAP adapters, git, plugin-launched tools — and any descriptor without the flag
// is inherited by all of them. For the control socket in particular that is a
// containment hole, not just a leak: the control channel is the interface that
// drives the editor headlessly, so a child would hold a live handle to it.
//
// A separate fcntl(F_SETFD) after the fact is NOT equivalent: another thread can
// fork/exec in the window between the two calls. This rule therefore requires
// the flag on the creating call itself.
//
// Matches are code-masked (BuildCodeMask) so occurrences inside string literals
// and comments — a syntax-highlight rule table naming `open(`, a settings/man-page
// string naming `socket(` — are not mistaken for call sites. The flag is searched
// for across the whole call expression, so a wrapped multi-line call still passes.
//
// Anchored on a required-presence count so the rule cannot go vacuous if these
// files move or are renamed.
RuleResult CheckDescriptorCreationIsCloseOnExec(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "descriptor creation must request close-on-exec atomically";
  result.hard_fail = true;

  struct Form {
    std::string_view name;
    std::string_view flag;
    std::string_view advice;
  };
  // NOTE ON THE `open` PATTERN: it must be `open(at)?`, NOT `openat?`. The `?`
  // binds to the single preceding character, so `openat?` matches `opena` /
  // `openat` and NEVER plain `open(` — which silently made this rule blind to the
  // exact call form it exists to police (it passed with two real unflagged
  // `open()` sites in the tree). The fixture below is the positive control.
  const std::array<Form, 7> forms = {
      Form{"socket", "SOCK_CLOEXEC", "pass SOCK_CLOEXEC in socket()'s type argument"},
      Form{"accept4", "SOCK_CLOEXEC",
           "pass SOCK_CLOEXEC to accept4() (and never plain accept())"},
      // Plain accept() cannot request close-on-exec at all, so every occurrence is
      // a violation: the flag string it is searched for can never appear in the
      // call, which is the point — the advice is "use accept4".
      Form{"accept", "SOCK_CLOEXEC",
           "use accept4() with SOCK_CLOEXEC; plain accept() cannot set close-on-exec atomically"},
      Form{"openat", "O_CLOEXEC", "pass O_CLOEXEC in openat()'s flags"},
      Form{"open", "O_CLOEXEC", "pass O_CLOEXEC in open()'s flags"},
      Form{"pipe2", "O_CLOEXEC", "pass O_CLOEXEC to pipe2()"},
      Form{"inotify_init1", "IN_CLOEXEC", "pass IN_CLOEXEC to inotify_init1()"},
  };
  // ONE pass over each file, not one per form. Six separate `std::sregex_iterator`
  // walks of every .cpp/.h/.inc in src cost 5.3 s natively, and an architecture
  // rule at that scale is one busy machine away from the runner's 300 s per-test
  // watchdog under TSAN — which is exactly how
  // ArchitectureInvariants/Workspace/CheckCoordinatorOperationsAreCalled took the
  // lane red (TD-2026-08-10-171's shape, again). The alternation is ordered
  // longest-first so `accept4` and `openat` win over their prefixes, which is what
  // the old `accept4`-before-`accept` form ordering and the `open(at)?` group did.
  const std::regex creation_pattern(
      R"((^|[^\w:])(::)?(socket|accept4|accept|openat|open|pipe2|inotify_init1)\s*\()");

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
          // The open paren is the final character of the pattern; require it to be
          // real code so string/comment occurrences are skipped.
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
            continue;  // unreachable: the alternation only produces known names
          }
          const Form& form = *form_it;
          ++scanned_creation_sites;
          // Scan the balanced argument list, so a call wrapped across lines is
          // still judged on its whole flag expression.
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

  // Loud-missing-target guard: if the scan finds no creation sites at all, the
  // rule has been hollowed out (files moved, spellings changed) and would pass
  // vacuously forever.
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

// Every bool-typed setting the code READS must be declared in
// WorkspaceSettingsRegistry. An unregistered key is not a compile error and not a
// runtime error — it simply reads as its default forever, so the feature it gates
// is invisible in the Settings overlay, absent from the docs, and reachable only
// by hand-editing a config file with a key name found in a source comment. Two
// shipped plugin-rendering toggles (plugins.inline_surfaces, plugins.code_lens_above)
// were in exactly that state.
//
// Scans the balanced argument list of each Setting* read helper for its first
// dotted string literal, code-masked so comments and unrelated literals are not
// mistaken for call sites.
// A bool setting's fallback default, where a caller spells one out, must match
// what the registry declares.
//
// GetSettingValue resolves an unset key to its registered default, so today the
// caller-supplied default only decides what happens when the getter itself is
// absent or unresolvable. That makes a disagreement latent rather than live —
// and latent is exactly why it goes unnoticed. Two were found this way:
// `chrome.project_tabs.hide_when_single` and `editor.inlay_hints.enabled` are
// both registered on-by-default, and both had a read that would have fallen back
// to off. `editor.inlay_hints.enabled` disagreed with *itself* — the same file
// read it twice, once with an explicit true and once with the implicit false.
//
// Reading with no explicit default means false, which is why an omitted default
// on an on-by-default setting is reported.
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

  // Registered Bool settings and their declared defaults. A SettingSpec with no
  // explicit `.default_bool` defaults to false.
  std::map<std::string, bool> bool_defaults;
  {
    const std::regex spec_pattern(R"RX(\.id\s*=\s*"([a-z][a-z0-9_.]*)")RX");
    for (std::sregex_iterator it(registry_text.begin(), registry_text.end(), spec_pattern), last;
         it != last; ++it) {
      const std::string key = (*it)[1].str();
      const std::size_t start = static_cast<std::size_t>(it->position());
      // The spec body runs until the next `.id =` (or the end).
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
        continue;  // reads a value held in a variable
      }
      const std::string key = key_match[1].str();
      const auto declared = bool_defaults.find(key);
      if (declared == bool_defaults.end()) {
        continue;  // not a registered bool setting; a sibling rule covers registration
      }
      ++scanned_reads;
      // The call's trailing argument, if any, is the caller's fallback default.
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
  // Declared ids: `.id = "..."` entries.
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
      continue;  // the declarations themselves
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
        continue;  // reads a value held in a variable; nothing to check
      }
      const std::string key = key_match[1].str();
      if (key.find('.') == std::string::npos) {
        continue;  // not a dotted setting id
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

const std::vector<NamedRule>& TerminalArchitectureRuleList() {
  static const std::vector<NamedRule> rules = {
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

// The mirror of CheckSettingsReadAreRegistered. That rule stops a setting being
// READ without being declared (invisible, unreachable). This one stops the
// opposite and equally user-visible failure: a setting DECLARED but read by
// nothing, which the Settings overlay renders with a label and a description, and
// persists when changed, while doing absolutely nothing.
//
// Two shipped that way — "Hover Delay (ms)" and "Scrollbar Size" — and neither
// had any consumer at all. A user could set them, see them stick across restarts,
// and never observe an effect.
//
// A setting whose value is reached through a computed key rather than a literal
// would false-positive here; none exist today, and the fix for one would be to
// name the id in a comment at the read site rather than to weaken the rule.
RuleResult CheckRegisteredSettingsAreRead(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "declared settings must be read by something";
  result.hard_fail = true;

  const std::filesystem::path registry = repo_root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp";
  const std::string registry_text = ReadRuleTarget(result, registry);
  const std::vector<bool> registry_is_code = BuildCodeMask(registry_text);
  // Custom delimiter: the pattern itself contains `)"`.
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

  // Any mention of the quoted id anywhere in src/ outside the registry counts as
  // a consumer; the read helpers are varied (SettingFlagEnabled, SettingIntValue,
  // direct SettingsStore lookups) and this rule is about reachability, not shape.
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

}  // namespace microide::tests::architecture
