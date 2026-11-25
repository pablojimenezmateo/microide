#include "workspace/lsp/LspProtocol.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/ProtocolNumeric.h"

namespace microide::workspace::lsp_protocol {

using util::JsonObject;
using util::JsonValue;
using protocol_numeric::JsonIntInRange;

LspClient::Position ParsePosition(const JsonValue& value) {
  LspClient::Position position;
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

constexpr std::size_t kMaxLspLocations = 50000;
constexpr std::size_t kMaxLspDiagnostics = 10000;
constexpr std::size_t kMaxLspSymbolNodes = 100000;

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
  {
    const int raw = JsonIntInRange(value["severity"], 1);
    diagnostic.severity = (raw >= 1 && raw <= 4) ? raw : 1;
  }
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

constexpr int kMaxDocumentSymbolDepth = 64;

static LspClient::DocumentSymbol ParseDocumentSymbolAtDepth(const JsonValue& value, int depth,
                                                            std::size_t& remaining) {
  LspClient::DocumentSymbol symbol;
  symbol.name = value["name"].AsString();
  symbol.detail = value["detail"].AsString();
  symbol.kind = JsonIntInRange(value["kind"], 1);
  if (value.HasKey("location")) {
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
  const auto& children = value["children"].AsArray();
  symbol.children.reserve(std::min(children.size(), remaining));
  for (const auto& child : children) {
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
          continue;
        }
        op.overwrite = options["overwrite"].AsBool(false);
        op.ignore_if_exists = options["ignoreIfExists"].AsBool(false);
        op.ignore_if_not_exists = options["ignoreIfNotExists"].AsBool(false);
        op.recursive = options["recursive"].AsBool(false);
        if (op.uri.empty() ||
            (op.kind == LspClient::WorkspaceEdit::ResourceOp::Kind::Rename &&
             op.new_uri.empty())) {
          continue;
        }
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
        continue;
      }
      if (!doc_change.HasKey("edits") || !doc_change["edits"].IsArray()) {
        continue;
      }
      const JsonValue& text_document = doc_change["textDocument"];
      const std::string& uri = text_document["uri"].AsString();
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
    return out;
  }
  if (result.HasKey("defaultBehavior")) {
    out.can_rename = result["defaultBehavior"].AsBool(false);
    return out;
  }
  if (result.HasKey("range")) {
    out.can_rename = true;
    out.range = ParseRange(result["range"]);
    out.placeholder = result["placeholder"].AsString();
    return out;
  }
  if (result.HasKey("start") && result.HasKey("end")) {
    out.can_rename = true;
    out.range = ParseRange(result);
    return out;
  }
  return out;
}

namespace {
std::string StringOrValueField(const JsonValue& value) {
  if (value.IsString()) {
    return value.AsString();
  }
  if (value.HasKey("value")) {
    return value["value"].AsString();
  }
  return {};
}

inline void TruncateUtf8InPlace(std::string& text, std::size_t max_bytes) {
  util::TruncateUtf8ToByteBudget(text, max_bytes);
}
}

LspClient::SignatureHelp ParseSignatureHelp(const JsonValue& result) {
  LspClient::SignatureHelp help;
  if (!result.HasKey("signatures") || !result["signatures"].IsArray()) {
    return help;
  }
  help.active_signature = JsonIntInRange(result["activeSignature"], 0);
  help.active_parameter = JsonIntInRange(result["activeParameter"], 0);
  constexpr std::size_t kMaxSignatures = 64;
  constexpr std::size_t kMaxParameters = 512;
  const auto& signatures = result["signatures"].AsArray();
  const std::size_t signature_count = std::min(signatures.size(), kMaxSignatures);
  help.signatures.reserve(signature_count);
  for (std::size_t i = 0; i < signature_count; ++i) {
    const JsonValue& signature = signatures[i];
    LspClient::SignatureInformation info;
    info.label = signature["label"].AsString();
    info.documentation = StripMarkdownFences(StringOrValueField(signature["documentation"]));
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

std::string StripMarkdownFences(std::string text) {
  if (text.find("```") == std::string::npos && text.find("~~~") == std::string::npos) {
    return text;
  }
  std::string out;
  out.reserve(text.size());
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::size_t line_end = end == std::string::npos ? text.size() : end;
    std::string_view line(text.data() + start, line_end - start);
    std::string_view trimmed = line;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
      trimmed.remove_prefix(1);
    }
    const bool fence = trimmed.starts_with("```") || trimmed.starts_with("~~~");
    if (!fence) {
      if (!out.empty() || start > 0) {
        if (!out.empty()) out.push_back('\n');
      }
      out.append(line);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

std::string HoverMarkdownToText(std::string_view markdown) {
  std::string out;
  out.reserve(markdown.size());
  std::string_view open_fence;
  std::size_t start = 0;
  while (start <= markdown.size()) {
    const std::size_t end = markdown.find('\n', start);
    const std::size_t stop = end == std::string_view::npos ? markdown.size() : end;
    std::string_view line = markdown.substr(start, stop - start);
    std::string_view fence_probe = line;
    while (!fence_probe.empty() && (fence_probe.front() == ' ' || fence_probe.front() == '\t')) {
      fence_probe.remove_prefix(1);
    }
    const bool fence_line = fence_probe.starts_with("```") || fence_probe.starts_with("~~~");
    if (fence_line && (open_fence.empty() || fence_probe.starts_with(open_fence))) {
      open_fence = open_fence.empty() ? fence_probe.substr(0, 3) : std::string_view{};
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
      continue;
    }
    if (!open_fence.empty()) {
      while (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      out.append(line);
      if (end == std::string_view::npos) {
        break;
      }
      out += '\n';
      start = end + 1;
      continue;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    std::string_view body = line;
    while (!body.empty() && body.front() == ' ') {
      body.remove_prefix(1);
    }
    const bool rule = body.size() >= 3 &&
                      body.find_first_not_of(body.front()) == std::string_view::npos &&
                      (body.front() == '-' || body.front() == '*' || body.front() == '_');
    if (rule) {
      out += '\n';
    } else {
      std::size_t hashes = 0;
      while (hashes < body.size() && hashes < 6 && body[hashes] == '#') {
        ++hashes;
      }
      if (hashes > 0 && hashes < body.size() && body[hashes] == ' ') {
        line = body.substr(hashes + 1);
      }
      for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size() &&
            std::string_view("\\`*_{}[]()#+-.!|<>~\"'").find(line[i + 1]) != std::string_view::npos) {
          out += line[++i];
          continue;
        }
        if (c == '`') {
          std::size_t run = 1;
          while (i + run < line.size() && line[i + run] == '`') {
            ++run;
          }
          std::size_t close = std::string_view::npos;
          for (std::size_t j = i + run; j < line.size();) {
            if (line[j] != '`') {
              ++j;
              continue;
            }
            std::size_t other = 0;
            while (j + other < line.size() && line[j + other] == '`') {
              ++other;
            }
            if (other == run) {
              close = j;
              break;
            }
            j += other;
          }
          if (close == std::string_view::npos) {
            out.append(line.substr(i, run));
            i += run - 1;
            continue;
          }
          std::string_view code = line.substr(i + run, close - (i + run));
          if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' &&
              code.find_first_not_of(' ') != std::string_view::npos) {
            code = code.substr(1, code.size() - 2);
          }
          out.append(code);
          i = close + run - 1;
          continue;
        }
        if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
          ++i;
          continue;
        }
        out += c;
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    out += '\n';
    start = end + 1;
  }
  std::string folded;
  folded.reserve(out.size());
  for (const char c : out) {
    if (c == '\n' && folded.size() >= 2 && folded[folded.size() - 1] == '\n' &&
        folded[folded.size() - 2] == '\n') {
      continue;
    }
    folded += c;
  }
  while (!folded.empty() && folded.back() == '\n') {
    folded.pop_back();
  }
  folded.erase(0, folded.find_first_not_of('\n') == std::string::npos ? folded.size()
                                                                      : folded.find_first_not_of('\n'));
  return folded;
}

std::string ParseHoverContents(const JsonValue& hover_result) {
  constexpr std::size_t kMaxHoverBytes = 32 * 1024;
  static constexpr char kTruncationMarker[] = "\n\n… (truncated)";
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
  if (contents.HasKey("value")) {
    const bool markdown = contents["kind"].AsString() == "markdown";
    std::string value = cap_string(contents["value"].AsString());
    return markdown ? HoverMarkdownToText(value) : value;
  }
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
      const std::string piece = element.IsString()
                                    ? HoverMarkdownToText(element.AsString())
                                    : StripMarkdownFences(StringOrValueField(element));
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
  if (contents.IsString()) {
    return HoverMarkdownToText(cap_string(contents.AsString()));
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
  const std::size_t groups = ints.size() / 5;
  tokens.reserve(std::min(groups, max_tokens));
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
      continue;
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
    TruncateUtf8InPlace(hint.label, kMaxLabelBytes);
    if (hint.label.empty()) {
      continue;
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
    const int kind = JsonIntInRange(entry["kind"], 1);
    highlight.kind = (kind >= 1 && kind <= 3) ? kind : 1;
    if (highlight.range.end.line < highlight.range.start.line ||
        (highlight.range.end.line == highlight.range.start.line &&
         highlight.range.end.character <= highlight.range.start.character)) {
      continue;
    }
    highlights.push_back(highlight);
  }
  return highlights;
}

LspClient::CodeLens ParseCodeLens(const JsonValue& value) {
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
      constexpr std::size_t kMaxArguments = 64;
      const auto& array = args.AsArray();
      const std::size_t count = std::min(array.size(), kMaxArguments);
      lens.arguments.assign(array.begin(), array.begin() + static_cast<std::ptrdiff_t>(count));
    }
  }
  if (lens.title.empty()) {
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
    if (lens.title.empty() && lens.unresolved.IsNull()) {
      continue;
    }
    lenses.push_back(std::move(lens));
  }
  return lenses;
}

namespace {
LspClient::CallHierarchyItem ParseCallHierarchyItem(const JsonValue& value) {
  constexpr std::size_t kMaxNameBytes = 512;
  LspClient::CallHierarchyItem item;
  if (!value.IsObject()) {
    return item;
  }
  item.name = value["name"].AsString();
  TruncateUtf8InPlace(item.name, kMaxNameBytes);
  item.detail = value["detail"].AsString();
  TruncateUtf8InPlace(item.detail, kMaxNameBytes);
  item.kind = JsonIntInRange(value["kind"], 1);
  item.uri = value["uri"].AsString();
  item.range = ParseRange(value["range"]);
  item.selection_range = value.HasKey("selectionRange") ? ParseRange(value["selectionRange"])
                                                        : item.range;
  item.raw = value;
  return item;
}
}

std::vector<LspClient::CallHierarchyItem> ParseCallHierarchyItems(const JsonValue& result,
                                                                  std::size_t max_items) {
  std::vector<LspClient::CallHierarchyItem> items;
  if (!result.IsArray()) {
    return items;
  }
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_items);
  items.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    LspClient::CallHierarchyItem item = ParseCallHierarchyItem(array[i]);
    if (item.uri.empty()) {
      continue;
    }
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<LspClient::CallHierarchyCall> ParseCallHierarchyCalls(const JsonValue& result,
                                                                  bool incoming,
                                                                  std::size_t max_calls) {
  std::vector<LspClient::CallHierarchyCall> calls;
  if (!result.IsArray()) {
    return calls;
  }
  const char* const item_key = incoming ? "from" : "to";
  const auto& array = result.AsArray();
  const std::size_t count = std::min(array.size(), max_calls);
  calls.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const JsonValue& entry = array[i];
    if (!entry.IsObject()) {
      continue;
    }
    LspClient::CallHierarchyCall call;
    call.item = ParseCallHierarchyItem(entry[item_key]);
    if (call.item.uri.empty()) {
      continue;
    }
    if (const JsonValue& ranges = entry["fromRanges"]; ranges.IsArray()) {
      constexpr std::size_t kMaxCallRanges = 1024;
      const auto& range_array = ranges.AsArray();
      const std::size_t range_count = std::min(range_array.size(), kMaxCallRanges);
      call.call_ranges.reserve(range_count);
      for (std::size_t r = 0; r < range_count; ++r) {
        call.call_ranges.push_back(ParseRange(range_array[r]));
      }
    }
    calls.push_back(std::move(call));
  }
  return calls;
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

}
