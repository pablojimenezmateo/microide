#include "TestSupport.h"

#include "project/CompileCommandsLocator.h"
#include "ScriptedProcessLauncher.h"

#include <algorithm>
#include <filesystem>

namespace microide::tests {
namespace {

using microide::project::DiscoverCompileCommandsDir;
using microide::project::PlanClangdCommand;

bool HasCompileCommandsDirFlag(const std::vector<std::string>& command) {
  return std::any_of(command.begin(), command.end(), [](const std::string& arg) {
    return arg.rfind("--compile-commands-dir", 0) == 0;
  });
}

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

// A local project with its database in a directory clangd does not search gets
// the flag; other servers and an explicit flag are left alone.
void TestPlanPointsLocalClangdAtANonStandardBuildDir() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "out" / "compile_commands.json", "[]\n");
  ScriptedProcessLauncher launcher;

  const auto plan = PlanClangdCommand({"clangd", "--background-index"}, root, launcher);
  Expect(HasCompileCommandsDirFlag(plan.command), "a local clangd should be pointed at out/");
  Expect(plan.note.find("pointing clangd") != std::string::npos, "the plan says what it did");

  const auto other = PlanClangdCommand({"pylsp"}, root, launcher);
  Expect(other.command == std::vector<std::string>{"pylsp"} && other.note.empty(),
         "a server that is not clangd passes through untouched");
  const auto explicit_dir =
      PlanClangdCommand({"clangd", "--compile-commands-dir=/x"}, root, launcher);
  Expect(explicit_dir.command.size() == 2 && explicit_dir.note.empty(),
         "a command that names its own directory is left alone");
}

// TD-2026-10-16-344: a remote project's tree here is the mirror, which never
// holds the host's build output -- so the host neither searches it, nor warns
// that the database is missing, nor points clangd at a mirror path.
void TestPlanLeavesRemoteClangdToItsOwnDiscovery() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "mirror";
  WriteFile(root / "out" / "compile_commands.json", "[]\n");  // stale local copy
  ScriptedProcessLauncher launcher;
  launcher.local = false;

  const auto plan = PlanClangdCommand({"clangd"}, root, launcher);
  Expect(!HasCompileCommandsDirFlag(plan.command),
         "a remote clangd must not be pointed at a directory found in the mirror");
  Expect(plan.note.find("no compile_commands.json") == std::string::npos,
         "a remote project must not be warned about a database missing from the mirror");
  Expect(plan.note.find("scripted") != std::string::npos,
         "the note names the machine clangd runs on");
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
  AddTest(tests, "CompileCommandsLocator/PlanPointsLocalClangdAtANonStandardBuildDir",
          TestPlanPointsLocalClangdAtANonStandardBuildDir);
  AddTest(tests, "CompileCommandsLocator/PlanLeavesRemoteClangdToItsOwnDiscovery",
          TestPlanLeavesRemoteClangdToItsOwnDiscovery);
}

}  // namespace microide::tests
