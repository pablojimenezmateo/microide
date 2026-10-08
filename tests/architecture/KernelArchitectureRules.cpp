#include "architecture/KernelArchitectureRules.h"

#include <algorithm>
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
//
// `server/` is not kernel but is held to the same rule: microide-server links the
// kernel and nothing else, and runs on hosts that have no display at all.
constexpr std::array<std::string_view, 7> kKernelDirectories = {
    "util/", "platform/", "project/", "compare/", "persistence/", "terminal/", "server/",
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

// microide-server links microide_kernel and its own sources, nothing else
// (CMakeLists.txt). A server TU that includes a shell header still COMPILES when the
// header is inline-only, and then the server quietly depends on the shell's layer;
// the link only catches the out-of-line half. So: a src/server file includes only
// kernel directories and src/server itself.
RuleResult CheckServerIncludesOnlyTheKernel(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "microide-server includes only the kernel";
  result.hard_fail = true;
  const std::filesystem::path server_dir = repo_root / "src" / "server";
  if (!RequireRuleTarget(result, server_dir)) {
    return result;
  }
  const std::regex local_include(R"RX(#\s*include\s*"([^"]+)")RX");
  std::size_t scanned = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(server_dir)) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path())) {
      continue;
    }
    ++scanned;
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), local_include), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos < is_code.size() && !is_code[pos]) {
        continue;
      }
      const std::string included = (*it)[1].str();
      if (IsKernelDirectory(included)) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1,
          .message = "includes \"" + included +
                     "\", outside the kernel; the server links microide_kernel only",
      });
    }
  }
  if (scanned == 0) {
    result.missing_targets.push_back(Violation{
        .path = server_dir,
        .line = 1,
        .message = "found no server sources; this rule is scanning nothing",
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
  // Only ProcessLauncher.cpp may name the primitives: it IS the launcher. A spawn
  // that must never follow the project — `xdg-open` in HostIntegration.cpp, because
  // opening a file manager on the build server is always wrong — still goes through
  // a launcher, the explicit `LocalProcessLauncher()`, and so needs no exemption.
  // The primitives' own definitions are excluded by path, not allowlisted.
  static constexpr std::array<const char*, 5> kExemptSuffixes = {
      "src/platform/ProcessLauncher.cpp", "src/platform/ProcessLauncher.h",
      "src/platform/Subprocess.cpp",      "src/platform/Subprocess.h",
      "src/platform/AsyncSubprocess.cpp",
  };
  // The vacuity guard below watches THIS file alone, and for a CALL. Scoping it to
  // "any exempt file" is what it used to do, and Subprocess.h is exempt and declares
  // `SubprocessResult RunSubprocess(...)` — so a declaration satisfied the guard
  // forever and the rule could go blind while still reporting green, which is the
  // failure this suite exists to prevent (dev-docs/project/validation-traps.md).
  static constexpr std::string_view kLauncherSuffix = "src/platform/ProcessLauncher.cpp";
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
      // The launcher is also where the primitive must actually BE. If it stops
      // calling one, the rule is scanning for a call form that no longer exists and
      // would pass forever. A declaration or a mention in a comment does not count.
      if (generic.ends_with(kLauncherSuffix) && CodeMaskedPatternAppears(text, run_subprocess)) {
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
        .message = "found no RunSubprocess call in ProcessLauncher.cpp itself; this "
                   "rule is scanning for a call form the tree no longer uses",
    });
  }
  return result;
}

RuleResult CheckEveryUserSaveRunsTheSamePreparation(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every user-initiated save runs the same preparation";
  result.hard_fail = true;

  // `TextViewport::Save()` writes the buffer and nothing else. The save PARTICIPANTS,
  // format-on-save and the disk-conflict check all live above it, so a call site that
  // reaches Save() directly is a second save door with different behaviour — and the
  // difference is invisible until someone notices that "Save" in the dirty prompt did
  // not run their formatter, or, as it actually happened, that it overwrote a file that
  // had changed on disk with no banner.
  //
  // Enforced per FILE: a file that calls `<something viewport>.Save()` must also name a
  // preparation. That is coarse in the safe direction and does not depend on a receiver
  // spelling, which is the pattern-rot this repo keeps finding. Two files are exempt,
  // and both are PROGRAMMATIC saves rather than user-initiated ones — the same
  // distinction autosave already makes when it suppresses formatters.
  static constexpr std::array<const char*, 2> kProgrammaticSaveFiles = {
      // Applying an LSP workspace edit to buffers that happen to be open. The edit is
      // the server's, not the user's; running a formatter over it would rewrite a
      // rename's result behind the user's back.
      "src/workspace/shell/WorkspaceShellPlugins.cpp",
      // The same, for files that are NOT open: a scratch viewport used purely as a
      // read-modify-write of a closed file.
      "src/workspace/lsp/LspService.cpp",
  };
  // Any argument list: `Save` takes the project's write gate since TD-2026-09-29-308,
  // and a pattern pinned to `Save()` would have stopped matching every call at once.
  const std::regex viewport_save(R"([Vv]iewport(_)?\s*(\.|->)\s*Save\s*\()");
  // Only the preparation itself counts. An earlier draft also accepted `SaveGroupTab`
  // and `EditorTabService`, which appear in most of the workspace, so every file it
  // scanned passed on a name that had nothing to do with preparing a save.
  const std::regex prepares(R"(prepare_editor_view_for_save|PrepareEditorViewportForSave)");

  bool saw_any_save = false;
  for (const char* rel : kProgrammaticSaveFiles) {
    if (!RequireRuleTarget(result, repo_root / rel)) {
      continue;
    }
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path())) {
      continue;
    }
    const std::string generic = entry.path().generic_string();
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    bool calls_save = false;
    for (std::sregex_iterator it(text.begin(), text.end(), viewport_save), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos >= is_code.size() || is_code[pos]) {
        calls_save = true;
        break;
      }
    }
    if (!calls_save) {
      continue;
    }
    saw_any_save = true;
    bool programmatic = false;
    for (const char* rel : kProgrammaticSaveFiles) {
      if (generic.ends_with(rel)) {
        programmatic = true;
        break;
      }
    }
    if (programmatic || std::regex_search(text, prepares)) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = entry.path(),
        .line = 1,
        .message = "calls TextViewport::Save() without naming the shared preparation "
                   "(prepare_editor_view_for_save / PrepareEditorViewportForSave), and is "
                   "not one of the documented programmatic saves. A user-initiated save "
                   "must run the same save participants and "
                   "format-on-save, and refuse the same disk conflicts, wherever it was "
                   "triggered from",
    });
  }
  if (!saw_any_save) {
    result.missing_targets.push_back(Violation{
        .path = repo_root / "src",
        .line = 1,
        .message = "found no TextViewport::Save() call at all; this rule is scanning for a "
                   "call form the tree no longer uses",
    });
  }
  return result;
}

RuleResult CheckProjectWritesGoThroughTheWriteGate(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "project-tree writes go through the write gate";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!RequireRuleTarget(result, src_dir)) {
    return result;
  }

  // `util::WriteTextFileAtomically` is the primitive that replaces a file's
  // contents. Six subsystems used to call it (or its neighbours) directly and share
  // nothing above it: no common notion of what the write was computed against, no
  // serialization between them, and no single place that knows a file under the
  // project root just changed — which is why each of them grew its own post-write
  // fixup. `project::FileWriteGate` is that place, and it is the seam a remote
  // project needs (MirrorWriteGate writes into the mirror and enqueues the push).
  //
  // Scope, stated precisely because a lint that implies more than it checks is
  // worse than one that admits its edges (dev-docs/project/validation-traps.md).
  //
  // Two halves.
  //
  // CONTENT: the primitives that replace a file's contents —
  // `WriteTextFileAtomically` and a writing `std::ofstream` — in the three layers
  // that hold project files open.
  //
  // TREE (TD-2026-09-29-305): create/rename/delete/trash. The platform primitives
  // that do them (`MovePath*`, `RemovePath`, `CopyPath`, `MovePathToTrash`) may not
  // be named in those layers at all — the gate's ApplyTreeOps is the door, with one
  // transactional journal instead of the two that used to exist. And the
  // `std::filesystem` mutators are banned in the three directories whose files act
  // on the project tree (coordinators/, lsp/, git/). Not everywhere in the gated
  // layers: persistence, the tool-download cache, the plugin data directory and the
  // control descriptor all legitimately create and remove files that are NOT in a
  // project tree, and naming that list file by file is how this rule would stop
  // meaning anything.
  static constexpr std::array<const char*, 3> kGatedDirectories = {
      "workspace/", "plugin/", "editor/",
  };
  // Files in a gated layer that do not write into a project tree, each named with
  // the reason rather than lumped together — an allowlist whose entries are not
  // individually justified is how a rule stops meaning anything.
  static constexpr std::array<const char*, 1> kWritersOutsideTheGate = {
      // Publishes the per-instance control descriptor under $XDG_RUNTIME_DIR. Not
      // a project file at all, and its temp-then-rename is its own atomicity
      // contract for a concurrent reader racing startup.
      "workspace/control/ControlChannelService.cpp",
  };
  const std::regex raw_write(
      R"(\bWriteTextFileAtomically\s*\(|\bstd::ofstream\s+\w+\s*\()");
  const std::regex raw_tree_primitive(
      R"(\b(?:MovePathToTrash|MovePathNoOverwrite|MovePath|RemovePath|CopyPath)\s*\()");
  const std::regex raw_fs_mutation(
      R"(\b(?:std::filesystem|fs)::(?:rename|remove|remove_all|create_directory|create_directories)\s*\()");
  static constexpr std::array<const char*, 3> kTreeActingDirectories = {
      "workspace/coordinators/", "workspace/lsp/", "workspace/git/",
  };

  bool saw_gate_call = false;
  bool saw_tree_primitive = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path())) {
      continue;
    }
    const std::string key = std::filesystem::relative(entry.path(), src_dir).generic_string();
    const std::string text = ReadText(entry.path());
    if (key.starts_with("project/FileWriteGate")) {
      if (std::regex_search(text, raw_write)) {
        saw_gate_call = true;
      }
      continue;
    }
    if (key.starts_with("project/LocalTreeOps")) {
      if (CodeMaskedPatternAppears(text, raw_tree_primitive)) {
        saw_tree_primitive = true;
      }
      continue;
    }
    bool gated_layer = false;
    for (const std::string_view dir : kGatedDirectories) {
      if (key.starts_with(dir)) {
        gated_layer = true;
        break;
      }
    }
    if (!gated_layer) {
      continue;
    }
    bool outside_the_gate = false;
    for (const std::string_view exempt : kWritersOutsideTheGate) {
      if (key == exempt) {
        outside_the_gate = true;
        break;
      }
    }
    if (outside_the_gate) {
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, raw_write,
        "a write that replaces a file in a project tree must go through "
        "project::FileWriteGate, not util::WriteTextFileAtomically directly — the gate "
        "is what captures the post-write signature and what a remote project replaces");
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, raw_tree_primitive,
        "a create/rename/delete/trash in a project tree goes through "
        "FileWriteGate::ApplyTreeOps (the project's write_gate()), not a platform "
        "primitive — the gate is the one journal, and what a remote project replaces");
    for (const std::string_view dir : kTreeActingDirectories) {
      if (key.starts_with(dir)) {
        AppendCodeMaskRegexViolations(
            result, entry.path(), text, raw_fs_mutation,
            "this directory acts on the project tree: a create/rename/remove goes through "
            "FileWriteGate::ApplyTreeOps (the project's write_gate()), not std::filesystem");
        break;
      }
    }
  }
  if (!saw_tree_primitive) {
    result.missing_targets.push_back(Violation{
        .path = src_dir / "project" / "LocalTreeOps.cpp",
        .line = 1,
        .message = "the local tree operations no longer call a platform move primitive; "
                   "the tree half of this rule is scanning for a call form the tree no "
                   "longer uses (repoint it)",
    });
  }
  if (!saw_gate_call) {
    result.missing_targets.push_back(Violation{
        .path = src_dir / "project" / "FileWriteGate.cpp",
        .line = 1,
        .message = "the gate no longer calls WriteTextFileAtomically; either the primitive "
                   "was renamed (repoint this rule) or the gate stopped writing — either "
                   "way the rule is scanning for a call form the tree no longer uses",
    });
  }
  return result;
}

RuleResult CheckGitLayerDoesNotChooseLocality(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "the git layer takes its launcher, it does not choose one";
  result.hard_fail = true;
  const std::filesystem::path project_dir = repo_root / "src" / "project";
  if (!RequireRuleTarget(result, project_dir)) {
    return result;
  }

  // TD-2026-09-22-301 threaded the project's launcher through every git entry
  // point in three slices: status, the read side, then the write side and blame.
  // Before it, 17 `LocalProcessLauncher()` constructions lived INSIDE these units,
  // each one a place where a remote project's git would quietly run on this
  // machine — against whatever tree happens to share the path. The caller knows
  // which project a query belongs to; the git layer never does, so it must not be
  // the one deciding. A spawn that must stay local whatever the project says
  // (xdg-open) lives outside this layer and names the local launcher there.
  //
  // Scoped to .cpp: a copyable request struct (GitBlameRequest, PatchApplyRequest)
  // needs SOME default for its launcher pointer, and the request's builder is what
  // overwrites it — that is a value, not a decision.
  const std::regex local_launcher(R"(\bLocalProcessLauncher\s*\()");
  // Vacuity guard: the rule is only meaningful while these files still build
  // GitRepository objects from a launcher they were handed. If nothing in scope
  // constructs one, the layer was renamed or moved and this scans nothing.
  const std::regex repository_from_launcher(
      R"(\bGitRepository\s+\w+\s*\(\s*\w[\w.]*\s*,\s*\*?\w+)");
  bool saw_repository_construction = false;
  for (const auto& entry : std::filesystem::directory_iterator(project_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("Git") && !name.starts_with("Commit") &&
        !name.starts_with("PatchApply")) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    if (CodeMaskedPatternAppears(text, repository_from_launcher)) {
      saw_repository_construction = true;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, local_launcher,
        "the git layer must take the launcher from its caller (the project's, via "
        "ProjectWorkspaceState::launcher()), not construct LocalProcessLauncher() "
        "itself — a remote project's git would silently run on this machine");
  }
  if (!saw_repository_construction) {
    result.missing_targets.push_back(Violation{
        .path = project_dir,
        .line = 1,
        .message = "no src/project/Git*/Commit*/PatchApply* .cpp constructs a GitRepository "
                   "any more; the git layer moved or was renamed and this rule scans nothing "
                   "(repoint it)",
    });
  }
  return result;
}

RuleResult CheckGitMetadataIsAskedOfTheHost(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "a .git probe asks the project's GitMetadataSource, not this machine";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!RequireRuleTarget(result, src_dir)) {
    return result;
  }
  // TD-2026-10-06-319: every place that decided "is this a repository?" or "where is
  // the git directory?" did it by statting `.git` under the project root on THIS
  // machine. A remote project's root is a mirror with no `.git`, so each of those
  // answered "not a repository" and the git call it guarded never reached the host.
  // They now ask GitMetadataFor(launcher). The two primitives stay legal where they
  // are defined, in the local source that wraps them, and in the metadata tracker's
  // change sampling (a watcher of the LOCAL git directory, which a remote project
  // replaces with pushed metadata -- the rest of G9).
  const std::regex probe(R"(\b(HasGitMarker|ResolveGitDirectory)\s*\()");
  constexpr std::string_view kAllowed[] = {
      "project/GitCommandUtil.cpp",
      "project/GitCommandUtil.h",
      "project/GitMetadataSource.cpp",
      "project/GitRepositoryMetadataTracker.cpp",
  };
  bool saw_local_source_probe = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    const std::string extension = entry.path().extension().string();
    if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h")) {
      continue;
    }
    const std::string key = entry.path().lexically_relative(src_dir).generic_string();
    const std::string text = ReadText(entry.path());
    if (key == "project/GitMetadataSource.cpp") {
      saw_local_source_probe = CodeMaskedPatternAppears(text, probe);
    }
    if (std::find(std::begin(kAllowed), std::end(kAllowed), key) != std::end(kAllowed)) {
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, probe,
        "ask project::GitMetadataFor(launcher) -- the host's .git -- instead of statting "
        "one under the root on this machine: a remote project's root is a mirror with no "
        ".git, so a local probe answers 'not a repository' and git is never asked");
  }
  if (!saw_local_source_probe) {
    result.missing_targets.push_back(Violation{
        .path = src_dir / "project" / "GitMetadataSource.cpp",
        .line = 1,
        .message = "the local metadata source no longer calls HasGitMarker/ResolveGitDirectory; "
                   "the primitives were renamed or moved and this rule scans for a call form "
                   "the tree no longer uses (repoint it)",
    });
  }
  return result;
}

const std::vector<NamedRule>& KernelArchitectureRuleList() {
  static const std::vector<NamedRule> rules = {
      {"CheckKernelStaysFreeOfTheWindowingLibrary", CheckKernelStaysFreeOfTheWindowingLibrary},
      {"CheckServerIncludesOnlyTheKernel", CheckServerIncludesOnlyTheKernel},
      {"CheckEverySpawnGoesThroughAProcessLauncher", CheckEverySpawnGoesThroughAProcessLauncher},
      {"CheckGitLayerDoesNotChooseLocality", CheckGitLayerDoesNotChooseLocality},
      {"CheckGitMetadataIsAskedOfTheHost", CheckGitMetadataIsAskedOfTheHost},
      {"CheckEveryUserSaveRunsTheSamePreparation", CheckEveryUserSaveRunsTheSamePreparation},
      {"CheckProjectWritesGoThroughTheWriteGate", CheckProjectWritesGoThroughTheWriteGate},
  };
  return rules;
}

}  // namespace microide::tests::architecture
