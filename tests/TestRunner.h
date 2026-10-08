#pragma once

#include "TestSupport.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

// What differs between the two test binaries. Everything else — argument parsing,
// filters, listing, sharding, the watchdog, the per-process isolation of every
// user directory — is one runner, so `microide_tests` and `microide_kernel_tests`
// accept the same command line and ctest shards them the same way.
struct TestSuiteConfig {
  // Prefix of every diagnostic line ("<name>: OK (...)").
  std::string_view binary_name;
  void (*register_tests)(std::vector<TestCase>& tests) = nullptr;
  // Process-wide bindings made before argument parsing (the shell binds the wake
  // path to SDL's event queue here).
  std::function<void()> install_process_hooks;
  // Runs after the arguments parse and before any test is registered; an error
  // string exits with status 2.
  std::function<std::optional<std::string>()> set_up;
  // Runs on every exit path once argument parsing has started.
  std::function<void()> tear_down;
};

int RunTestSuite(int argc, char** argv, const TestSuiteConfig& config);

}  // namespace microide::tests
