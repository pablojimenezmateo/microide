// File I/O, save normalization, and encoding/line-ending labels for
// TextViewport. Split out of TextViewport.cpp to keep that translation unit
// focused on editing/layout/undo. These methods are still members of the
// `TextViewport` class — see editor/TextViewport.h for the declarations.

#include "editor/TextViewport.h"

#include <algorithm>
#include <vector>

#include "editor/SaveNormalization.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

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

  // Fast path: a file with no '\r' is already in the document's canonical
  // representation (lines joined by '\n'), so hand the bytes straight to the
  // buffer instead of splitting into a vector<string> and rejoining. This skips
  // two full copies of the file plus one heap allocation per line on the open
  // path -- the dominant cost for large files.
  if (content->find('\r') == std::string::npos) {
    const TextEncoding encoding = DetectEncoding(*content);
    ResetStateFromText(std::move(*content), path, LineEnding::LF, false, encoding, false, false);
    return true;
  }

  const util::DecodedText decoded = util::DecodeLines(*content);
  ResetState(decoded.lines, path, decoded.line_ending, decoded.mixed_line_endings,
             DetectEncoding(*content), false, false);
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
  std::vector<std::string> normalized = document_->lines.ToVector();
  bool changed = false;
  if (save_trim_trailing_whitespace_) {
    if (TrimTrailingWhitespace(normalized)) changed = true;
  }
  if (save_ensure_final_newline_) {
    if (EnsureSingleFinalNewline(normalized)) changed = true;
  }

  // "auto" keeps the file's detected ending; an explicit lf/crlf override wins.
  const LineEnding effective_ending = save_line_ending_override_.value_or(document_->line_ending);
  const std::string text = util::SerializeLines(
      changed ? normalized : document_->lines.Snapshot(), effective_ending);
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
    ReplaceLines(0, document_->lines.size(), normalized, /*record_undo=*/true);
  }

  document_->mixed_line_endings = false;
  document_->dirty = false;
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
  // Fast path mirrors OpenFile: when the caller does not force a line ending and
  // the content is already '\n'-canonical, skip the split/rejoin round-trip.
  if (!line_ending.has_value() && content.find('\r') == std::string_view::npos) {
    const TextEncoding encoding = DetectEncoding(content);
    ResetStateFromText(std::string(content), path, LineEnding::LF, false, encoding, false, false);
    return;
  }
  const util::DecodedText decoded = util::DecodeLines(content);
  ResetState(decoded.lines, path, line_ending.value_or(decoded.line_ending),
             line_ending.has_value() ? false : decoded.mixed_line_endings, DetectEncoding(content),
             false, false);
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
      return "ASCII";
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
