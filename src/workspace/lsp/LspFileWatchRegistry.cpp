#include "workspace/lsp/LspFileWatchRegistry.h"

#include <cstdint>
#include <system_error>
#include <utility>

#include "project/GlobMatch.h"
#include "util/PerformanceCounters.h"
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

std::string NormalizeGlobText(const std::filesystem::path& path) {
  std::string text = path.generic_string();
  while (text.starts_with("./")) {
    text.erase(0, 2);
  }
  return text;
}

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

}

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

bool LspFileWatchRegistry::WantsChange(std::string_view relative_path,
                                       std::string_view absolute_path,
                                       LspFileChangeType change) const {
  const int wanted = WatchKindMaskFor(change);
  std::uint64_t glob_tests = 0;
  bool matched = false;
  for (const Registration& entry : registrations_) {
    if ((entry.kind & wanted) == 0) {
      continue;
    }
    for (const std::string& pattern : entry.relative_patterns) {
      ++glob_tests;
      if (project::GlobMatches(pattern, relative_path)) {
        matched = true;
        break;
      }
    }
    if (matched) {
      break;
    }
    for (const std::string& pattern : entry.absolute_patterns) {
      ++glob_tests;
      if (project::GlobMatches(pattern, absolute_path)) {
        matched = true;
        break;
      }
    }
    if (matched) {
      break;
    }
  }
  util::AddPerformanceCounter(util::PerfCounterId::LspWatchedFileGlobTests, glob_tests);
  return matched;
}

std::size_t LspFileWatchRegistry::PatternCountForTesting() const {
  std::size_t total = 0;
  for (const Registration& entry : registrations_) {
    total += entry.relative_patterns.size() + entry.absolute_patterns.size();
  }
  return total;
}

}
