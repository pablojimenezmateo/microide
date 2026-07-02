#include "TestSupport.h"

#include "platform/DetachedProcess.h"
#include "workspace/ControlChannelService.h"
#include "workspace/WorkspaceDetachCoordinator.h"

#include <filesystem>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ControlInstanceDescriptor;
using microide::workspace::ResolveDropTarget;

ControlInstanceDescriptor MakeInstance(int pid, int x, int y, int w, int h) {
  ControlInstanceDescriptor descriptor;
  descriptor.pid = pid;
  descriptor.socket = std::filesystem::path("/run/microide/" + std::to_string(pid) + ".sock");
  descriptor.win_x = x;
  descriptor.win_y = y;
  descriptor.win_w = w;
  descriptor.win_h = h;
  return descriptor;
}

void TestResolveDropTargetHitsWindowUnderPoint() {
  const std::vector<ControlInstanceDescriptor> instances = {
      MakeInstance(100, 0, 0, 800, 600),
      MakeInstance(200, 900, 100, 800, 600),
  };
  const auto target = ResolveDropTarget(1000, 300, /*self_pid=*/999, instances);
  Expect(target.has_value(), "a point inside window 200's bounds should resolve a target");
  Expect(target->pid == 200, "the resolved target should be the window under the point");
}

void TestResolveDropTargetSkipsSelfWindow() {
  const std::vector<ControlInstanceDescriptor> instances = {
      MakeInstance(100, 0, 0, 800, 600),
  };
  // The point is inside window 100, but 100 is the dragging window itself: a drop
  // on your own window is an in-window reorder, not a reattach.
  const auto target = ResolveDropTarget(400, 300, /*self_pid=*/100, instances);
  Expect(!target.has_value(), "dropping on your own window should not resolve a reattach target");
}

void TestResolveDropTargetSkipsUnpublishedGeometry() {
  const std::vector<ControlInstanceDescriptor> instances = {
      MakeInstance(200, 0, 0, 0, 0),  // Geometry not published yet (all-zero).
  };
  const auto target = ResolveDropTarget(0, 0, /*self_pid=*/999, instances);
  Expect(!target.has_value(), "a window with no published geometry cannot be a drop target");
}

void TestResolveDropTargetReturnsNulloptOutsideAllWindows() {
  const std::vector<ControlInstanceDescriptor> instances = {
      MakeInstance(100, 0, 0, 800, 600),
      MakeInstance(200, 900, 100, 800, 600),
  };
  const auto target = ResolveDropTarget(5000, 5000, /*self_pid=*/999, instances);
  Expect(!target.has_value(), "a point over no window should resolve no target (→ new window)");
}

void TestCurrentExecutablePathResolves() {
  const std::filesystem::path exe = platform::CurrentExecutablePath();
  Expect(!exe.empty(), "the running executable path should resolve");
  Expect(std::filesystem::exists(exe), "the resolved executable path should exist on disk");
}

void TestSpawnDetachedLaunchesWithoutBlocking() {
  // `true` exits 0 immediately; SpawnDetached must return promptly (never wait).
  const bool launched = platform::SpawnDetached({"/bin/true"});
  Expect(launched, "SpawnDetached should report a successful launch of /bin/true");
  const bool rejected_empty = platform::SpawnDetached({});
  Expect(!rejected_empty, "SpawnDetached should reject an empty argv");
}

}  // namespace

void RegisterWorkspaceDetachCoordinatorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceDetach/ResolveDropTargetHitsWindowUnderPoint",
          TestResolveDropTargetHitsWindowUnderPoint);
  AddTest(tests, "WorkspaceDetach/ResolveDropTargetSkipsSelfWindow",
          TestResolveDropTargetSkipsSelfWindow);
  AddTest(tests, "WorkspaceDetach/ResolveDropTargetSkipsUnpublishedGeometry",
          TestResolveDropTargetSkipsUnpublishedGeometry);
  AddTest(tests, "WorkspaceDetach/ResolveDropTargetReturnsNulloptOutsideAllWindows",
          TestResolveDropTargetReturnsNulloptOutsideAllWindows);
  AddTest(tests, "WorkspaceDetach/CurrentExecutablePathResolves",
          TestCurrentExecutablePathResolves);
  AddTest(tests, "WorkspaceDetach/SpawnDetachedLaunchesWithoutBlocking",
          TestSpawnDetachedLaunchesWithoutBlocking);
}

}  // namespace microide::tests
