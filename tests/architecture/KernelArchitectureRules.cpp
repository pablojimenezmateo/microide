#include "architecture/KernelArchitectureRules.h"

#include <array>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {

namespace {

// The kernel: the directories whose contents are data, I/O and process model, with
// no window. Everything here must compile and run in a process that never opened a
// display — that is what makes a headless `microide-agent` a couple of MB rather
// than thirty, and it is a layering this tree half-had and never stated.
//
// `editor/`, `render/`, `workspace/`, `plugin/` and `app/` are deliberately NOT in
// the set: `editor/` already includes upward into `workspace/WorkspaceUiText.h`, so
// directory is not layer there and pretending otherwise would just produce an
// allowlist. Individual SDL-free editor headers (`SingleLineEditor.h`, the compare
// value types) are reachable from the kernel and stay reachable: the rule follows
// the include graph, so a header earns kernel membership by being clean, not by
// living in a blessed directory.
constexpr std::array<std::string_view, 6> kKernelDirectories = {
    "util/", "platform/", "project/", "compare/", "persistence/", "terminal/",
};

bool IsKernelPath(std::string_view relative) {
  for (const std::string_view dir : kKernelDirectories) {
    if (relative.starts_with(dir)) {
      return true;
    }
  }
  return false;
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

  for (const auto& [key, facts] : files) {
    if (!IsKernelPath(key)) {
      continue;
    }
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

const std::vector<NamedRule>& KernelArchitectureRuleList() {
  static const std::vector<NamedRule> rules = {
      {"CheckKernelStaysFreeOfTheWindowingLibrary", CheckKernelStaysFreeOfTheWindowingLibrary},
  };
  return rules;
}

}  // namespace microide::tests::architecture
