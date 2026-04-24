#include <iostream>
#include <string>
#include <string_view>

#include "provider/CloudProviderBridge.h"

namespace {

void PrintUsage() {
  std::cerr << "usage: microide_provider_bridge --provider <openai|anthropic> "
               "[--base-url <url>] [--default-model <model>]\n";
}

}  // namespace

int main(int argc, char** argv) {
  microide::provider::BridgeOptions options;
  options.base_url = "https://api.openai.com";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto require_value = [&](std::string* out) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      *out = argv[++i];
      return true;
    };

    if (arg == "--provider") {
      if (!require_value(&options.provider)) {
        PrintUsage();
        return 1;
      }
      if (options.provider == "anthropic" && options.base_url == "https://api.openai.com") {
        options.base_url = "https://api.anthropic.com";
      }
      continue;
    }
    if (arg == "--base-url") {
      if (!require_value(&options.base_url)) {
        PrintUsage();
        return 1;
      }
      continue;
    }
    if (arg == "--default-model") {
      if (!require_value(&options.default_model)) {
        PrintUsage();
        return 1;
      }
      continue;
    }

    PrintUsage();
    return 1;
  }

  if (options.provider.empty()) {
    PrintUsage();
    return 1;
  }
  if (options.base_url.empty()) {
    options.base_url = options.provider == "anthropic" ? "https://api.anthropic.com"
                                                        : "https://api.openai.com";
  }

  return microide::provider::RunProviderBridge(options);
}
