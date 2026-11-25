#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

struct ControlSendOptions {
  std::optional<int> pid;
  std::optional<std::filesystem::path> socket;

  std::vector<std::string> command_tokens;
  std::optional<std::string> query_verb;
  std::optional<std::string> query_args_json;
  std::optional<std::string> raw_json;

  int timeout_seconds = 5;
  std::optional<std::string> wait_event;
  int request_id = 1;

  bool valid = false;
  std::string error;
};

ControlSendOptions ParseControlSendArgs(const std::vector<std::string>& args);

std::optional<std::string> BuildControlSendRequest(const ControlSendOptions& options,
                                                   std::string* error);

int RunControlSend(int argc, char** argv);

}
