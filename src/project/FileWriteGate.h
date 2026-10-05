#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/TextFileIO.h"

namespace microide::project {

// One door for every write into a project tree: replacing a file's contents, and
// changing the tree itself (create, rename, delete, trash).
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

  // ---- Tree operations (TD-2026-09-29-305) ------------------------------------
  //
  // A change to the TREE rather than to one file's bytes. The sidebar's file
  // operations and the LSP's workspace-edit resource ops each used to implement
  // these on their own — the first with an exclusive create and an atomic
  // no-overwrite rename, the second with a truncating ofstream and an overwriting
  // rename made safe only by staging the victim aside first — and a remote
  // project needs every one of them to reach the host as an `fs/op`, which only
  // works if there is one door. Policy (is this path inside the project? does an
  // ignore-if-exists flag turn a clash into a no-op?) stays with the caller; the
  // gate applies, and undoes.
  struct TreeOp {
    enum class Kind : std::uint8_t {
      CreateFile,       // an empty file; missing parent directories are created
      CreateDirectory,  // with any missing parents
      Rename,           // `path` -> `new_path`; missing parents of `new_path` are created
      Delete,           // see `recursive`
      Trash,            // to the desktop trash: the user can restore it; a rollback
                        // moves it back
    };
    Kind kind = Kind::CreateFile;
    std::filesystem::path path;
    std::filesystem::path new_path;
    // CreateFile / Rename: replace an existing target. Off, an existing target
    // fails the op, ATOMICALLY — an O_EXCL create and a RENAME_NOREPLACE move — so
    // a target appearing between a check and the act still cannot be clobbered.
    bool overwrite = false;
    // Delete: a directory with contents only when set. Unset, a directory is
    // removed with rmdir, which refuses a non-empty one in the kernel rather than
    // after a check something else can race.
    bool recursive = false;
  };

  struct TreeResult {
    bool ok = false;
    // Index of the op that failed; meaningful only when !ok.
    std::size_t failed_index = 0;
    // A sentence for the user, naming what went wrong.
    std::string error_message;
    // Where the last applied op left its path: the created path, the rename's
    // destination, the trash location.
    std::filesystem::path resulting_path;
    // Directories a delete or an overwrite staged aside, for the caller to remove
    // OFF the shell thread (a deep tree is arbitrarily slow to unlink). Staged
    // FILES are already gone — one unlink each — so nothing hidden can be walked
    // into the file index before the caller's refresh.
    std::vector<std::filesystem::path> staged_directories;
  };

  // Apply `ops` in order, ALL OR NOTHING: each op records its inverse, and the
  // first failure undoes every earlier op in reverse before returning. A deleted
  // or overwritten path is renamed aside (same directory, so a pure rename) until
  // the whole batch lands, which is what makes a delete undoable.
  [[nodiscard]] virtual TreeResult ApplyTreeOps(std::span<const TreeOp> ops) = 0;

  // Remove what a landed batch handed back in `staged_directories`. Thread-safe:
  // callers run it OFF the shell thread, because a staged tree can be arbitrarily
  // deep. Failure is harmless — a hidden staging entry left behind, never user
  // data — so it reports nothing.
  virtual void DisposeStaged(std::span<const std::filesystem::path> staged) = 0;

  // One op — the sidebar's shape.
  [[nodiscard]] TreeResult ApplyTreeOp(const TreeOp& op) {
    return ApplyTreeOps(std::span<const TreeOp>(&op, 1));
  }
};

// The local gate: an atomic temp-file + rename, then one stat. Every write in a
// local project goes through this one.
FileWriteGate& LocalFileWriteGate();

}  // namespace microide::project
