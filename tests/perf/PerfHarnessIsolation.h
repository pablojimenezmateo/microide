#pragma once

#include <filesystem>
#include <string>

namespace microide::tests::perf {

// Establish a per-process isolated app sandbox: a fresh directory under the
// system temp dir, populated with empty `config`, `state`, `cache`, and `data`
// subdirectories and exported via the four XDG_*_HOME env vars. Must be called
// before any workspace or SDL initialization so platform path resolution sees
// the sandbox, not real user state.
//
// When `keep_artifacts` is true the path is per-PID (stable enough for triage)
// and SHALL NOT be deleted at shutdown. Otherwise the path includes a random
// suffix and SHOULD be removed via `CleanupIsolatedAppRoot`.
std::filesystem::path EstablishIsolatedAppRoot(bool keep_artifacts, std::string* error = nullptr);

// Best-effort cleanup. No-op when `root` is empty or `keep` is true.
void CleanupIsolatedAppRoot(const std::filesystem::path& root, bool keep);

}  // namespace microide::tests::perf
