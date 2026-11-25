#include "workspace/JsonRpcMessageFraming.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>

#include "workspace/lsp/LspClientTrace.h"

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

}

std::optional<util::JsonValue> JsonRpcMessageFramer::Next() {
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
    buf.consume(nl + 1);
    return std::nullopt;
  }

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
  if (!header_terminated) return std::nullopt;

  if (static_cast<std::size_t>(content_len) > max_message_bytes) {
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

}
