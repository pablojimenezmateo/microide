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
// A diff surface's prepared paint inputs (render/PreparedFrameState.h).
struct DiffSurfacePaint;

struct CompareRenderProjectInputs {
  // The surface's paint inputs frame prep built: settings, drags, the pointer, the
  // blame, and the working side's diagnostics and decorations. Nothing here reaches
  // a store: what the surface draws is what its row units compared
  // (CheckDerivedSurfacePaintReadsOnlyItsSnapshot).
  const DiffSurfacePaint* paint = nullptr;
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
