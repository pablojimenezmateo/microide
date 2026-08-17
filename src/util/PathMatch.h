#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

// True when two paths denote the same location once normalized. The raw `==`
// comparison runs first so the common case (both already normalized, which is how
// the workspace stores document paths) never allocates the temporaries that
// `lexically_normal()` produces; the normalization only happens on a mismatch.
[[nodiscard]] inline bool SamePathNormalized(const std::filesystem::path& a,
                                             const std::filesystem::path& b) {
  return a == b || a.lexically_normal() == b.lexically_normal();
}

// True when `candidate` is `root` itself or a path nested under it, for inputs the
// caller has ALREADY normalized. Allocation-free: it answers with a prefix compare
// on the two native strings instead of building the `lexically_normal()` /
// `lexically_relative()` / `generic_string()` temporaries the general entry point
// below needs (four allocations per call). Use it wherever the containment check
// sits in a per-entry loop — the project traversal filter runs it once per
// filesystem entry of every index walk.
//
// Passing a NON-normalized path here is a caller bug, not UB: the answer then
// simply reflects the textual form ("a/./b" is not seen as within "a/b").
[[nodiscard]] inline bool NormalizedPathEqualsOrWithin(const std::filesystem::path& candidate,
                                                       const std::filesystem::path& root) {
  const std::filesystem::path::string_type& candidate_text = candidate.native();
  const std::filesystem::path::string_type& root_text = root.native();
  if (candidate_text.empty() || root_text.empty()) {
    return false;
  }
  if (candidate_text == root_text) {
    return true;
  }
  const auto is_separator = [](std::filesystem::path::value_type c) {
#ifdef _WIN32
    return c == L'\\' || c == L'/';
#else
    return c == '/';
#endif
  };
  // A "." root is the current directory, whose members are spelled without any
  // "./" prefix once normalized ("./x" normalizes to "x"), so the prefix compare
  // below would miss them. Every relative path that does not climb out is within.
  if (root_text.size() == 1 && root_text[0] == '.') {
    if (candidate.is_absolute()) {
      return false;
    }
    const bool climbs_out = candidate_text.size() >= 2 && candidate_text[0] == '.' &&
                            candidate_text[1] == '.' &&
                            (candidate_text.size() == 2 || is_separator(candidate_text[2]));
    return !climbs_out;
  }
  if (candidate_text.size() <= root_text.size() ||
      candidate_text.compare(0, root_text.size(), root_text) != 0) {
    return false;
  }
  // A root that already ends in a separator ("/", "C:\") needs no extra one; any
  // other root must be followed by a separator so "/a/bc" is not "within" "/a/b".
  return is_separator(root_text.back()) || is_separator(candidate_text[root_text.size()]);
}

// True when `candidate` denotes the same location as `normalized_reference`, which
// the caller has ALREADY normalized (once, outside the loop).
//
// This is the shape a SCAN needs, and it is not `SamePathNormalized`. A scan is
// mostly mismatches, and that helper normalizes BOTH sides on every mismatch — so
// looking one path up among twenty open tabs costs ~19 x 24 allocations to answer
// "no" nineteen times. Here a mismatch between two already-normal paths is a
// string compare and nothing else; only an unusually spelled candidate normalizes,
// and the reference never does.
[[nodiscard]] inline bool SameAsNormalizedPath(const std::filesystem::path& candidate,
                                               const std::filesystem::path& normalized_reference);

// The portion of `candidate_text` that is relative to `root_text`, as a view INTO
// `candidate_text` — the allocation-free companion to
// `NormalizedPathEqualsOrWithin`, whose preconditions it shares: both texts are
// already lexically normal, and the containment check above has already returned
// true for them. Returns an empty view when the two denote the same location.
//
// This exists so a per-entry traversal decision can be made without the
// `lexically_relative()` + `lexically_normal()` + `generic_string()` temporaries
// (a dozen-odd allocations) that deriving the same text through `path` costs. The
// containment check has already established the prefix; the relative part is that
// prefix removed, and nothing else.
[[nodiscard]] inline std::string_view NormalizedRelativeView(std::string_view candidate_text,
                                                             std::string_view root_text) {
  const auto is_separator = [](char c) {
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
  };
  if (root_text.empty() || candidate_text == root_text) {
    return {};
  }
  // A "." root is the current directory; its members are spelled with no prefix
  // at all once normalized, so the whole candidate is the relative part.
  if (root_text.size() == 1 && root_text[0] == '.') {
    return candidate_text;
  }
  std::size_t offset = root_text.size();
  if (!is_separator(root_text.back())) {
    ++offset;  // skip the separator the containment check proved is there
  }
  if (offset >= candidate_text.size()) {
    return {};
  }
  return candidate_text.substr(offset);
}

// The directory part of an already-normalized path's text, as a view into it —
// what `path::parent_path()` would yield, without constructing a path. An empty
// view means the path has no directory part (a bare relative filename).
[[nodiscard]] inline std::string_view NormalizedParentDirectoryView(std::string_view text) {
#ifdef _WIN32
  const std::size_t last_separator = text.find_last_of("\\/");
#else
  const std::size_t last_separator = text.find_last_of('/');
#endif
  if (last_separator == std::string_view::npos) {
    return {};
  }
  if (last_separator == 0) {
    return text.substr(0, 1);  // the filesystem root itself keeps its separator
  }
  return text.substr(0, last_separator);
}

// The final component of an already-normalized path's text, as a view into it —
// what `path::filename()` would yield, without constructing a path. The companion
// to `NormalizedParentDirectoryView`, and the reason it exists: a traversal that
// asks "is this name hidden?" per entry was spending a path plus a string on the
// answer (`path.filename().string()[0] == '.'`).
[[nodiscard]] inline std::string_view NormalizedFileNameView(std::string_view text) {
#ifdef _WIN32
  const std::size_t last_separator = text.find_last_of("\\/");
#else
  const std::size_t last_separator = text.find_last_of('/');
#endif
  return last_separator == std::string_view::npos ? text : text.substr(last_separator + 1);
}

// True when `text` — a path's own separator-separated spelling — is NOT already in
// lexically-normal form, i.e. when `lexically_normal()` would produce something
// different and therefore has to run.
//
// `lexically_normal()` costs ~12 allocations (a fresh path plus a component list
// with a string per component). Almost every path the workspace holds is already
// normalized: the git status ingress, the branch-review store and the project
// catalog all normalize once on the way in. This is the allocation-free scan that
// confirms it, so a hot loop can spend one string instead of one path per item and
// still fall back to the authoritative form for anything unusual.
//
// A normalized path contains no "." component, no ".." component, and no empty
// component other than a leading one (the root separator) or a trailing one (the
// directory-form separator, which `lexically_normal()` preserves).
[[nodiscard]] inline bool PathTextNeedsNormalizing(std::string_view text) {
  const auto is_separator = [](char c) {
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
  };
  if (text.empty()) {
    return false;
  }
  std::size_t begin = 0;
  while (true) {
    std::size_t end = begin;
    while (end < text.size() && !is_separator(text[end])) {
      ++end;
    }
    const std::string_view component = text.substr(begin, end - begin);
    if (component.empty() && begin != 0 && end != text.size()) {
      return true;  // an interior "//" run
    }
    if (component == "." || component == "..") {
      return true;
    }
    if (end >= text.size()) {
      return false;
    }
    begin = end + 1;
  }
}

// `path.lexically_normal()`, skipped when the path is ALREADY normal.
//
// `lexically_normal()` is ~12 allocations: a fresh path plus a component list
// with a string per component, built eagerly by libstdc++'s constructor. Almost
// every path the workspace holds is already normal (the git ingress, the project
// catalog and the branch-review store each normalize once on the way in), so the
// call is usually an expensive way to reproduce its own input.
//
// Two forms, and the second is the one a hot path wants:
//   - `NormalizedPath` returns an owned path. Still a path COPY (two
//     allocations) in the already-normal case, but not twelve.
//   - `NormalizedPathView` returns a reference to the input when it is already
//     normal and to `scratch` otherwise, so the common case allocates nothing at
//     all. The reference is valid as long as both arguments are.
[[nodiscard]] inline std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
  return PathTextNeedsNormalizing(path.native()) ? path.lexically_normal() : path;
}

[[nodiscard]] inline const std::filesystem::path& NormalizedPathView(
    const std::filesystem::path& path,
    std::filesystem::path& scratch) {
  if (!PathTextNeedsNormalizing(path.native())) {
    return path;
  }
  scratch = path.lexically_normal();
  return scratch;
}

inline bool SameAsNormalizedPath(const std::filesystem::path& candidate,
                                 const std::filesystem::path& normalized_reference) {
  if (candidate.native() == normalized_reference.native()) {
    return true;
  }
  if (!PathTextNeedsNormalizing(candidate.native())) {
    return false;
  }
  return candidate.lexically_normal().native() == normalized_reference.native();
}

// `PathEqualsOrWithin` for a candidate that may not be normalized yet, without
// paying `lexically_normal()` when it already is. The eager-argument form
// (`PathEqualsOrWithin(x.lexically_normal(), root)`) builds a whole path before
// the containment test can reject it.
[[nodiscard]] inline bool PathEqualsOrWithinNormalized(const std::filesystem::path& candidate,
                                                       const std::filesystem::path& root);

// True when `candidate` is `root` itself or a path nested under it. Purely
// lexical: it never touches the filesystem (no symlink resolution, no cwd
// dependency), which keeps it cheap enough for hot path-prefix scans in the
// diagnostics/decoration stores and deterministic regardless of the process's
// working directory. Empty inputs are never "within" anything.
[[nodiscard]] inline bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                                             const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return false;
  }
  if (candidate == root) {
    return true;  // already-normalized identical paths: skip the normalize cost
  }
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }
  const std::filesystem::path relative = normalized_candidate.lexically_relative(normalized_root);
  if (relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
}

// Forward-slashed, project-relative representation of `candidate` when it is nested
// under `root`, else nullopt. Purely lexical — never touches the filesystem — so it
// stays correct and cheap on deleted paths, symlinks, or inaccessible mounts, where
// std::filesystem::relative would stat/canonicalize and can fail, slow, or depend on
// the process cwd. Returns nullopt when either input is empty, when candidate is root
// itself, or when candidate escapes root via `..`. Use this for display labels and
// any prefix trimming that must not incur a syscall per path.
[[nodiscard]] inline std::optional<std::string> RelativePathWithin(
    const std::filesystem::path& candidate, const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relative =
      candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return std::nullopt;
  }
  std::string text = relative.generic_string();
  if (text == "." || text == ".." || text.rfind("../", 0) == 0) {
    return std::nullopt;
  }
  return text;
}

// Rewrite `path` so a leading `old_prefix` becomes `new_prefix`, returning the
// normalized result. Paths outside `old_prefix` are returned unchanged (still
// normalized). Purely lexical like PathEqualsOrWithin — no filesystem access, no
// symlink resolution — so it is safe to call once per stored path when remapping
// after a rename without incurring a syscall per entry.
[[nodiscard]] inline std::filesystem::path ReplacePathPrefix(
    const std::filesystem::path& path,
    const std::filesystem::path& old_prefix,
    const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_old_prefix = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new_prefix = new_prefix.lexically_normal();
  // The inputs are normalized right here, so use the variant that does not
  // normalize them a second time.
  if (!NormalizedPathEqualsOrWithin(normalized_path, normalized_old_prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_old_prefix) {
    return normalized_new_prefix;
  }
  const std::filesystem::path relative =
      normalized_path.lexically_relative(normalized_old_prefix);
  if (relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

inline bool PathEqualsOrWithinNormalized(const std::filesystem::path& candidate,
                                         const std::filesystem::path& root) {
  std::filesystem::path scratch;
  return PathEqualsOrWithin(NormalizedPathView(candidate, scratch), root);
}

}  // namespace microide::util
