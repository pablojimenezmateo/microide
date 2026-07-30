// File I/O, save normalization, and encoding/line-ending labels for
// TextViewport. Split out of TextViewport.cpp to keep that translation unit
// focused on editing/layout/undo. These methods are still members of the
// `TextViewport` class — see editor/TextViewport.h for the declarations.

#include "editor/TextViewport.h"

#include <algorithm>
#include <vector>

#include "editor/SaveNormalization.h"
#include "editor/TextLayout.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {
namespace {

struct LineEndingMetadata {
  util::LineEnding line_ending = util::LineEnding::LF;
  bool mixed_line_endings = false;
  bool has_cr = false;
};

LineEndingMetadata AnalyzeLineEndings(std::string_view content) {
  std::size_t crlf = 0;
  std::size_t lf = 0;
  std::size_t cr = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r') {
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++crlf;
        ++i;
      } else {
        ++cr;
      }
    } else if (content[i] == '\n') {
      ++lf;
    }
  }

  LineEndingMetadata metadata;
  metadata.has_cr = crlf > 0 || cr > 0;
  const std::size_t present_styles =
      (crlf > 0 ? 1 : 0) + (lf > 0 ? 1 : 0) + (cr > 0 ? 1 : 0);
  metadata.mixed_line_endings = present_styles > 1;
  if (crlf >= lf && crlf >= cr && crlf > 0) {
    metadata.line_ending = util::LineEnding::CRLF;
  } else if (lf >= cr && lf > 0) {
    metadata.line_ending = util::LineEnding::LF;
  } else if (cr > 0) {
    metadata.line_ending = util::LineEnding::CR;
  }
  return metadata;
}

std::string CanonicalizeLineEndingsToLf(std::string_view content,
                                        const LineEndingMetadata& metadata) {
  if (!metadata.has_cr) {
    return std::string(content);
  }
  std::string normalized;
  normalized.reserve(content.size());
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r') {
      normalized.push_back('\n');
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++i;
      }
    } else {
      normalized.push_back(content[i]);
    }
  }
  return normalized;
}

std::string CanonicalizeLineEndingsToLf(std::string&& content,
                                        const LineEndingMetadata& metadata) {
  if (!metadata.has_cr) {
    return std::move(content);
  }
  return CanonicalizeLineEndingsToLf(std::string_view(content), metadata);
}

// Split on '\n' only, preserving every other byte -- including a stray '\r' or a NUL --
// verbatim inside the line. Used for opaque/binary ("Bytes"-encoded) content where a CR is
// payload, not a line terminator: the split is exactly reversible by joining the lines back
// with '\n', so an unedited open->save round-trips the bytes. (SplitLines/DecodeLines cannot
// be used here: they treat CR as a line ending and would drop it.)
std::vector<std::string> SplitOnLineFeedOnly(std::string_view content) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\n') {
      lines.emplace_back(content.substr(start, i - start));
      start = i + 1;
    }
  }
  lines.emplace_back(content.substr(start));
  return lines;
}

}  // namespace

bool TextViewport::OpenFile(const std::filesystem::path& path) {
  std::string perf_label = "TextViewport::OpenFile";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(path=" + path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  EnsureDocument();
  std::optional<std::string> content = util::ReadTextFile(path);
  if (!content.has_value()) {
    return false;
  }

  // Convert directly to the editor's canonical LF buffer. The old CRLF/CR path
  // decoded into vector<string> and PieceTree immediately joined it back into a
  // string, so a dense CRLF file could force one allocation per line on open.
  const LineEndingMetadata metadata = AnalyzeLineEndings(*content);
  const TextEncoding encoding = DetectEncoding(*content);
  if (encoding == TextEncoding::Bytes) {
    // Opaque/binary content: a 0x0D or 0x0A is data, not a line ending. Split on '\n'
    // only (keeping CR bytes in the line) and label the ending LF so Save joins with a
    // single '\n' -- the only transform is the reversible split, so the bytes survive.
    ResetState(SplitOnLineFeedOnly(*content), path, LineEnding::LF,
               /*mixed_line_endings=*/false, encoding, /*placeholder=*/false, /*dirty=*/false);
    return true;
  }
  ResetStateFromText(CanonicalizeLineEndingsToLf(std::move(*content), metadata), path,
                     metadata.line_ending, metadata.mixed_line_endings, encoding, false, false);
  return true;
}

bool TextViewport::Save() {
  EnsureDocument();
  if (document_->path.empty()) {
    return false;
  }

  // Save-time normalization: apply trim and final-newline transforms to a
  // local copy of the lines buffer. We write the normalized text but keep the
  // in-memory buffer untouched unless the toggles change the content; this
  // keeps subsequent edits idempotent and avoids the noise of a re-layout
  // immediately after save.
  // Opaque/binary content is round-tripped verbatim: skip whitespace/newline normalization
  // and force an LF join (mirroring the '\n'-only split on open) so a stray CR or NUL is
  // never rewritten. Applying trim or an ending override to binary bytes would corrupt them.
  const bool opaque = document_->encoding == TextEncoding::Bytes;
  // Only materialize a whole-document vector when a save-time transform is actually
  // enabled. A clean save with transforms off (or an opaque/binary buffer) skips the
  // O(line count) ToVector pass entirely and serializes straight from the snapshot —
  // the transform check is cheap and the common case is no transform. (TD-2026-07-16-12.)
  const bool wants_transform =
      !opaque && (save_trim_trailing_whitespace_ || save_ensure_final_newline_);
  std::vector<std::string> normalized;
  bool changed = false;
  if (wants_transform) {
    normalized = document_->lines.ToVector();
    if (save_trim_trailing_whitespace_ && TrimTrailingWhitespace(normalized)) {
      changed = true;
    }
    if (save_ensure_final_newline_ && EnsureSingleFinalNewline(normalized)) {
      changed = true;
    }
  }

  // "auto" keeps the file's detected ending; an explicit lf/crlf override wins. Opaque
  // content ignores both and always joins with LF.
  const LineEnding effective_ending =
      opaque ? LineEnding::LF : save_line_ending_override_.value_or(document_->line_ending);
  // The changed path already holds a materialized `normalized` vector. The common
  // clean-save path streams straight from the live TextBuffer (zero-copy via LineView)
  // instead of materializing a whole-document vector with Snapshot() before the join
  // (TD-2026-07-17A-013).
  const std::string text =
      changed ? util::SerializeLines(normalized, effective_ending)
              : util::SerializeLinesStreaming(LineSpan(document_->lines), effective_ending);
  if (!util::WriteTextFileAtomically(document_->path, text)) {
    return false;
  }

  // Commit the (possibly overridden) ending only after the write succeeds, so a
  // failed save leaves the in-memory ending — and the status-bar label — matching
  // what is still on disk rather than the ending we tried and failed to write.
  document_->line_ending = effective_ending;

  if (changed) {
    // Mirror the normalization into the live buffer so the user sees the
    // same content they just saved. Routed through ReplaceLines so undo can
    // unwind the change.
    //
    // ReplaceLines over the whole document snaps the caret/selection/scroll to
    // (0,0)/top. VS Code preserves the caret across format-on-save, so capture the
    // pre-normalization view and restore it afterwards, then clamp every caret and
    // anchor back into the (possibly trimmed) content.
    const ViewState pre_normalize_view = CaptureViewState();
    ReplaceLines(0, document_->lines.size(), normalized, /*record_undo=*/true);
    RestoreViewState(pre_normalize_view);
    const auto clamp_position = [this](TextPosition& position) {
      if (document_->lines.empty()) {
        position.line = 0;
        position.column = 0;
        return;
      }
      position.line = std::min(position.line, document_->lines.size() - 1);
      position.column = TextLayout::ClampTextColumn(document_->lines[position.line], position.column);
    };
    TextPosition primary{cursor_line_, cursor_column_};
    clamp_position(primary);
    cursor_line_ = primary.line;
    cursor_column_ = primary.column;
    if (selection_anchor_.has_value()) {
      clamp_position(*selection_anchor_);
    }
    for (SecondaryCaret& caret : secondary_carets_) {
      clamp_position(caret.position);
      if (caret.selection_anchor.has_value()) {
        clamp_position(*caret.selection_anchor);
      }
    }
    ClampScrollState();
  }

  document_->mixed_line_endings = false;
  document_->dirty = false;
  // Re-baseline undo/redo dirty flags to this saved position so undoing past the
  // save marks the buffer dirty (its content then differs from what we just wrote).
  undo_history_.MarkSaved();
  // Record the just-written file's identity so (a) a save-time conflict check
  // sees a matching signature next time and (b) the watcher's echo of our own
  // write is recognized and suppressed instead of forcing a redundant reload.
  document_->disk_signature = util::StatFileSignature(document_->path);
  return true;
}

TextViewport::DiskConflict TextViewport::DetectDiskConflict() const {
  // No baseline (untitled/new buffer, or never sampled): never block a save.
  if (!document_->disk_signature.exists || document_->path.empty()) {
    return DiskConflict::None;
  }
  const util::FileSignature current = util::StatFileSignature(document_->path);
  if (current.error) {
    return DiskConflict::StatError;
  }
  if (!current.exists) {
    return DiskConflict::Vanished;
  }
  return current.SameContentAs(document_->disk_signature) ? DiskConflict::None
                                                          : DiskConflict::Changed;
}

void TextViewport::LoadContent(std::string_view content,
                               const std::filesystem::path& path,
                               std::optional<LineEnding> line_ending) {
  EnsureDocument();
  const LineEndingMetadata metadata = AnalyzeLineEndings(content);
  const TextEncoding encoding = DetectEncoding(content);
  if (encoding == TextEncoding::Bytes) {
    // Same opaque-bytes handling as OpenFile: preserve CR bytes and force an LF ending so a
    // restored binary buffer saves back byte-for-byte.
    ResetState(SplitOnLineFeedOnly(content), path, line_ending.value_or(LineEnding::LF),
               /*mixed_line_endings=*/false, encoding, /*placeholder=*/false, /*dirty=*/false);
    return;
  }
  ResetStateFromText(CanonicalizeLineEndingsToLf(content, metadata), path,
                     line_ending.value_or(metadata.line_ending),
                     line_ending.has_value() ? false : metadata.mixed_line_endings,
                     encoding, false, false);
}

void TextViewport::LoadLines(std::vector<std::string> lines, const std::filesystem::path& path,
                            LineEnding line_ending) {
  EnsureDocument();
  // ResetState moves `lines` into the piece tree (no join/re-split). The encoding is
  // recomputed from the loaded buffer afterwards, mirroring LoadContent's detection.
  ResetState(std::move(lines), path, line_ending, /*mixed_line_endings=*/false,
             TextEncoding::ASCII, /*placeholder=*/false, /*dirty=*/true);
  RefreshEncoding();
}

void TextViewport::SetPath(const std::filesystem::path& path) {
  EnsureDocument();
  SetDocumentPath(path);
}

void TextViewport::SetDirty(bool dirty) {
  EnsureDocument();
  document_->dirty = dirty;
  if (dirty) {
    document_->placeholder = false;
  }
}

std::string TextViewport::LineEndingLabel() const {
  const std::string base = util::LineEndingLabel(document_->line_ending);
  const std::string upper =
      base == "crlf" ? "CRLF" : base == "cr" ? "CR" : "LF";
  return document_->mixed_line_endings ? "mixed:" + upper : upper;
}

std::string TextViewport::EncodingLabel() const {
  switch (document_->encoding) {
    case TextEncoding::ASCII:
      // ASCII is UTF-8, and the file is written as UTF-8 either way, so reporting
      // "ASCII" told the user nothing they could act on and made the status bar
      // appear to change the file's encoding the moment they typed an accent.
      // VSCode says UTF-8 for both; the enum keeps the two apart only because
      // ascii_only is the detection fast path that skips the UTF-8 validation.
    case TextEncoding::UTF8:
      return "UTF-8";
    case TextEncoding::Bytes:
    default:
      return "Bytes";
  }
}

void TextViewport::RefreshEncoding() {
  util::AddPerformanceCounter(util::PerfCounterId::EditorRefreshEncodingCalls);
  document_->encoding = DetectEncoding(document_->lines);
}

void TextViewport::UpgradeEncodingForInsertedLines(
    const std::vector<std::string>& inserted_lines) {
  util::AddPerformanceCounter(util::PerfCounterId::EditorRefreshEncodingCalls);
  if (document_->encoding == TextEncoding::Bytes) {
    return;  // already the worst classification; an insert cannot raise it further.
  }
  const TextEncoding delta = DetectEncoding(LineSpan(inserted_lines));
  if (static_cast<int>(delta) > static_cast<int>(document_->encoding)) {
    document_->encoding = delta;
  }
}

TextViewport::TextEncoding TextViewport::DetectEncoding(std::string_view content) {
  if (content.find('\0') != std::string_view::npos) {
    return TextEncoding::Bytes;
  }

  const bool ascii_only = std::all_of(content.begin(), content.end(), [](char character) {
    return static_cast<unsigned char>(character) < 0x80;
  });
  if (ascii_only) {
    return TextEncoding::ASCII;
  }

  return util::IsValidUtf8(content) ? TextEncoding::UTF8 : TextEncoding::Bytes;
}

TextViewport::TextEncoding TextViewport::DetectEncoding(LineSpan lines) {
  bool ascii_only = true;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.find('\0') != std::string_view::npos) {
      return TextEncoding::Bytes;
    }

    for (char character : line) {
      if (static_cast<unsigned char>(character) >= 0x80) {
        ascii_only = false;
        break;
      }
    }

    if (!ascii_only && !util::IsValidUtf8(line)) {
      return TextEncoding::Bytes;
    }
  }

  return ascii_only ? TextEncoding::ASCII : TextEncoding::UTF8;
}

}  // namespace microide::editor
