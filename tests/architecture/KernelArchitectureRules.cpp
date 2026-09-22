#include "architecture/KernelArchitectureRules.h"

#include <array>
#include <functional>
#include <map>
#include <set>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {

namespace {

// The kernel: data, filesystem, process model and the terminal emulator, with no
// window. Everything here must compile and run in a process that never opened a
// display — which `microide_kernel_link_probe` demonstrates by doing it.
//
// Membership is READ FROM `MICROIDE_KERNEL_SOURCES` in CMakeLists.txt rather than
// restated here, because the build and the lint disagreeing about what the kernel is
// would be the one failure neither could report. The directory prefixes below are an
// additional root set, so a kernel HEADER that no kernel .cpp happens to include is
// still covered. `editor/` is not among them on purpose: it includes upward into
// `workspace/`, so directory is not layer there — its two SDL-free members
// (`SingleLineEditor`, `WordBoundary`) join the kernel by being named in the CMake
// list, not by living anywhere in particular.
constexpr std::array<std::string_view, 6> kKernelDirectories = {
    "util/", "platform/", "project/", "compare/", "persistence/", "terminal/",
};

bool IsKernelDirectory(std::string_view relative) {
  for (const std::string_view dir : kKernelDirectories) {
    if (relative.starts_with(dir)) {
      return true;
    }
  }
  return false;
}

// Parse `set(MICROIDE_KERNEL_SOURCES ... )` out of CMakeLists.txt, returning
// src-relative paths. An empty result means the list moved or was renamed, which the
// caller reports as a missing target rather than scanning a shrunken kernel.
std::vector<std::string> ReadKernelSourceList(const std::filesystem::path& repo_root) {
  std::vector<std::string> sources;
  const std::string text = ReadText(repo_root / "CMakeLists.txt");
  const std::size_t begin = text.find("set(MICROIDE_KERNEL_SOURCES");
  if (begin == std::string::npos) {
    return sources;
  }
  const std::size_t end = text.find("\n)", begin);
  const std::regex source_line(R"(^\s*src/([^\s#)]+\.cpp)\s*$)", std::regex::multiline);
  const std::string body = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                      : end - begin);
  for (std::sregex_iterator it(body.begin(), body.end(), source_line), last; it != last; ++it) {
    sources.push_back((*it)[1].str());
  }
  return sources;
}

bool IsSourceExtension(const std::filesystem::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".inc";
}

}  // namespace

RuleResult CheckKernelStaysFreeOfTheWindowingLibrary(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "the kernel does not reach the windowing library";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!RequireRuleTarget(result, src_dir)) {
    return result;
  }

  const std::regex local_include(R"RX(#\s*include\s*"([^"]+)")RX");
  const std::regex sdl_include(R"(#\s*include\s*<SDL3/[^>]+>)");
  // Direct token use without an include is just as much a dependency, and it is the
  // form a precompiled header hides: `Uint32` and `SDL_Color` compiled fine in every
  // kernel TU precisely because the PCH had already pulled SDL in.
  const std::regex sdl_token(R"(\bSDL_[A-Za-z0-9_]+|\bUint(?:8|16|32|64)\b|\bSint(?:8|16|32|64)\b)");

  // Pass 1: index every source file under src/ by its src-relative path, recording
  // whether it names SDL itself (in code, not in prose) and which local headers it
  // includes.
  struct FileFacts {
    std::filesystem::path path;
    bool names_sdl = false;
    std::vector<std::string> includes;
  };
  std::map<std::string, FileFacts, std::less<>> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path())) {
      continue;
    }
    const std::string key = std::filesystem::relative(entry.path(), src_dir).generic_string();
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    const auto code_match = [&](const std::regex& pattern) {
      for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        const std::size_t pos = static_cast<std::size_t>(it->position());
        if (pos >= is_code.size() || is_code[pos]) {
          return true;
        }
      }
      return false;
    };
    FileFacts facts;
    facts.path = entry.path();
    facts.names_sdl = code_match(sdl_include) || code_match(sdl_token);
    for (std::sregex_iterator it(text.begin(), text.end(), local_include), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos < is_code.size() && !is_code[pos]) {
        continue;
      }
      facts.includes.push_back((*it)[1].str());
    }
    files.emplace(key, std::move(facts));
  }
  if (files.empty()) {
    result.missing_targets.push_back(Violation{
        .path = src_dir,
        .line = 1,
        .message = "found no source files to scan; the kernel layering rule is blind",
    });
    return result;
  }

  // Pass 2: transitive closure, memoized. `taint_via` records WHICH include tainted a
  // file so the violation can print the route rather than just the verdict — a
  // one-line "reaches SDL" on a file that includes six headers is not actionable.
  // An include that resolves to nothing under src/ is reported as a missing target
  // rather than assumed clean: today the only such include in the tree is the
  // vendored `stb_image.h`, and a rule that silently swallows an unresolvable
  // include is a rule that goes blind the moment someone mistypes a path.
  std::map<std::string, int, std::less<>> memo;  // -1 in progress, 0 clean, 1 tainted
  std::map<std::string, std::string, std::less<>> taint_via;
  std::function<bool(const std::string&)> taints = [&](const std::string& key) -> bool {
    const auto it = files.find(key);
    if (it == files.end()) {
      return false;  // not under src/ (vendored); reported separately below
    }
    if (it->second.names_sdl) {
      return true;
    }
    const auto cached = memo.find(key);
    if (cached != memo.end()) {
      if (cached->second == -1) {
        return false;  // include cycle: already being evaluated higher up
      }
      return cached->second == 1;
    }
    memo[key] = -1;
    for (const std::string& next : it->second.includes) {
      if (taints(next)) {
        taint_via[key] = next;
        memo[key] = 1;
        return true;
      }
    }
    memo[key] = 0;
    return false;
  };

  // Roots: everything the build compiles into microide_kernel, plus every file in
  // the kernel directories (so a header no kernel .cpp includes is still covered).
  std::set<std::string, std::less<>> roots;
  const std::vector<std::string> cmake_kernel_sources = ReadKernelSourceList(repo_root);
  if (cmake_kernel_sources.empty()) {
    result.missing_targets.push_back(Violation{
        .path = repo_root / "CMakeLists.txt",
        .line = 1,
        .message = "could not read set(MICROIDE_KERNEL_SOURCES ...); the kernel layering "
                   "rule would fall back to directories and silently stop covering the "
                   "kernel files that live outside them",
    });
  }
  for (const std::string& source : cmake_kernel_sources) {
    if (files.contains(source)) {
      roots.insert(source);
    } else {
      result.missing_targets.push_back(Violation{
          .path = repo_root / "CMakeLists.txt",
          .line = 1,
          .message = "MICROIDE_KERNEL_SOURCES names src/" + source +
                     ", which does not exist; the build and this rule disagree about what "
                     "the kernel is",
      });
    }
  }
  for (const auto& [key, unused_facts] : files) {
    if (IsKernelDirectory(key)) {
      roots.insert(key);
    }
  }

  for (const std::string& key : roots) {
    const FileFacts& facts = files.find(key)->second;
    for (const std::string& included : facts.includes) {
      if (!files.contains(included) && included != "stb_image.h") {
        result.missing_targets.push_back(Violation{
            .path = facts.path,
            .line = 1,
            .message = "includes \"" + included +
                       "\", which resolves to nothing under src/; the kernel layering rule "
                       "cannot follow it and would pass this file vacuously",
        });
      }
    }
    if (!taints(key)) {
      continue;
    }
    std::string route;
    for (std::string step = key;;) {
      const auto next = taint_via.find(step);
      if (next == taint_via.end()) {
        break;
      }
      route += " -> ";
      route += next->second;
      step = next->second;
    }
    result.violations.push_back(Violation{
        .path = facts.path,
        .line = 1,
        .message = "kernel file reaches SDL" +
                   (route.empty() ? std::string(" directly") : route) +
                   "; the kernel must run in a process with no window (convert at the "
                   "shell boundary — util::Rgba8/util::KeyModifiers via render/SdlConvert.h, "
                   "util::WakeChannel via app/SdlWaker.h, util::Log for notices)",
    });
  }
  return result;
}

RuleResult CheckEverySpawnGoesThroughAProcessLauncher(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every spawn goes through a ProcessLauncher";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!RequireRuleTarget(result, src_dir)) {
    return result;
  }

  // Locality is OWNED, not chosen per call: a project holds a launcher and every spawn
  // that belongs to it goes through that launcher, or a session silently splits across
  // two machines. Both spawn primitives are covered — scoping this to RunSubprocess
  // would let the language server and the debug adapter, the two whose answers are
  // line numbers in the user's buffer, escape it entirely.
  //
  // Only two files may name the primitives:
  //   * ProcessLauncher.cpp, which IS the launcher; and
  //   * HostIntegration.cpp, whose `xdg-open` must NEVER follow the project —
  //     opening a file manager on the build server is always wrong — and which says
  //     so at the call.
  // The primitives' own definitions are excluded by path, not allowlisted.
  static constexpr std::array<const char*, 5> kExemptSuffixes = {
      "src/platform/ProcessLauncher.cpp", "src/platform/ProcessLauncher.h",
      "src/platform/Subprocess.cpp",      "src/platform/Subprocess.h",
      "src/platform/AsyncSubprocess.cpp",
  };
  const std::regex run_subprocess(R"(\bRunSubprocess\s*\()");
  // The asynchronous half is checked per FILE rather than per call. Matching the call
  // shape would mean pinning an argument name or a receiver spelling, and a rule whose
  // pattern stops matching its own call form is the failure mode this repo keeps
  // finding (dev-docs/project/validation-traps.md). A file that starts an
  // AsyncSubprocess must name ResolveArgv; that is what the language-server and the
  // debug-adapter spawns do, and it is coarse in the safe direction.
  const std::regex async_start(R"(\.Start\s*\()");
  const std::regex async_type(R"(\bAsyncSubprocess\b)");
  const std::regex resolve_argv(R"(\bResolveArgv\s*\()");
  bool saw_any_primitive = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path())) {
      continue;
    }
    const std::string generic = entry.path().generic_string();
    bool exempt = false;
    for (const char* suffix : kExemptSuffixes) {
      if (generic.ends_with(suffix)) {
        exempt = true;
        break;
      }
    }
    const std::string text = ReadText(entry.path());
    if (exempt) {
      // The exempt files are also where the primitives must actually BE. If none of
      // them names one any more, the rule is scanning for a call form that no longer
      // exists and would pass forever.
      if (std::regex_search(text, run_subprocess)) {
        saw_any_primitive = true;
      }
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, run_subprocess,
        "spawns must go through a platform::ProcessLauncher (LocalProcessLauncher() "
        "when the spawn must never follow the project, and say why), not "
        "platform::RunSubprocess directly");
    if (std::regex_search(text, async_type) && std::regex_search(text, async_start) &&
        !std::regex_search(text, resolve_argv)) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "starts an AsyncSubprocess without resolving its argv through a "
                     "platform::ProcessLauncher; a language server or debug adapter "
                     "started on the wrong machine reports line numbers for a file "
                     "nobody is looking at",
      });
    }
  }
  if (!saw_any_primitive) {
    result.missing_targets.push_back(Violation{
        .path = src_dir / "platform" / "ProcessLauncher.cpp",
        .line = 1,
        .message = "found no RunSubprocess call in the launcher itself; this rule is "
                   "scanning for a call form the tree no longer uses",
    });
  }
  return result;
}

const std::vector<NamedRule>& KernelArchitectureRuleList() {
  static const std::vector<NamedRule> rules = {
      {"CheckKernelStaysFreeOfTheWindowingLibrary", CheckKernelStaysFreeOfTheWindowingLibrary},
      {"CheckEverySpawnGoesThroughAProcessLauncher", CheckEverySpawnGoesThroughAProcessLauncher},
  };
  return rules;
}

}  // namespace microide::tests::architecture
