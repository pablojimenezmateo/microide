#include "workspace/JsonRpcMessageFraming.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>

#include "workspace/LspClientTrace.h"

namespace microide::workspace {

namespace {

bool EqualsAsciiCaseInsensitive(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
    const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

void TrimAsciiSpaces(std::string_view& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
}

// Parse a `Content-Length` header line tolerantly: the header name is matched
// case-insensitively (JSON-RPC-over-header peers are often HTTP-style tolerant),
// and whitespace around the value is optional (`Content-Length:42` is valid).
// Returns the byte count, or nullopt if the line is not a Content-Length header.
std::optional<std::string_view> ContentLengthValue(std::string_view line) {
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  std::string_view name = line.substr(0, colon);
  TrimAsciiSpaces(name);
  if (!EqualsAsciiCaseInsensitive(name, "content-length")) return std::nullopt;
  std::string_view value = line.substr(colon + 1);
  TrimAsciiSpaces(value);
  return value;
}

}  // namespace

std::optional<util::JsonValue> JsonRpcMessageFramer::Next() {
  // Draining the body of an oversized frame we chose to skip: consume what is
  // buffered and stop until the rest arrives. The read loop keeps feeding bytes,
  // so the frame is discarded a chunk at a time without the buffer ever growing.
  if (skip_body_bytes > 0) {
    const std::size_t drop = std::min<std::size_t>(skip_body_bytes, buf.view().size());
    buf.consume(drop);
    skip_body_bytes -= drop;
    return std::nullopt;
  }

  const std::string_view v = buf.view();
  const auto nl = v.find('\n');
  if (nl == std::string_view::npos) return std::nullopt;

  std::string_view line = v.substr(0, nl);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  const auto len_value = ContentLengthValue(line);
  if (!len_value.has_value()) {
    buf.consume(nl + 1);
    return std::nullopt;
  }

  const std::string_view len_sv = *len_value;
  int content_len = 0;
  const auto [ptr, ec] = std::from_chars(len_sv.data(), len_sv.data() + len_sv.size(), content_len);
  if (ec != std::errc{} || content_len <= 0) {
    // Malformed/absurd length (an out-of-int-range value fails to parse here):
    // drop the header line and try to resync on the next. The read-buffer cap
    // remains the backstop if the stream never recovers.
    buf.consume(nl + 1);
    return std::nullopt;
  }

  // Locate the end of the header block (the blank line). Headers are tiny, so
  // waiting for the whole block never blocks on the (possibly huge) body.
  std::size_t body_start = nl + 1;
  bool header_terminated = false;
  while (body_start < v.size()) {
    const auto nl2 = v.find('\n', body_start);
    if (nl2 == std::string_view::npos) return std::nullopt;
    std::string_view hdr = v.substr(body_start, nl2 - body_start);
    if (!hdr.empty() && hdr.back() == '\r') hdr.remove_suffix(1);
    body_start = nl2 + 1;
    if (hdr.empty()) {
      header_terminated = true;
      break;
    }
  }
  // The loop can also exit because the buffer ran out mid-header-block (e.g. a
  // recv split right on a header newline). `body_start` then points at the
  // buffer end, NOT past the real blank-line terminator. Committing the
  // oversized skip below with that body_start would count the still-unseen
  // terminator bytes as body and stop short, desyncing the stream. Wait for the
  // rest of the header block instead.
  if (!header_terminated) return std::nullopt;

  if (static_cast<std::size_t>(content_len) > max_message_bytes) {
    // Too large to buffer: skip the entire frame (headers + body) so the parser
    // resyncs to the next frame instead of reading body bytes as headers.
    buf.consume(body_start);
    skip_body_bytes = static_cast<std::size_t>(content_len);
    return std::nullopt;
  }

  if (v.size() - body_start < static_cast<std::size_t>(content_len)) {
    return std::nullopt;
  }

  const std::string_view body = v.substr(body_start, content_len);
  auto parsed = util::ParseJson(body);
  buf.consume(body_start + content_len);
  return parsed;
}

}  // namespace microide::workspace
