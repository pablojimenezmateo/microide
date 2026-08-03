#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace {

// LSP `FileChangeType` (workspace/didChangeWatchedFiles). The wire values are
// normative — they are serialized straight into the notification.
enum class LspFileChangeType : int {
  Created = 1,
  Changed = 2,
  Deleted = 3,
};

// LSP `WatchKind` bitmask from DidChangeWatchedFilesRegistrationOptions. The
// spec's default when a watcher omits `kind` is all three.
inline constexpr int kLspWatchKindCreate = 1;
inline constexpr int kLspWatchKindChange = 2;
inline constexpr int kLspWatchKindDelete = 4;
inline constexpr int kLspWatchKindAll =
    kLspWatchKindCreate | kLspWatchKindChange | kLspWatchKindDelete;

// The set of file watchers a language server registered through
// `client/registerCapability` for method `workspace/didChangeWatchedFiles`.
//
// Why this exists: every mainstream server (clangd, rust-analyzer, gopls,
// tsserver) learns about on-disk edits it did not make — a `git switch`, a
// `pull`, a generated header — only through that notification. Without it the
// server keeps serving definitions and diagnostics from a pre-change index for
// every file the user does not happen to have open.
//
// We advertise `dynamicRegistration: true` and honor the registered globs rather
// than blasting every change at every server, because a branch switch in a large
// repository is thousands of paths and most of them are of no interest to any
// given server.
//
// Threading: registrations arrive on the client's I/O thread and are queried from
// the shell thread, so the owner (LspClient::Impl) guards this under its mutex and
// keeps a lock-free `has_file_watchers` atomic for the no-registration fast path.
class LspFileWatchRegistry {
 public:
  // Ingest one entry of `client/registerCapability`'s `registrations` array. The
  // caller has already checked that `method` is workspace/didChangeWatchedFiles.
  // `project_root` resolves RelativePattern base URIs. Returns true when at least
  // one usable watcher was stored.
  bool Register(const util::JsonValue& registration,
                const std::filesystem::path& project_root);

  // Drop the registration with this id. Returns true when one was removed.
  bool Unregister(std::string_view id);

  void Clear();

  bool empty() const { return registrations_.empty(); }
  std::size_t size() const { return registrations_.size(); }

  // True when some registered watcher wants this change. `relative_path` must be
  // forward-slash and project-root-relative; `absolute_path` is the same file's
  // absolute path (matched only against patterns that were registered absolute,
  // so the common `**/*.rs` case costs one match against the shorter string).
  bool WantsChange(std::string_view relative_path,
                   std::string_view absolute_path,
                   LspFileChangeType change) const;

  // Total pattern count across every registration; used by tests and by the
  // per-registration cap accounting.
  std::size_t PatternCountForTesting() const;

 private:
  struct Registration {
    std::string id;
    // Patterns matched against the project-relative path (the `**/*.ext` shape
    // every server actually registers).
    std::vector<std::string> relative_patterns;
    // Patterns matched against the absolute path — a RelativePattern whose base
    // lies outside the project, or a server that registered an absolute glob.
    std::vector<std::string> absolute_patterns;
    int kind = kLspWatchKindAll;
  };

  std::vector<Registration> registrations_;
};

// Bounds. A server is trusted-ish but not trusted to be sane: these keep a
// misbehaving or hostile server from turning every file change into an unbounded
// match loop on the shell thread. Exceeding a cap drops the excess watchers rather
// than the whole registration, so a server with one pathological glob still gets
// notified about its normal ones.
inline constexpr std::size_t kMaxLspFileWatchRegistrations = 64;
inline constexpr std::size_t kMaxLspFileWatchPatternsPerRegistration = 128;

}  // namespace microide::workspace
