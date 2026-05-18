// File I/O, save normalization, and encoding/line-ending labels for
// TextViewport. Split out of TextViewport.cpp to keep that translation unit
// focused on editing/layout/undo. These methods are still members of the
// `TextViewport` class — see editor/TextViewport.h for the declarations.

#include "editor/TextViewport.h"

#include <algorithm>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

namespace {

bool TrimTrailingWhitespaceInPlace(std::vector<std::string>& lines) {
  bool any = false;
  for (std::string& line : lines) {
    std::size_t end = line.size();
    while (end > 0) {
      char c = line[end - 1];
      if (c != ' ' && c != '\t') break;
      --end;
    }
    if (end != line.size()) {
      line.resize(end);
      any = true;
    }
  }
  return any;
}

bool EnsureSingleFinalNewlineInPlace(std::vector<std::string>& lines) {
  if (lines.empty()) {
    lines.emplace_back();
    return true;
  }
  bool changed = false;
  while (lines.size() > 1 && lines.back().empty() && lines[lines.size() - 2].empty()) {
    lines.pop_back();
    changed = true;
  }
  if (lines.empty() || !lines.back().empty()) {
    lines.emplace_back();
    changed = true;
  }
  return changed;
}

}  // namespace

bool TextViewport::OpenFile(const std::filesystem::path& path) {
  std::string perf_label = "TextViewport::OpenFile";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(path=" + path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  EnsureDocument();
  const std::optional<std::string> content = util::ReadTextFile(path);
  if (!content.has_value()) {
    return false;
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
  std::vector<std::string> normalized = document_->lines;
  bool changed = false;
  if (save_trim_trailing_whitespace_) {
    if (TrimTrailingWhitespaceInPlace(normalized)) changed = true;
  }
  if (save_ensure_final_newline_) {
    if (EnsureSingleFinalNewlineInPlace(normalized)) changed = true;
  }

  const std::string text = util::SerializeLines(
      changed ? normalized : document_->lines, document_->line_ending);
  if (!util::WriteTextFileAtomically(document_->path, text)) {
    return false;
  }

  if (changed) {
    // Mirror the normalization into the live buffer so the user sees the
    // same content they just saved. Routed through ReplaceLines so undo can
    // unwind the change.
    ReplaceLines(0, document_->lines.size(), normalized, /*record_undo=*/true);
  }

  document_->mixed_line_endings = false;
  document_->dirty = false;
  return true;
}

void TextViewport::LoadContent(std::string_view content,
                               const std::filesystem::path& path,
                               std::optional<LineEnding> line_ending) {
  EnsureDocument();
  const util::DecodedText decoded = util::DecodeLines(content);
  ResetState(decoded.lines, path, line_ending.value_or(decoded.line_ending),
             line_ending.has_value() ? false : decoded.mixed_line_endings, DetectEncoding(content),
             false, false);
}

void TextViewport::SetPath(const std::filesystem::path& path) {
  EnsureDocument();
  document_->path = path;
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

TextViewport::TextEncoding TextViewport::DetectEncoding(const std::vector<std::string>& lines) {
  bool ascii_only = true;
  for (const std::string& line : lines) {
    if (line.find('\0') != std::string::npos) {
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
