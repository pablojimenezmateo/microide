#include "TestSupport.h"

#include "project/CompileCommandsLocator.h"

#include <filesystem>

namespace microide::tests {
namespace {

using microide::project::DiscoverCompileCommandsDir;

void TestLocatorFindsCompileCommandsInRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "compile_commands.json", "[]\n");

  const auto found = DiscoverCompileCommandsDir(root);
  Expect(found.has_value(), "a compile_commands.json in the root should be discovered");
  Expect(found.has_value() && *found == root.lexically_normal(),
         "the discovered directory should be the root");
}

void TestLocatorFindsCompileCommandsInBuildDirs() {
  for (const char* dir : {"build", "builds", "out", "cmake-build-debug"}) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    WriteFile(root / "src" / "main.cpp", "int main() {}\n");
    WriteFile(root / dir / "compile_commands.json", "[]\n");

    const auto found = DiscoverCompileCommandsDir(root);
    Expect(found.has_value(), "a compile_commands.json in a conventional build dir should be found");
    Expect(found.has_value() && *found == (root / dir).lexically_normal(),
           "the discovered directory should be the build dir");
  }
}

void TestLocatorFindsCompileCommandsInMarkerChild() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  // A non-conventional build dir name, identified by its build.ninja marker.
  WriteFile(root / "out-x64" / "build.ninja", "# ninja\n");
  WriteFile(root / "out-x64" / "compile_commands.json", "[]\n");

  const auto found = DiscoverCompileCommandsDir(root);
  Expect(found.has_value(),
         "a child dir holding build.ninja + compile_commands.json should be discovered");
  Expect(found.has_value() && *found == (root / "out-x64").lexically_normal(),
         "the discovered dir should be the marker child");
}

void TestLocatorReturnsNulloptWhenAbsent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() {}\n");
  // A build dir with a marker but NO compile_commands.json must not match.
  WriteFile(root / "build" / "CMakeCache.txt", "cache\n");

  const auto found = DiscoverCompileCommandsDir(root);
  Expect(!found.has_value(),
         "no compile_commands.json anywhere should yield nullopt");
}

}  // namespace

void RegisterCompileCommandsLocatorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CompileCommandsLocator/FindsInRoot", TestLocatorFindsCompileCommandsInRoot);
  AddTest(tests, "CompileCommandsLocator/FindsInBuildDirs",
          TestLocatorFindsCompileCommandsInBuildDirs);
  AddTest(tests, "CompileCommandsLocator/FindsInMarkerChild",
          TestLocatorFindsCompileCommandsInMarkerChild);
  AddTest(tests, "CompileCommandsLocator/ReturnsNulloptWhenAbsent",
          TestLocatorReturnsNulloptWhenAbsent);
}

}  // namespace microide::tests
