#pragma once

#include <string>

namespace microide::provider {

struct BridgeOptions {
  std::string provider;
  std::string base_url;
  std::string default_model;
};

int RunProviderBridge(const BridgeOptions& options);

}  // namespace microide::provider
