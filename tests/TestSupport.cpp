#include "TestSupport.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace microide::tests {

void AddTest(std::vector<TestCase>& tests, std::string_view name, void (*run)()) {
  tests.push_back(TestCase{std::string(name), run});
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::filesystem::path TestRoot() {
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR);
}

std::filesystem::path FixturePath(std::string_view relative_path) {
  return TestRoot() / "fixtures" / relative_path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to read file: " + path.string());
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  stream << content;
}

void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::filesystem::create_directories(destination);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = std::filesystem::relative(entry.path(), source);
    const auto target = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target);
      continue;
    }
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
  }
}

std::string ShellEscape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

int RunCommand(const std::string& command) {
  return std::system(command.c_str());
}

void RequireCommandSuccess(const std::string& command, std::string_view context) {
  if (RunCommand(command) != 0) {
    throw std::runtime_error(std::string(context) + ": command failed: " + command);
  }
}

TemporaryDirectory::TemporaryDirectory() {
  path_ = std::filesystem::temp_directory_path() /
          ("microide-tests-" + std::to_string(std::rand()) + "-" + std::to_string(std::rand()));
  std::filesystem::create_directories(path_);
}

TemporaryDirectory::~TemporaryDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

ScopedEnvVar::ScopedEnvVar(std::string name, std::string value) : name_(std::move(name)) {
  const char* previous = std::getenv(name_.c_str());
  if (previous != nullptr) {
    had_previous_ = true;
    previous_value_ = previous;
  }
  Set(value);
}

ScopedEnvVar::~ScopedEnvVar() {
  if (had_previous_) {
    Set(previous_value_);
    return;
  }
#if defined(_WIN32)
  _putenv_s(name_.c_str(), "");
#else
  unsetenv(name_.c_str());
#endif
}

void ScopedEnvVar::Set(const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name_.c_str(), value.c_str());
#else
  setenv(name_.c_str(), value.c_str(), 1);
#endif
}

}  // namespace microide::tests
