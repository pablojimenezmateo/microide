#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace microide::editor {
class DiagnosticsStore;
struct FileDecorations;
}  // namespace microide::editor

namespace microide::workspace {

// Everything the compare render surface reads out of project state, injected as
// one parameter by the frame caller.
//
// The compare render translation unit may not touch `context_.current_project_state`
// (`CheckCompareRenderStructuralGate`), so each of these arrived as its own
// argument and the list grew one parameter at a time. Bundling them keeps the
// shell's declaration budget — a hard invariant — from being the thing that
// decides whether the diff surface gets a feature.
struct CompareRenderProjectInputs {
  const std::filesystem::path* project_root = nullptr;
  const editor::DiagnosticsStore* diagnostics_store = nullptr;
  // Plugin decorations for the EDITABLE right pane only. The left pane is a
  // different revision of the file, so a decoration published against the
  // working-tree line numbers would land on the wrong line there.
  const editor::FileDecorations* right_plugin_decorations = nullptr;
};


// One side of a non-git ("plain") comparison. The caller resolves `content`
// (file read, buffer serialize, or clipboard read); `path` is the real on-disk
// file backing this side, empty for a clipboard or untitled-buffer side;
// `editable` requests that — when this is the right (primary) side and it is a
// real file — the compare's right pane save back to `path`.
struct CompareInput {
  std::string content;
  std::string label;
  std::filesystem::path path;
  bool editable = false;
};

// Reads `path` into a CompareInput for a plain comparison. Returns nullopt when
// the file is unreadable, binary, or too large (a missing file yields empty
// content — a legitimate "deleted" side). The label is the filename.
std::optional<CompareInput> ReadFileCompareInput(const std::filesystem::path& path, bool editable);

}  // namespace microide::workspace
