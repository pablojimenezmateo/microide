#pragma once

#include <filesystem>
#include <string_view>

#include "util/TextFileIO.h"

namespace microide::project {

// One door for every write that replaces a file inside a project tree.
//
// Six subsystems independently knew how to do this — the editor's save, the plugin
// file API, replace-in-project, the merge writer, the LSP resource ops and the
// sidebar's file operations — and they shared `WriteTextFileAtomically` and nothing
// above it: no common notion of what the write was computed against, no
// serialization between them, and no single place that knows a file under the
// project root just changed. Each of them learns about its own write by observing
// the filesystem afterwards, which is why each grew its own post-write fixup.
//
// The gate is also the seam a remote project needs: `MirrorWriteGate` is the
// implementation that writes into the mirror and enqueues the push, and the local
// one below is a pass-through (see dev-docs/design/remote-projects.md § 6.3).
class FileWriteGate {
 public:
  struct Result {
    bool ok = false;
    // The written file's identity, captured immediately after the write, by the
    // gate rather than by the caller. Two callers want it: the editor's save, so a
    // later conflict check compares against what it actually wrote; and anything
    // that must recognise the watcher's echo of its OWN write instead of treating
    // it as an external change. Returning it here is also one fewer stat — the
    // editor save used to write and then stat the same path again.
    util::FileSignature signature;
  };

  // Whether the caller wants the written file's signature back. Only the editor's
  // save does — it records it as the baseline for the next conflict check. The
  // others (replace-in-project across N files, the plugin file API, the merge
  // rollback) read only `ok`, and statting the file for them turned the editor
  // save's "one fewer stat" into one extra stat per written file everywhere else.
  enum class Signature { Skip, Capture };

  virtual ~FileWriteGate() = default;

  // Replace `path`'s contents with `text`. Atomic: a failed write leaves the
  // original intact.
  [[nodiscard]] virtual Result WriteText(const std::filesystem::path& path,
                           std::string_view text,
                           Signature signature = Signature::Skip) = 0;
};

// The local gate: an atomic temp-file + rename, then one stat. Every write in a
// local project goes through this one.
FileWriteGate& LocalFileWriteGate();

}  // namespace microide::project
