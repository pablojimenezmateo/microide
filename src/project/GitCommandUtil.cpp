#include "project/GitCommandUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <climits>
#include <sys/stat.h>
#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif
#endif

#include "platform/Subprocess.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project::internal {

bool HasGitMarker(const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
#if !defined(_WIN32)
  // POSIX fast path: assemble `<root>/.git` in a stack buffer and stat it.
  // `root / ".git"` is not free — the resulting path allocates its own string
  // plus libstdc++'s component list, ~167 bytes across the pair — and this runs
  // on the background executor for essentially every git operation as well as
  // once per painted frame from the status bar (TD-2026-08-06-158). `native()`
  // hands back the string that already exists, so this path allocates nothing.
  //
  // Same failure policy as the fallback below: any stat error, not just ENOENT,
  // reads as "no usable repository here".
  {
    const std::string& native = root.native();
    constexpr std::string_view kMarker = "/.git";
    char buffer[PATH_MAX];
    if (native.size() + kMarker.size() + 1 <= sizeof(buffer)) {
      std::size_t end = native.size();
      std::memcpy(buffer, native.data(), end);
      while (end > 0 && buffer[end - 1] == '/') {
        --end;  // "/" and "/repo/" must not produce "//.git" or "/repo//.git"
      }
      std::memcpy(buffer + end, kMarker.data(), kMarker.size());
      end += kMarker.size();
      buffer[end] = '\0';
      struct ::stat info;
      return ::stat(buffer, &info) == 0;
    }
  }
#endif
  // Use the non-throwing overload: this runs on the background-executor thread on
  // essentially every git operation, and the throwing exists() aborts the process
  // when the failure is not "does not exist" (a parent dir losing +x, an unmounted
  // network volume, ENAMETOOLONG). Those must degrade to "repo unavailable", not
  // crash. Matches every other filesystem probe in this subsystem.
  std::error_code error;
  return std::filesystem::exists(root / ".git", error) && !error;
}

namespace {

// `lexically_normal()`, skipped when the text already is normal. Every path this
// subsystem holds arrives normalized (the project catalog, the git status ingress
// and the branch-review store all normalize once on the way in), and the call is
// ~12 allocations even when it changes nothing (TD-2026-08-10-174).
std::filesystem::path NormalizedCopy(const std::filesystem::path& path) {
  if (util::PathTextNeedsNormalizing(path.native())) {
    return path.lexically_normal();
  }
  return path;
}

std::optional<std::filesystem::path> ComputeAbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  const std::filesystem::path normalized_root = NormalizedCopy(root);
  const std::filesystem::path normalized_path = NormalizedCopy(absolute_path);

  // The overwhelmingly common answer -- the path sits under the root -- is a
  // prefix strip, not a component walk. Taking it here matters on the MISS path
  // of the memo below, which is where a bulk resolve lives: a git sidebar refresh
  // resolves every changed file once, and each of those is a memo miss.
  if (util::NormalizedPathEqualsOrWithin(normalized_path, normalized_root)) {
    const std::string_view relative_text =
        util::NormalizedRelativeView(normalized_path.native(), normalized_root.native());
    if (!relative_text.empty()) {
      return std::filesystem::path(relative_text);
    }
    return std::filesystem::path(".");
  }

  const std::filesystem::path relative = normalized_path.lexically_relative(normalized_root);
  if (relative.empty() ||
      (relative.begin() != relative.end() &&
       *relative.begin() == std::filesystem::path(".."))) {
#if defined(_WIN32)
    const std::string root_text = normalized_root.generic_string();
    const std::string path_text = normalized_path.generic_string();
    std::string lowered_root = util::ToLowerAscii(root_text);
    std::string lowered_path = util::ToLowerAscii(path_text);
    const std::string lowered_root_prefix =
        lowered_root.ends_with('/') ? lowered_root : lowered_root + "/";
    const std::string root_prefix = root_text.ends_with('/') ? root_text : root_text + "/";
    if (lowered_path == lowered_root) {
      return std::filesystem::path(".");
    }
    if (lowered_path.size() > lowered_root_prefix.size() &&
        lowered_path.rfind(lowered_root_prefix, 0) == 0) {
      return std::filesystem::path(path_text.substr(root_prefix.size())).lexically_normal();
    }
#endif
    return std::nullopt;
  }
  return NormalizedCopy(relative);
}

}  // namespace

std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty()) {
    return std::nullopt;
  }

  // Memoized because this is on the per-frame render path and it is expensive
  // out of all proportion to what it does: `lexically_normal` builds a fresh
  // path component by component, so one call is ~12 allocations, and the inline
  // blame overlay resolves the SAME two paths three to four times on every frame
  // it paints (the overlay's own eligibility check, then GitBlameService's
  // Request and Snapshot). Measured with the phase-scoped allocation tracer that
  // TD-2026-08-06-151 made aimable: 11 of the top 12 sites in
  // editor_fold_viewport_refresh's measured phase were this call, ~20% of the
  // phase's 31,079 allocations.
  //
  // Memoizing is correct by construction rather than by convention.
  // `lexically_normal` and `lexically_relative` are PURELY LEXICAL -- neither
  // touches the filesystem -- so the result is a pure function of the two
  // arguments and there is no state it could go stale against. That is the whole
  // reason the cache needs no invalidation hook, no generation counter, and no
  // participation in any watcher.
  //
  // Per thread, so it needs no lock: the shell thread paints while the
  // background executor runs git, and both call this. Four entries because a
  // split editor with two panes plus a git-sidebar refresh touches more than one
  // (root, path) pair between frames, and a one-entry memo would thrash to a 0%
  // hit rate exactly when there is most to save.
  struct Entry {
    std::filesystem::path root;
    std::filesystem::path absolute_path;
    std::optional<std::filesystem::path> result;
  };
  constexpr std::size_t kMemoEntries = 4;
  thread_local std::array<Entry, kMemoEntries> memo{};
  thread_local std::size_t memo_used = 0;

  for (std::size_t i = 0; i < memo_used; ++i) {
    if (memo[i].root != root || memo[i].absolute_path != absolute_path) {
      continue;
    }
    if (i != 0) {
      std::rotate(memo.begin(), memo.begin() + static_cast<std::ptrdiff_t>(i),
                  memo.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    }
    return memo[0].result;
  }

  Entry entry{.root = root,
              .absolute_path = absolute_path,
              .result = ComputeAbsoluteToRelativePath(root, absolute_path)};
  // Evict the least recently used by rotating it to the back, then overwrite it.
  std::rotate(memo.begin(), memo.end() - 1, memo.end());
  memo[0] = std::move(entry);
  memo_used = std::min(memo_used + 1, kMemoEntries);
  return memo[0].result;
}

std::optional<std::string> ResolveHeadId(const std::filesystem::path& root) {
  const auto result = ReadGitCommandOutput(root, {"rev-parse", "--verify", "HEAD"});
  if (!result.success() || result.output.empty()) {
    return std::nullopt;
  }

  std::string head_id = result.output;
  util::TrimTrailingLineEndings(&head_id);
  return head_id.empty() ? std::nullopt : std::make_optional(std::move(head_id));
}

std::optional<std::filesystem::path> ResolveGitDirectory(const std::filesystem::path& root) {
  if (root.empty()) {
    return std::nullopt;
  }
  // Non-throwing probes throughout: this can run against an unmounted network
  // volume or a directory that lost +x, and those must degrade to "no git dir"
  // rather than abort the process (same reasoning as HasGitMarker).
  const std::filesystem::path marker = root / ".git";
  std::error_code error;
  if (std::filesystem::is_directory(marker, error) && !error) {
    return marker;
  }
  error.clear();
  if (!std::filesystem::is_regular_file(marker, error) || error) {
    return std::nullopt;
  }
  const std::optional<std::string> contents = util::ReadTextFile(marker);
  if (!contents.has_value()) {
    return std::nullopt;
  }
  constexpr std::string_view kGitDirPrefix = "gitdir:";
  std::string_view line(*contents);
  if (const std::size_t newline = line.find('\n'); newline != std::string_view::npos) {
    line = line.substr(0, newline);
  }
  if (!line.starts_with(kGitDirPrefix)) {
    return std::nullopt;
  }
  line.remove_prefix(kGitDirPrefix.size());
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    line.remove_prefix(1);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return std::nullopt;
  }
  std::filesystem::path git_dir(line);
  // A `.git` file may hold either an absolute path or one relative to the
  // worktree root.
  return git_dir.is_absolute() ? git_dir : (root / git_dir).lexically_normal();
}

std::optional<std::string> ReadPendingMergeHeadId(const std::filesystem::path& root) {
  const std::optional<std::filesystem::path> git_dir = ResolveGitDirectory(root);
  if (!git_dir.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string> contents = util::ReadTextFile(*git_dir / "MERGE_HEAD");
  if (!contents.has_value()) {
    return std::nullopt;
  }
  std::string_view first_line(*contents);
  if (const std::size_t newline = first_line.find('\n'); newline != std::string_view::npos) {
    first_line = first_line.substr(0, newline);
  }
  while (!first_line.empty() &&
         (first_line.back() == '\r' || first_line.back() == ' ' || first_line.back() == '\t')) {
    first_line.remove_suffix(1);
  }
  // Only accept a plain object id. A ref file holding anything else (a symref, a
  // truncated write mid-merge) must not be surfaced as a commit label.
  if (first_line.size() < 7 || first_line.size() > 64) {
    return std::nullopt;
  }
  for (const char byte : first_line) {
    const bool is_hex = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                        (byte >= 'A' && byte <= 'F');
    if (!is_hex) {
      return std::nullopt;
    }
  }
  return std::string(first_line);
}

CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr,
                                   int timeout_ms) {
  return ReadGitCommandOutputWithStdin(root, std::move(arguments), {}, silence_stderr, timeout_ms);
}

CommandResult ReadGitCommandOutputWithStdin(const std::filesystem::path& root,
                                            std::vector<std::string> arguments,
                                            std::string stdin_text,
                                            bool silence_stderr,
                                            int timeout_ms) {
  // Label by subcommand only. This is the single choke point every git call in
  // the app funnels through, so `git.status` vs `git.blame` vs `git.diff` is the
  // breakdown worth having; the operands (paths, revs, sha1s) would mint a fresh
  // label per invocation and turn the ranked summary back into a log.
  util::PerformanceTrace::ScopeLabel label("git::RunCommand");
  if (!arguments.empty()) {
    label.Field("sub", arguments.front());
  }
  util::PerformanceTrace::Scope perf_scope(label.View());

  std::vector<std::string> command;
  command.reserve(arguments.size() + 4);
  command.emplace_back("git");
  // Suppress optional index refresh so read-only commands (status, blame, etc.)
  // never touch .git/index.lock; concurrent invocations and killed subprocesses
  // previously left stale locks that blocked the user's own `git commit`.
  command.emplace_back("--no-optional-locks");
  // Force every pathspec across all git commands to be a literal path. Without
  // this, a file whose name begins with git pathspec magic — `:(glob)`, `:(top)`,
  // `:(exclude)`, `:!…`, etc. — passed after `--` still triggers that magic and
  // could stage/discard/blame/diff/history the wrong path. No git call in this
  // codebase intentionally uses pathspec magic, so this is a pure safety gate.
  command.emplace_back("--literal-pathspecs");
  command.emplace_back("-C");
  command.push_back(root.lexically_normal().string());
  for (std::string& argument : arguments) {
    command.push_back(std::move(argument));
  }
  platform::SubprocessOptions options;
  // Never let git block on an interactive credential/passphrase prompt. The child
  // inherits our stdio, so a git launched from a terminal can read a password from
  // a tty the user is not looking at and simply hang until the wall-clock cap kills
  // it (300 s for write ops). With this, git fails immediately and reports why,
  // which the caller can surface. A configured GUI askpass still works — only the
  // terminal prompt is disabled.
  options.environment_overrides.push_back({"GIT_TERMINAL_PROMPT", "0"});
  options.environment_overrides.push_back({"GCM_INTERACTIVE", "never"});
  options.capture_stdout = true;
  options.capture_stderr = !silence_stderr;
  options.silence_stderr = silence_stderr;
  options.stdin_text = std::move(stdin_text);
  options.timeout_ms = timeout_ms;
  const platform::SubprocessResult result = platform::RunSubprocess(command, options);
  util::AddPerformanceCounter(util::PerfCounterId::GitCommandsRun);
  util::AddPerformanceCounter(util::PerfCounterId::GitCommandOutputBytes,
                              result.stdout_text.size() + result.stderr_text.size());
  if (result.exit_code != 0) {
    util::AddPerformanceCounter(util::PerfCounterId::GitCommandFailures);
  }
  std::string output = result.stdout_text;
  if (!silence_stderr && !result.stderr_text.empty()) {
    if (!output.empty() && output.back() != '\n') {
      output.push_back('\n');
    }
    output += result.stderr_text;
  }
  return CommandResult{
      .exit_code = result.exit_code,
      .output = std::move(output),
      .timed_out = result.timed_out,
      .truncated = result.truncated,
  };
}

bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr) {
  return ReadGitCommandOutput(root, std::move(arguments), silence_stderr).success();
}

}  // namespace microide::project::internal
