#include "perf/PerfHarnessIsolation.h"

#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::tests::perf {

namespace {

std::uint64_t CurrentProcessId() {
#if defined(__unix__) || defined(__APPLE__)
  return static_cast<std::uint64_t>(::getpid());
#else
  return 0ULL;
#endif
}

}  // namespace

std::filesystem::path EstablishIsolatedAppRoot(bool keep_artifacts, std::string* error) {
  std::error_code ec;
  const std::filesystem::path tmp_base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    if (error != nullptr) {
      *error = std::string("temp_directory_path failed: ") + ec.message();
    }
    return {};
  }

  std::ostringstream name;
  name << "microide-perf-" << CurrentProcessId();
  if (!keep_artifacts) {
    std::random_device rd;
    name << '-' << std::hex << rd() << rd();
  }
  const std::filesystem::path root = tmp_base / name.str();

  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create isolated app-root " + root.string() + ": " + ec.message();
    }
    return {};
  }
  for (const char* sub : {"config", "state", "cache", "data"}) {
    std::filesystem::create_directories(root / sub, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create isolated subdir " + (root / sub).string() + ": " + ec.message();
      }
      return {};
    }
  }

#if defined(_WIN32)
  _putenv_s("XDG_CONFIG_HOME", (root / "config").string().c_str());
  _putenv_s("XDG_STATE_HOME", (root / "state").string().c_str());
  _putenv_s("XDG_CACHE_HOME", (root / "cache").string().c_str());
  _putenv_s("XDG_DATA_HOME", (root / "data").string().c_str());
#else
  setenv("XDG_CONFIG_HOME", (root / "config").string().c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").string().c_str(), 1);
  setenv("XDG_CACHE_HOME", (root / "cache").string().c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").string().c_str(), 1);
#endif

  if (keep_artifacts) {
    std::cerr << "[perf] keeping isolated app-root at " << root.string() << "\n";
  }
  return root;
}

void CleanupIsolatedAppRoot(const std::filesystem::path& root, bool keep) {
  if (root.empty() || keep) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

}  // namespace microide::tests::perf
