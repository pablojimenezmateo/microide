#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "util/JsonValue.h"

namespace microide::workspace {

inline constexpr std::size_t kDefaultMaxJsonRpcMessageBytes = 64ull * 1024 * 1024;

struct ReadBuf {
  std::string data;
  std::size_t pos = 0;

  std::string_view view() const { return std::string_view(data).substr(pos); }

  void consume(std::size_t n) {
    pos += n;
    if (pos > 65536 && pos > data.size() / 2) {
      data.erase(0, pos);
      pos = 0;
    }
  }

  void append(std::string_view chunk) { data.append(chunk); }
};

struct JsonRpcMessageFramer {
  ReadBuf buf;
  std::size_t max_message_bytes = kDefaultMaxJsonRpcMessageBytes;
  std::size_t skip_body_bytes = 0;

  void Append(std::string_view chunk) { buf.append(chunk); }

  std::size_t BufferedBytes() const { return buf.view().size(); }

  std::optional<util::JsonValue> Next();
};

}
