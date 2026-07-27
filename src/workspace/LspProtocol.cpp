#include "workspace/LspProtocol.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/ProtocolNumeric.h"

namespace microide::workspace::lsp_protocol {

using util::JsonObject;
using util::JsonValue;
using protocol_numeric::JsonIntInRange;

LspClient::Position ParsePosition(const JsonValue& value) {
  LspClient::Position position;
  // Clamp out-of-int-range positions deterministically rather than relying on the
  // implementation-defined narrowing of an out-of-range int64 (a hostile server
  // could otherwise wrap a huge line/character into a small/negative coordinate).
  position.line = JsonIntInRange(value["line"]);
  position.character = JsonIntInRange(value["character"]);
  return position;
}

LspClient::Range ParseRange(const JsonValue& value) {
  LspClient::Range range;
  if (!value.HasKey("start") || !value.HasKey("end")) {
    return range;
  }
  range.start = ParsePosition(value["start"]);
  range.end = ParsePosition(value["end"]);
  return range;
}

LspClient::Location ParseLocation(const JsonValue& value) {
  LspClient::Location location;
  // LocationLink uses targetUri/targetRange; Location uses uri/range.
  if (value.HasKey("targetUri")) {
    location.uri = value["targetUri"].AsString();
    location.range = ParseRange(value.HasKey("targetSelectionRange")
                                    ? value["targetSelectionRange"]
                                    : value["targetRange"]);
    return location;
  }
  location.uri = value["uri"].AsString();
  location.range = ParseRange(value["range"]);
  return location;
}

// Cap the number of entries materialized from a server array. LSP messages are
// bounded to 64 MiB (kMaxLspMessageBytes), but a minimal location/diagnostic/
// symbol object is only a few dozen bytes of JSON, so a hostile/buggy server can
// still pack ~1M entries into one message — each materialized into strings +
// ranges and built into a picker/outline on the UI thread. These caps sit far
// above any usable list; past them the extra entries are dropped, not parsed.
constexpr std::size_t kMaxLspLocations = 50000;
constexpr std::size_t kMaxLspDiagnostics = 10000;  // matches DiagnosticsStore cap
constexpr std::size_t kMaxLspSymbolNodes = 100000;  // total across the symbol tree

std::vector<LspClient::Location> ParseLocations(const JsonValue& result) {
  std::vector<LspClient::Location> locations;
  if (result.IsArray()) {
    const auto& array = result.AsArray();
    const std::size_t count = std::min(array.size(), kMaxLspLocations);
    locations.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      locations.push_back(ParseLocation(array[i]));
    }
  } else if (result.HasKey("uri") || result.HasKey("targetUri")) {
    locations.push_back(ParseLocation(result));
  }
  return locations;
}

LspClient::Diagnostic ParseDiagnostic(const JsonValue& value) {
  LspClient::Diagnostic diagnostic;
  diagnostic.range = ParseRange(value["range"]);
  diagnostic.message = value["message"].AsString();
  // Clamp to the LSP severity domain (1=Error … 4=Hint). Out-of-spec values
  // (0, 99, INT_MAX) must not slip through to severity filters or "most severe"
  // status logic where they would misclassify; anything invalid defaults to Error.
  {
    const int raw = JsonIntInRange(value["severity"], 1);
    diagnostic.severity = (raw >= 1 && raw <= 4) ? raw : 1;
  }
  // LSP `Diagnostic.code` is `integer | string`. Many servers (TypeScript's 2304,
  // and most compiler-backed servers) report a numeric code; AsString() yields ""
  // for those, silently dropping the code from the UI. Capture the int form too.
  const JsonValue& code = value["code"];
  diagnostic.code = code.IsInt() ? std::to_string(code.AsInt()) : code.AsString();
  return diagnostic;
}

std::vector<LspClient::Diagnostic> ParseDiagnostics(const JsonValue& array) {
  std::vector<LspClient::Diagnostic> diagnostics;
  if (!array.IsArray()) {
    return diagnostics;
  }
  const auto& items = array.AsArray();
  const std::size_t count = std::min(items.size(), kMaxLspDiagnostics);
  diagnostics.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    diagnostics.push_back(ParseDiagnostic(items[i]));
  }
  return diagnostics;
}

// Bound recursion over the server-supplied `children` tree: a hostile/buggy
// server could otherwise nest it deeply enough to overflow the stack. Past the
// cap, keep the symbol but drop its deeper descendants (an outline that deep is
// unusable anyway).
constexpr int kMaxDocumentSymbolDepth = 64;

// `remaining` is a shared total-node budget: a server can stay under the depth
// cap yet pack millions of siblings/children into the tree, so bound the total
// count of materialized nodes across the whole recursion, not just the depth.
static LspClient::DocumentSymbol ParseDocumentSymbolAtDepth(const JsonValue& value, int depth,
                                                            std::size_t& remaining) {
  LspClient::DocumentSymbol symbol;
  symbol.name = value["name"].AsString();
  symbol.detail = value["detail"].AsString();
  symbol.kind = JsonIntInRange(value["kind"], 1);
  if (value.HasKey("location")) {
    // SymbolInformation shape.
    symbol.range = ParseRange(value["location"]["range"]);
    symbol.selection_range = symbol.range;
  } else {
    symbol.range = ParseRange(value["range"]);
    symbol.selection_range =
        value.HasKey("selectionRange") ? ParseRange(value["selectionRange"]) : symbol.range;
  }
  if (depth >= kMaxDocumentSymbolDepth) {
    return symbol;
  }
  for (const auto& child : value["children"].AsArray()) {
    if (remaining == 0) {
      break;
    }
    --remaining;
    symbol.children.push_back(ParseDocumentSymbolAtDepth(child, depth + 1, remaining));
  }
  return symbol;
}

LspClient::DocumentSymbol ParseDocumentSymbol(const JsonValue& value) {
  std::size_t remaining = kMaxLspSymbolNodes;
  return ParseDocumentSymbolAtDepth(value, 0, remaining);
}

std::vector<LspClient::WorkspaceSymbol> ParseWorkspaceSymbols(const JsonValue& result) {
  std::vector<LspClient::WorkspaceSymbol> symbols;
  if (!result.IsArray()) {
    return symbols;
  }
  const auto& items = result.AsArray();
  // Reuse the location cap: each symbol materializes strings + a location, and the
  // host builds a navigable list from them on the UI thread.
  const std::size_t count = std::min(items.size(), kMaxLspLocations);
  symbols.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const JsonValue& item = items[i];
    LspClient::WorkspaceSymbol symbol;
    symbol.name = item["name"].AsString();
    symbol.container_name = item["containerName"].AsString();
    symbol.kind = JsonIntInRange(item["kind"], 1);
    symbol.location = ParseLocation(item["location"]);
    symbols.push_back(std::move(symbol));
  }
  return symbols;
}

std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const JsonValue& result) {
  std::vector<LspClient::DocumentSymbol> symbols;
  if (!result.IsArray()) {
    return symbols;
  }
  const auto& items = result.AsArray();
  std::size_t remaining = kMaxLspSymbolNodes;
  const std::size_t count = std::min(items.size(), remaining);
  symbols.reserve(count);
  for (std::size_t i = 0; i < count && remaining > 0; ++i) {
    --remaining;
    symbols.push_back(ParseDocumentSymbolAtDepth(items[i], 0, remaining));
  }
  return symbols;
}

std::vector<LspClient::TextEdit> ParseTextEdits(const JsonValue& result, std::size_t max_edits) {
  std::vector<LspClient::TextEdit> edits;
  if (!result.IsArray()) {
    return edits;
  }
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_edits);
  edits.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    edits.emplace_back(ParseRange(array[i]["range"]), array[i]["newText"].AsString());
  }
  return edits;
}

LspClient::WorkspaceEdit ParseWorkspaceEdit(const JsonValue& edit, std::size_t max_files,
                                            std::size_t max_edits_total) {
  LspClient::WorkspaceEdit out;
  std::size_t total_edits = 0;
  const auto append_edits = [&](const std::string& uri, const JsonValue& edits_array) {
    if (!edits_array.IsArray()) {
      return;
    }
    if (out.changes.size() >= max_files && out.changes.find(uri) == out.changes.end()) {
      return;
    }
    auto& bucket = out.changes[uri];
    for (const auto& text_edit : edits_array.AsArray()) {
      if (total_edits >= max_edits_total) {
        break;
      }
      bucket.emplace_back(ParseRange(text_edit["range"]), text_edit["newText"].AsString());
      ++total_edits;
    }
  };
  if (edit.HasKey("changes")) {
    for (const auto& [uri, edits_val] : edit["changes"].AsObject()) {
      if (total_edits >= max_edits_total) {
        break;
      }
      append_edits(uri, edits_val);
    }
  }
  if (edit.HasKey("documentChanges") && edit["documentChanges"].IsArray()) {
    for (const auto& doc_change : edit["documentChanges"].AsArray()) {
      // Resource op shape: { kind: "create"|"rename"|"delete", uri/oldUri/newUri,
      // options? }. Kept in array order; the op count shares the file cap so a
      // hostile server cannot force an unbounded op list.
      if (doc_change.HasKey("kind") && doc_change["kind"].IsString()) {
        if (out.resource_ops.size() >= max_files) {
          continue;
        }
        const std::string& kind = doc_change["kind"].AsString();
        LspClient::WorkspaceEdit::ResourceOp op;
        const JsonValue& options = doc_change["options"];
        if (kind == "create") {
          op.kind = LspClient::WorkspaceEdit::ResourceOp::Kind::Create;
          op.uri = doc_change["uri"].AsString();
        } else if (kind == "rename") {
          op.kind = LspClient::WorkspaceEdit::ResourceOp::Kind::Rename;
          op.uri = doc_change["oldUri"].AsString();
          op.new_uri = doc_change["newUri"].AsString();
        } else if (kind == "delete") {
          op.kind = LspClient::WorkspaceEdit::ResourceOp::Kind::Delete;
          op.uri = doc_change["uri"].AsString();
        } else {
          continue;  // unknown kind: ignore rather than misapply
        }
        op.overwrite = options["overwrite"].AsBool(false);
        op.ignore_if_exists = options["ignoreIfExists"].AsBool(false);
        op.ignore_if_not_exists = options["ignoreIfNotExists"].AsBool(false);
        op.recursive = options["recursive"].AsBool(false);
        if (op.uri.empty() ||
            (op.kind == LspClient::WorkspaceEdit::ResourceOp::Kind::Rename &&
             op.new_uri.empty())) {
          continue;  // malformed op: no usable target
        }
        // The host applies all resource ops before any text edit, so text edits
        // already accumulated under the pre-rename URI must follow the file to
        // its new name (the wire order applied them before the rename ran).
        if (op.kind == LspClient::WorkspaceEdit::ResourceOp::Kind::Rename &&
            op.uri != op.new_uri) {
          const auto old_bucket = out.changes.find(op.uri);
          if (old_bucket != out.changes.end()) {
            auto& new_bucket = out.changes[op.new_uri];
            new_bucket.insert(new_bucket.end(),
                              std::make_move_iterator(old_bucket->second.begin()),
                              std::make_move_iterator(old_bucket->second.end()));
            out.changes.erase(op.uri);
          }
        }
        out.resource_ops.push_back(std::move(op));
        continue;
      }
      if (total_edits >= max_edits_total) {
        continue;  // text-edit budget spent; still collect later resource ops
      }
      // TextDocumentEdit shape: { textDocument: { uri, version? }, edits: [...] }.
      if (!doc_change.HasKey("edits") || !doc_change["edits"].IsArray()) {
        continue;
      }
      const JsonValue& text_document = doc_change["textDocument"];
      const std::string& uri = text_document["uri"].AsString();
      // OptionalVersionedTextDocumentIdentifier: a non-null integer version pins
      // the edit to that document snapshot (accept a float-echoed integer, matching
      // the diagnostics version gate). Keyed by the URI as sent — never remapped.
      // Bounded by the file cap: empty-edit entries bypass the edit budget, so a
      // hostile server could otherwise materialize an unbounded version map.
      const JsonValue& version = text_document["version"];
      if ((version.IsInt() || version.IsDouble()) &&
          (out.expected_versions.size() < max_files ||
           out.expected_versions.find(uri) != out.expected_versions.end())) {
        out.expected_versions[uri] = version.AsInt();
      }
      append_edits(uri, doc_change["edits"]);
    }
  }
  return out;
}

LspClient::PrepareRename ParsePrepareRename(const JsonValue& result) {
  LspClient::PrepareRename out;
  if (result.IsNull()) {
    return out;  // server says this position is not renameable
  }
  if (result.HasKey("defaultBehavior")) {
    out.can_rename = result["defaultBehavior"].AsBool(false);
    return out;
  }
  if (result.HasKey("range")) {
    // { range: Range, placeholder: string }
    out.can_rename = true;
    out.range = ParseRange(result["range"]);
    out.placeholder = result["placeholder"].AsString();
    return out;
  }
  if (result.HasKey("start") && result.HasKey("end")) {
    // A bare Range (no placeholder; the caller keeps its heuristic seed).
    out.can_rename = true;
    out.range = ParseRange(result);
    return out;
  }
  return out;
}

namespace {
// A field carried as either a bare string or an object with a `value` string:
// covers both `documentation` (MarkupContent {kind, value}) and a MarkedString
// hover element ({language, value}).
std::string StringOrValueField(const JsonValue& value) {
  if (value.IsString()) {
    return value.AsString();
  }
  if (value.HasKey("value")) {
    return value["value"].AsString();
  }
  return {};
}

// Truncate `text` to at most `max_bytes` without splitting a UTF-8 code point.
// Thin alias over the shared primitive so the hover/label harvest reads locally.
inline void TruncateUtf8InPlace(std::string& text, std::size_t max_bytes) {
  util::TruncateUtf8ToByteBudget(text, max_bytes);
}
}  // namespace

LspClient::SignatureHelp ParseSignatureHelp(const JsonValue& result) {
  LspClient::SignatureHelp help;
  if (!result.HasKey("signatures") || !result["signatures"].IsArray()) {
    return help;
  }
  help.active_signature = JsonIntInRange(result["activeSignature"], 0);
  help.active_parameter = JsonIntInRange(result["activeParameter"], 0);
  // Bound the materialized signature/parameter counts (hostile-server backstop,
  // mirrors the other request caps). A usable overload menu never approaches these.
  constexpr std::size_t kMaxSignatures = 64;
  constexpr std::size_t kMaxParameters = 512;
  const auto& signatures = result["signatures"].AsArray();
  const std::size_t signature_count = std::min(signatures.size(), kMaxSignatures);
  help.signatures.reserve(signature_count);
  for (std::size_t i = 0; i < signature_count; ++i) {
    const JsonValue& signature = signatures[i];
    LspClient::SignatureInformation info;
    info.label = signature["label"].AsString();
    info.documentation = StringOrValueField(signature["documentation"]);
    info.active_parameter =
        signature.HasKey("activeParameter") ? JsonIntInRange(signature["activeParameter"], -1) : -1;
    if (signature.HasKey("parameters") && signature["parameters"].IsArray()) {
      const auto& parameters = signature["parameters"].AsArray();
      const std::size_t parameter_count = std::min(parameters.size(), kMaxParameters);
      info.parameters.reserve(parameter_count);
      for (std::size_t p = 0; p < parameter_count; ++p) {
        const JsonValue& parameter = parameters[p];
        LspClient::SignatureParameter out_parameter;
        const JsonValue& label = parameter["label"];
        if (label.IsArray()) {
          // `[start, end]` are UTF-16 code-unit offsets into the signature label
          // (LSP §SignatureInformation). Convert them to UTF-8 byte offsets before
          // slicing — treating them as bytes corrupted the active-parameter
          // highlight for any non-ASCII label (e.g. `f(é, β)`). Exact for ASCII.
          const auto& bounds = label.AsArray();
          if (bounds.size() == 2) {
            const std::int64_t start = bounds[0].AsInt(0);
            const std::int64_t end = bounds[1].AsInt(0);
            if (start >= 0 && end >= start) {
              const std::size_t start_byte = lsp_encoding::LspCharacterToByteColumn(
                  info.label, static_cast<std::size_t>(start),
                  lsp_encoding::PositionEncoding::Utf16);
              const std::size_t end_byte = lsp_encoding::LspCharacterToByteColumn(
                  info.label, static_cast<std::size_t>(end),
                  lsp_encoding::PositionEncoding::Utf16);
              if (end_byte >= start_byte) {
                out_parameter.label =
                    info.label.substr(start_byte, end_byte - start_byte);
              }
            }
          }
        } else {
          out_parameter.label = label.AsString();
        }
        out_parameter.documentation = StringOrValueField(parameter["documentation"]);
        info.parameters.push_back(std::move(out_parameter));
      }
    }
    help.signatures.push_back(std::move(info));
  }
  return help;
}

std::string ParseHoverContents(const JsonValue& hover_result) {
  constexpr std::size_t kMaxHoverBytes = 32 * 1024;
  static constexpr char kTruncationMarker[] = "\n\n… (truncated)";
  // Cap ANY single hover string on a UTF-8 boundary with a visible marker. A bare
  // string or `MarkupContent { value }` was previously returned in full, so a
  // server could push a tens-of-MiB hover payload (inside the 64 MiB frame cap)
  // onto the callback/render path. The array shape already caps below.
  const auto cap_string = [&](std::string s) {
    if (s.size() > kMaxHoverBytes) {
      TruncateUtf8InPlace(s, kMaxHoverBytes);
      s += kTruncationMarker;
    }
    return s;
  };

  if (!hover_result.HasKey("contents")) {
    return {};
  }
  const JsonValue& contents = hover_result["contents"];
  // MarkupContent ({kind, value}) and the object form of MarkedString ({language,
  // value}) both carry a "value" — take it directly.
  if (contents.HasKey("value")) {
    return cap_string(contents["value"].AsString());
  }
  // MarkedString[]: join each element's text with a blank line between blocks.
  // Bound both the element count and the accumulated byte size so a hostile server
  // cannot force a near-64 MiB concatenation on the callback path before the UI
  // truncates it. The hover popup only displays a few KiB; cap comfortably above
  // that and stop early, truncating the final piece on a UTF-8 boundary with an
  // explicit marker so the user sees the content was clipped.
  if (contents.IsArray()) {
    constexpr std::size_t kMaxHoverElements = 128;
    std::string out;
    std::size_t elements = 0;
    bool truncated = false;
    for (const auto& element : contents.AsArray()) {
      if (elements >= kMaxHoverElements) {
        truncated = true;
        break;
      }
      const std::string piece = StringOrValueField(element);
      if (piece.empty()) {
        continue;
      }
      ++elements;
      if (!out.empty()) {
        out += "\n\n";
      }
      out += piece;
      if (out.size() >= kMaxHoverBytes) {
        TruncateUtf8InPlace(out, kMaxHoverBytes);
        truncated = true;
        break;
      }
    }
    if (truncated) {
      out += kTruncationMarker;
    }
    return out;
  }
  // Bare MarkedString.
  if (contents.IsString()) {
    return cap_string(contents.AsString());
  }
  return {};
}

std::vector<LspClient::SemanticToken> ParseSemanticTokensData(const JsonValue& result,
                                                              std::size_t max_tokens) {
  std::vector<LspClient::SemanticToken> tokens;
  if (!result.HasKey("data")) {
    return tokens;
  }
  const JsonValue& data = result["data"];
  if (!data.IsArray()) {
    return tokens;
  }
  const auto& ints = data.AsArray();
  const std::size_t groups = ints.size() / 5;  // 5 ints per token; trailing partial ignored
  tokens.reserve(std::min(groups, max_tokens));
  // Accumulate the delta-encoded positions in 64-bit, but with SATURATING adds:
  // AsInt() clamps to [INT64_MIN, INT64_MAX], and a dropped out-of-range token still
  // leaves the accumulator huge, so a plain `line += delta` can execute signed int64
  // overflow (UB) — e.g. two groups whose deltaLine is INT64_MAX each, or a same-line
  // run of huge positive deltaStart. Saturating at the int64 bounds keeps the later
  // `> INT_MAX` drop decision correct without ever overflowing.
  const auto sat_add = [](std::int64_t a, std::int64_t b) -> std::int64_t {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
      return std::numeric_limits<std::int64_t>::max();
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return a + b;
  };
  std::int64_t line = 0;
  std::int64_t start_char = 0;
  for (std::size_t i = 0; i + 5 <= ints.size() && tokens.size() < max_tokens; i += 5) {
    const std::int64_t delta_line = ints[i].AsInt();
    const std::int64_t delta_start = ints[i + 1].AsInt();
    const std::int64_t length = ints[i + 2].AsInt();
    const int token_type = JsonIntInRange(ints[i + 3]);
    if (delta_line > 0) {
      line = sat_add(line, delta_line);
      start_char = delta_start;
    } else {
      start_char = sat_add(start_char, delta_start);
    }
    static constexpr std::int64_t kMaxCoord = std::numeric_limits<int>::max();
    if (length <= 0 || length > kMaxCoord || line < 0 || line > kMaxCoord ||
        start_char < 0 || start_char > kMaxCoord) {
      continue;  // skip degenerate/out-of-range tokens but keep decoding the rest
    }
    tokens.push_back(LspClient::SemanticToken{.line = static_cast<int>(line),
                                              .start_char = static_cast<int>(start_char),
                                              .length = static_cast<int>(length),
                                              .token_type = token_type});
  }
  return tokens;
}

std::vector<LspClient::InlayHint> ParseInlayHints(const JsonValue& result, std::size_t max_hints) {
  std::vector<LspClient::InlayHint> hints;
  if (!result.IsArray()) {
    return hints;
  }
  // Cap the label a single hint can carry: inlay labels are short annotations
  // (": i32", "name:"), so a huge one is a hostile/degenerate response we clamp
  // rather than materialize and try to render mid-line.
  constexpr std::size_t kMaxLabelBytes = 512;
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_hints);
  hints.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const JsonValue& entry = array[i];
    if (!entry.IsObject()) {
      continue;
    }
    LspClient::InlayHint hint;
    hint.position = ParsePosition(entry["position"]);
    // label is either a plain string or an InlayHintLabelPart[]; flatten parts by
    // concatenating each part's `value`.
    const JsonValue& label = entry["label"];
    if (label.IsString()) {
      hint.label = label.AsString();
    } else if (label.IsArray()) {
      for (const JsonValue& part : label.AsArray()) {
        if (hint.label.size() >= kMaxLabelBytes) {
          break;
        }
        hint.label += part["value"].AsString();
      }
    }
    // Truncate on a UTF-8 boundary (not a raw byte resize) so a non-ASCII label
    // whose cap falls mid-sequence does not leave a split code point that renders
    // as a replacement glyph and breaks downstream width/codepoint iteration.
    TruncateUtf8InPlace(hint.label, kMaxLabelBytes);
    if (hint.label.empty()) {
      continue;  // nothing to render
    }
    hint.kind = JsonIntInRange(entry["kind"], 0);
    hint.padding_left = entry["paddingLeft"].AsBool(false);
    hint.padding_right = entry["paddingRight"].AsBool(false);
    hints.push_back(std::move(hint));
  }
  return hints;
}

std::vector<LspClient::DocumentHighlight> ParseDocumentHighlights(const JsonValue& result,
                                                                  std::size_t max_highlights) {
  std::vector<LspClient::DocumentHighlight> highlights;
  if (!result.IsArray()) {
    return highlights;
  }
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_highlights);
  highlights.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const JsonValue& entry = array[i];
    if (!entry.IsObject()) {
      continue;
    }
    LspClient::DocumentHighlight highlight;
    highlight.range = ParseRange(entry["range"]);
    // `kind` is optional; the spec's default is Text(1). Clamp out-of-vocabulary
    // values to Text rather than dropping the highlight — the range is still the
    // server's authoritative answer, only its read/write tint is unknown.
    const int kind = JsonIntInRange(entry["kind"], 1);
    highlight.kind = (kind >= 1 && kind <= 3) ? kind : 1;
    if (highlight.range.end.line < highlight.range.start.line ||
        (highlight.range.end.line == highlight.range.start.line &&
         highlight.range.end.character <= highlight.range.start.character)) {
      continue;  // empty or inverted range paints nothing
    }
    highlights.push_back(highlight);
  }
  return highlights;
}

LspClient::CodeLens ParseCodeLens(const JsonValue& value) {
  // Cap the title: a lens is a one-line annotation rendered inside the editor row,
  // so a huge one is a degenerate response we clamp rather than try to lay out.
  constexpr std::size_t kMaxTitleBytes = 256;
  LspClient::CodeLens lens;
  if (!value.IsObject()) {
    return lens;
  }
  lens.range = ParseRange(value["range"]);
  const JsonValue& command = value["command"];
  if (command.IsObject()) {
    lens.title = command["title"].AsString();
    TruncateUtf8InPlace(lens.title, kMaxTitleBytes);
    lens.command = command["command"].AsString();
    if (const JsonValue& args = command["arguments"]; args.IsArray()) {
      // Bound the argument list: these are forwarded verbatim to
      // workspace/executeCommand, so they are retained until the lens is replaced.
      constexpr std::size_t kMaxArguments = 64;
      const auto& array = args.AsArray();
      const std::size_t count = std::min(array.size(), kMaxArguments);
      lens.arguments.assign(array.begin(), array.begin() + static_cast<std::ptrdiff_t>(count));
    }
  }
  if (lens.title.empty()) {
    // Keep the lens object verbatim for codeLens/resolve: servers stash private
    // correlation state in `data` and reject anything we rebuild ourselves.
    lens.unresolved = value;
  }
  return lens;
}

std::vector<LspClient::CodeLens> ParseCodeLenses(const JsonValue& result,
                                                 std::size_t max_lenses) {
  std::vector<LspClient::CodeLens> lenses;
  if (!result.IsArray()) {
    return lenses;
  }
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_lenses);
  lenses.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    LspClient::CodeLens lens = ParseCodeLens(array[i]);
    if (lens.range.start.line < 0) {
      continue;
    }
    // A lens with neither a title nor anything to resolve from paints nothing.
    if (lens.title.empty() && lens.unresolved.IsNull()) {
      continue;
    }
    lenses.push_back(std::move(lens));
  }
  return lenses;
}

JsonValue MakePosition(const LspClient::Position& position) {
  JsonObject object;
  object["line"] = JsonValue(static_cast<std::int64_t>(position.line));
  object["character"] = JsonValue(static_cast<std::int64_t>(position.character));
  return JsonValue(std::move(object));
}

JsonValue MakeRange(const LspClient::Range& range) {
  JsonObject object;
  object["start"] = MakePosition(range.start);
  object["end"] = MakePosition(range.end);
  return JsonValue(std::move(object));
}

JsonValue MakeDiagnostic(const LspClient::Diagnostic& diagnostic) {
  JsonObject object;
  object["range"] = MakeRange(diagnostic.range);
  object["message"] = JsonValue(diagnostic.message);
  object["severity"] = JsonValue(static_cast<std::int64_t>(diagnostic.severity));
  if (!diagnostic.code.empty()) {
    object["code"] = JsonValue(diagnostic.code);
  }
  return JsonValue(std::move(object));
}

JsonValue MakeTextDocumentIdentifier(const std::string& uri) {
  JsonObject object;
  object["uri"] = JsonValue(uri);
  return JsonValue(std::move(object));
}

JsonValue MakeTextDocumentPositionParams(const std::string& uri,
                                         const LspClient::Position& position) {
  JsonObject params;
  params["textDocument"] = MakeTextDocumentIdentifier(uri);
  params["position"] = MakePosition(position);
  return JsonValue(std::move(params));
}

}  // namespace microide::workspace::lsp_protocol
