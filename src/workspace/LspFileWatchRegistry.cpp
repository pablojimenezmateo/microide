#include "workspace/LspFileWatchRegistry.h"

#include <system_error>
#include <utility>

#include "project/GlobMatch.h"
#include "workspace/FileUri.h"

namespace microide::workspace {
namespace {

using util::JsonValue;

int WatchKindMaskFor(LspFileChangeType change) {
  switch (change) {
    case LspFileChangeType::Created:
      return kLspWatchKindCreate;
    case LspFileChangeType::Changed:
      return kLspWatchKindChange;
    case LspFileChangeType::Deleted:
      return kLspWatchKindDelete;
  }
  return 0;
}

// Normalize a path to the forward-slash, unrooted form GlobMatches expects.
std::string NormalizeGlobText(const std::filesystem::path& path) {
  std::string text = path.generic_string();
  while (text.starts_with("./")) {
    text.erase(0, 2);
  }
  return text;
}

// Resolve a `globPattern` — either a plain string (relative to the workspace) or a
// RelativePattern `{baseUri, pattern}` — into concrete patterns, appending each to
// the relative or absolute bucket. Splitting the buckets here means the per-file
// match loop never tests an absolute pattern against a relative path (or the
// reverse), so the hot path does exactly one comparison per pattern.
void AppendResolvedPatterns(const JsonValue& glob_pattern,
                            const std::filesystem::path& project_root,
                            std::vector<std::string>& relative_out,
                            std::vector<std::string>& absolute_out) {
  std::string pattern;
  std::filesystem::path base;

  if (glob_pattern.IsString()) {
    pattern = glob_pattern.AsString();
  } else if (glob_pattern.IsObject() && glob_pattern.HasKey("pattern")) {
    const JsonValue& pattern_value = glob_pattern["pattern"];
    if (!pattern_value.IsString()) {
      return;
    }
    pattern = pattern_value.AsString();
    // `baseUri` is either a plain URI string or a WorkspaceFolder {uri, name}.
    const JsonValue& base_uri = glob_pattern["baseUri"];
    if (base_uri.IsString()) {
      if (auto decoded = PathFromFileUri(base_uri.AsString()); decoded.has_value()) {
        base = std::move(*decoded);
      }
    } else if (base_uri.IsObject() && base_uri["uri"].IsString()) {
      if (auto decoded = PathFromFileUri(base_uri["uri"].AsString()); decoded.has_value()) {
        base = std::move(*decoded);
      }
    }
  } else {
    return;
  }

  if (pattern.empty()) {
    return;
  }

  // A RelativePattern anchored inside the project collapses to a relative pattern
  // (cheaper to match, and it keeps the common in-project case on the short
  // string). One anchored outside it stays absolute.
  if (!base.empty()) {
    std::error_code error;
    const std::filesystem::path relative_base =
        project_root.empty() ? std::filesystem::path{}
                             : std::filesystem::relative(base, project_root, error);
    const std::string relative_text =
        error ? std::string{} : NormalizeGlobText(relative_base);
    const bool inside_project =
        !relative_text.empty() && relative_text != ".." && !relative_text.starts_with("../");
    std::string joined;
    if (inside_project) {
      joined = relative_text == "." ? pattern : relative_text + "/" + pattern;
      project::ExpandGlobBraces(std::move(joined), relative_out);
    } else {
      joined = NormalizeGlobText(base) + "/" + pattern;
      project::ExpandGlobBraces(std::move(joined), absolute_out);
    }
    return;
  }

  // A bare pattern starting at the filesystem root is absolute; everything else is
  // workspace-relative, which is the shape servers actually send ("**/*.rs").
  if (pattern.starts_with('/')) {
    project::ExpandGlobBraces(std::move(pattern), absolute_out);
  } else {
    project::ExpandGlobBraces(std::move(pattern), relative_out);
  }
}

void TruncateToCap(std::vector<std::string>& relative_patterns,
                   std::vector<std::string>& absolute_patterns) {
  if (relative_patterns.size() > kMaxLspFileWatchPatternsPerRegistration) {
    relative_patterns.resize(kMaxLspFileWatchPatternsPerRegistration);
  }
  const std::size_t remaining =
      kMaxLspFileWatchPatternsPerRegistration - relative_patterns.size();
  if (absolute_patterns.size() > remaining) {
    absolute_patterns.resize(remaining);
  }
}

}  // namespace

bool LspFileWatchRegistry::Register(const util::JsonValue& registration,
                                    const std::filesystem::path& project_root) {
  if (registrations_.size() >= kMaxLspFileWatchRegistrations) {
    return false;
  }
  const JsonValue& options = registration["registerOptions"];
  const JsonValue& watchers = options["watchers"];
  if (!watchers.IsArray()) {
    return false;
  }

  const std::string id = registration["id"].IsString() ? registration["id"].AsString()
                                                       : std::string{};

  Registration entry;
  entry.id = id;
  // A registration carrying several watchers with different `kind` masks would
  // need one entry per mask to stay exact. Servers register a single mask in
  // practice, so we OR the masks and keep one entry: the cost of being wrong is
  // an occasional extra notification, which is strictly better than dropping one.
  int kind_mask = 0;
  const util::JsonArray& watcher_array = watchers.AsArray();
  for (const JsonValue& watcher : watcher_array) {
    if (!watcher.IsObject()) {
      continue;
    }
    AppendResolvedPatterns(watcher["globPattern"], project_root, entry.relative_patterns,
                           entry.absolute_patterns);
    const JsonValue& kind = watcher["kind"];
    kind_mask |= kind.IsInt() ? static_cast<int>(kind.AsInt(kLspWatchKindAll))
                              : kLspWatchKindAll;
  }
  if (entry.relative_patterns.empty() && entry.absolute_patterns.empty()) {
    return false;
  }
  entry.kind = kind_mask == 0 ? kLspWatchKindAll : kind_mask;
  TruncateToCap(entry.relative_patterns, entry.absolute_patterns);

  // Re-registering an existing id replaces it (the LSP spec has no "update", so a
  // server that re-registers means to supersede).
  if (!entry.id.empty()) {
    Unregister(entry.id);
  }
  registrations_.push_back(std::move(entry));
  return true;
}

bool LspFileWatchRegistry::Unregister(std::string_view id) {
  const auto before = registrations_.size();
  std::erase_if(registrations_,
                [id](const Registration& entry) { return entry.id == id; });
  return registrations_.size() != before;
}

void LspFileWatchRegistry::Clear() { registrations_.clear(); }

bool LspFileWatchRegistry::WantsChange(std::string_view relative_path,
                                       std::string_view absolute_path,
                                       LspFileChangeType change) const {
  const int wanted = WatchKindMaskFor(change);
  for (const Registration& entry : registrations_) {
    if ((entry.kind & wanted) == 0) {
      continue;
    }
    for (const std::string& pattern : entry.relative_patterns) {
      if (project::GlobMatches(pattern, relative_path)) {
        return true;
      }
    }
    for (const std::string& pattern : entry.absolute_patterns) {
      if (project::GlobMatches(pattern, absolute_path)) {
        return true;
      }
    }
  }
  return false;
}

std::size_t LspFileWatchRegistry::PatternCountForTesting() const {
  std::size_t total = 0;
  for (const Registration& entry : registrations_) {
    total += entry.relative_patterns.size() + entry.absolute_patterns.size();
  }
  return total;
}

}  // namespace microide::workspace
