#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "util/JsonValue.h"

namespace microide::workspace {

std::span<const std::string_view> ControlQueryVerbs();

struct ControlRequest {
  bool valid = false;
  std::string parse_error;
  std::optional<std::int64_t> id;
  std::string command;
  std::string query;
  util::JsonValue args;

  bool is_command() const { return !command.empty(); }
  bool is_query() const { return !query.empty(); }
};

ControlRequest ParseControlRequest(std::string_view json_line);

struct ControlResponse {
  std::optional<std::int64_t> id;
  bool ok = false;
  std::string feedback;
  std::string error;
  std::optional<util::JsonValue> result;
};

std::string SerializeControlResponse(const ControlResponse& response);
std::string SerializeControlResponseWithin(const ControlResponse& response, std::size_t max_line_bytes);

std::string SerializeControlEvent(const util::JsonValue& event);

std::string ControlChannelHelpText();

}
