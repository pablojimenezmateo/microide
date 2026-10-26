#include "TestSupport.h"

#include "project/ProjectId.h"
#include "project/remote/RemoteProject.h"

#include <filesystem>
#include <string>

namespace microide::tests {
namespace {

using project::ProjectId;
namespace remote = project::remote;

// Mirrors live under the data directory: one per test, so a test sees only its own.
struct ScopedDataHome {
  TemporaryDirectory dir;
  ScopedEnvVar env{"XDG_DATA_HOME", dir.path().string()};
};

// A mirror as Open leaves one, without a connection: its tree and its record.
std::filesystem::path MakeMirror(const std::string& host, const std::string& host_root) {
  const std::filesystem::path tree = remote::RemoteProject::DefaultTree(host, host_root);
  std::filesystem::create_directories(tree);
  std::filesystem::create_directories(tree.parent_path() / "meta");
  Expect(remote::RemoteProject::WriteRecord(tree, {.host = host, .host_root = host_root}),
         "the mirror's record is written");
  return tree;
}

void TestLabelsAreTheLastNamedComponent() {
  Expect(ProjectId::ForLocal("/home/me/repo").Label() == "repo", "a local project's basename");
  Expect(ProjectId::ForLocal("/home/me/repo/").Label() == "repo", "a trailing separator is not a name");
  Expect(ProjectId::ForLocal("/home/me/./repo").Label() == "repo", "nor is a `.` component");
  Expect(ProjectId::ForLocal("/").Label() == "/", "a filesystem root is shown whole");
  Expect(ProjectId::ForRemote("worky", "/home/me/repo").Label() == "repo",
         "a remote project's label is the HOST root's basename");
  Expect(ProjectId::ForRemote("worky", "/srv/app/").Label() == "app", "trailing separator on the host");
  Expect(ProjectId::ForRemote("worky", "/").Label() == "/", "the host's filesystem root");
  const ProjectId none;
  Expect(none.empty() && none.Label().empty() && !none.IsRemote(), "no project has no label");
  Expect(!ProjectId::ForRemote("worky", "/").empty(), "a remote id is never empty");
}

void TestLocationIsThePathOrHostColonPath() {
  Expect(ProjectId::ForLocal("/home/me/repo").Location() == "/home/me/repo", "a local path");
  Expect(ProjectId::ForLocal("/home/me/x/../repo").Location() == "/home/me/repo", "normalized");
  Expect(ProjectId::ForRemote("dev@worky:2222", "/home/me/repo").Location() ==
             "dev@worky:2222:/home/me/repo",
         "host:/path for a remote project");
}

// The id names the project; the I/O root is derived from it, and resolving that
// root again gives the same id back.
void TestIoRootRoundTrips() {
  const ScopedDataHome data;
  const ProjectId local = ProjectId::ForLocal("/home/me/repo");
  Expect(local.IoRoot() == "/home/me/repo", "a local project's root is its path");

  const ProjectId remote_id = ProjectId::ForRemote("dev@worky", "/home/me/repo");
  const std::filesystem::path tree = remote_id.IoRoot();
  Expect(tree == remote::RemoteProject::DefaultTree("dev@worky", "/home/me/repo"),
         "a remote project's root is its mirror's default tree");
  Expect(project::ResolveProjectId(tree) == ProjectId::ForLocal(tree),
         "before the mirror exists the path is an ordinary folder");
  MakeMirror("dev@worky", "/home/me/repo");
  Expect(project::ResolveProjectId(tree) == remote_id, "the mirror tree resolves to its remote id");
  Expect(project::ResolveProjectId(tree).IoRoot() == tree, "and back to the same root");
}

void TestACopiedMirrorIsLocal() {
  const ScopedDataHome data;
  TemporaryDirectory dir;
  const std::filesystem::path tree = MakeMirror("box", "/srv/copied");
  // The whole mirror directory copied elsewhere: its record still names the host.
  const std::filesystem::path copy = dir.path() / "elsewhere";
  std::filesystem::copy(tree.parent_path(), copy, std::filesystem::copy_options::recursive);
  const std::filesystem::path copied_tree = copy / tree.filename();
  Expect(remote::RemoteProject::ReadRecord(copied_tree).has_value(), "the copy carries the record");
  Expect(project::ResolveProjectId(copied_tree) == ProjectId::ForLocal(copied_tree),
         "but it is not the mirror's own tree, so it is a local folder");
  Expect(project::ResolveProjectId(dir.path()) == ProjectId::ForLocal(dir.path()),
         "an ordinary folder is local");
}

// The catalog dedupes by I/O root, which is only sound if two different ids never
// share one: distinct remotes get distinct trees, and a local id equal to a mirror
// tree is not what that tree resolves to.
void TestDistinctIdsNeverShareAnIoRoot() {
  const ScopedDataHome data;
  const ProjectId a = ProjectId::ForRemote("box", "/srv/app");
  const ProjectId b = ProjectId::ForRemote("other", "/srv/app");
  const ProjectId c = ProjectId::ForRemote("box", "/srv/lib/app");
  Expect(a.IoRoot() != b.IoRoot(), "same folder, different hosts");
  Expect(a.IoRoot() != c.IoRoot(), "same basename, different folders");
  const std::filesystem::path tree = MakeMirror("box", "/srv/app");
  Expect(project::ResolveProjectId(tree) == a && !(ProjectId::ForLocal(tree) == a),
         "the tree's own path resolves remote, never to a second, local id");
}

}  // namespace

void RegisterProjectIdTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectId/LabelsAreTheLastNamedComponent", TestLabelsAreTheLastNamedComponent);
  AddTest(tests, "ProjectId/LocationIsThePathOrHostColonPath", TestLocationIsThePathOrHostColonPath);
  AddTest(tests, "ProjectId/IoRootRoundTrips", TestIoRootRoundTrips);
  AddTest(tests, "ProjectId/ACopiedMirrorIsLocal", TestACopiedMirrorIsLocal);
  AddTest(tests, "ProjectId/DistinctIdsNeverShareAnIoRoot", TestDistinctIdsNeverShareAnIoRoot);
}

}  // namespace microide::tests
