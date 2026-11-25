#pragma once

#include "TestSupport.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

struct TestSuiteConfig {
  std::string_view binary_name;
  void (*register_tests)(std::vector<TestCase>& tests) = nullptr;
  std::function<void()> install_process_hooks;
  std::function<std::optional<std::string>()> set_up;
  std::function<void()> tear_down;
};

int RunTestSuite(int argc, char** argv, const TestSuiteConfig& config);

}
