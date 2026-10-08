#include "TestRunner.h"
#include "TestSupport.h"

#include <vector>

// The kernel's test binary (TD-2026-09-22-303). It links microide_kernel and
// nothing else — no SDL, no shell, no Lua — which is the same link unit
// microide-server is built from, so "the kernel's tests run without a display"
// is a property of this binary rather than a claim. A test TU that reaches for
// the shell fails to LINK here; move it to microide_tests instead of adding the
// dependency.

namespace microide::tests {

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests);
void RegisterTestSupportTests(std::vector<TestCase>& tests);
void RegisterCompareModelPropertyTests(std::vector<TestCase>& tests);
void RegisterCompareModelTests(std::vector<TestCase>& tests);
void RegisterDirectoryTreeTests(std::vector<TestCase>& tests);
void RegisterFilesystemTests(std::vector<TestCase>& tests);
void RegisterGlobMatchTests(std::vector<TestCase>& tests);
void RegisterGitBranchOperationsTests(std::vector<TestCase>& tests);
void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests);
void RegisterProjectTraversalFilterTests(std::vector<TestCase>& tests);
void RegisterCompileCommandsLocatorTests(std::vector<TestCase>& tests);
void RegisterFileFinderTests(std::vector<TestCase>& tests);
void RegisterFileIndexTests(std::vector<TestCase>& tests);
void RegisterFileTreeOpsTests(std::vector<TestCase>& tests);
void RegisterGitBlameServiceTests(std::vector<TestCase>& tests);
void RegisterLineEditSpanTests(std::vector<TestCase>& tests);
void RegisterGitPorcelainDifferentialTests(std::vector<TestCase>& tests);
void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests);
void RegisterEditorConfigTests(std::vector<TestCase>& tests);
void RegisterMergeModelPropertyTests(std::vector<TestCase>& tests);
void RegisterPluginThreadTests(std::vector<TestCase>& tests);
void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests);
void RegisterProjectChangeTests(std::vector<TestCase>& tests);
void RegisterRegexUtilTests(std::vector<TestCase>& tests);
void RegisterRuntimePathsTests(std::vector<TestCase>& tests);
void RegisterStringUtilTests(std::vector<TestCase>& tests);
void RegisterSha256Tests(std::vector<TestCase>& tests);
void RegisterSubprocessTests(std::vector<TestCase>& tests);
void RegisterTaskExecutorTests(std::vector<TestCase>& tests);
void RegisterSerialWorkQueueTests(std::vector<TestCase>& tests);
void RegisterWakePipeTests(std::vector<TestCase>& tests);
void RegisterGenerationTests(std::vector<TestCase>& tests);
void RegisterProjectBackgroundExecutorTests(std::vector<TestCase>& tests);
void RegisterTerminalBackendTests(std::vector<TestCase>& tests);
void RegisterTerminalInvariantSweepTests(std::vector<TestCase>& tests);
void RegisterTerminalSessionTests(std::vector<TestCase>& tests);
void RegisterTerminalSearchTests(std::vector<TestCase>& tests);
void RegisterWindowPresentationTests(std::vector<TestCase>& tests);
void RegisterAsyncBufferWorkTests(std::vector<TestCase>& tests);
void RegisterFileReadServiceTests(std::vector<TestCase>& tests);
void RegisterPhase3Tests(std::vector<TestCase>& tests);
void RegisterInlineVectorTests(std::vector<TestCase>& tests);
void RegisterSmallVectorTests(std::vector<TestCase>& tests);
void RegisterFlatDedupSetTests(std::vector<TestCase>& tests);
void RegisterParseTests(std::vector<TestCase>& tests);
void RegisterJsonValueTests(std::vector<TestCase>& tests);
void RegisterJsonFormatTests(std::vector<TestCase>& tests);
void RegisterProjectFileScannerTests(std::vector<TestCase>& tests);
void RegisterTextFileIOTests(std::vector<TestCase>& tests);
void RegisterControlSocketServerTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordIoTests(std::vector<TestCase>& tests);
void RegisterCommandLineTests(std::vector<TestCase>& tests);
void RegisterFileIndexWatcherTests(std::vector<TestCase>& tests);
void RegisterFileIndexWatcherContractTests(std::vector<TestCase>& tests);
void RegisterTerminalLifecycleStressTests(std::vector<TestCase>& tests);
void RegisterPatternCacheTests(std::vector<TestCase>& tests);
void RegisterEditBatchOrderTests(std::vector<TestCase>& tests);
void RegisterTestRunnerCliTests(std::vector<TestCase>& tests);
void RegisterRemoteFrameTests(std::vector<TestCase>& tests);
void RegisterRemoteFrameTransportTests(std::vector<TestCase>& tests);
void RegisterRemotePeerTests(std::vector<TestCase>& tests);
void RegisterRemoteServerTests(std::vector<TestCase>& tests);

namespace {

void RegisterKernelTests(std::vector<TestCase>& tests) {
  RegisterRemoteFrameTests(tests);
  RegisterRemoteFrameTransportTests(tests);
  RegisterRemotePeerTests(tests);
  RegisterRemoteServerTests(tests);
  RegisterTestRunnerCliTests(tests);
  RegisterAppDirectoriesTests(tests);
  RegisterTestSupportTests(tests);
  RegisterCompareModelPropertyTests(tests);
  RegisterCompareModelTests(tests);
  RegisterDirectoryTreeTests(tests);
  RegisterFilesystemTests(tests);
  RegisterGlobMatchTests(tests);
  RegisterGitBranchOperationsTests(tests);
  RegisterIgnoreMatcherTests(tests);
  RegisterProjectTraversalFilterTests(tests);
  RegisterCompileCommandsLocatorTests(tests);
  RegisterFileFinderTests(tests);
  RegisterFileIndexTests(tests);
  RegisterAsyncBufferWorkTests(tests);
  RegisterFileReadServiceTests(tests);
  RegisterPluginThreadTests(tests);
  RegisterInlineVectorTests(tests);
  RegisterSmallVectorTests(tests);
  RegisterFlatDedupSetTests(tests);
  RegisterParseTests(tests);
  RegisterJsonValueTests(tests);
  RegisterJsonFormatTests(tests);
  RegisterProjectFileScannerTests(tests);
  RegisterTextFileIOTests(tests);
  RegisterControlSocketServerTests(tests);
  RegisterPersistedRecordIoTests(tests);
  RegisterProjectSearchServiceTests(tests);
  RegisterGitBlameServiceTests(tests);
  RegisterTerminalBackendTests(tests);
  RegisterTerminalLifecycleStressTests(tests);
  RegisterTerminalInvariantSweepTests(tests);
  RegisterTerminalSessionTests(tests);
  RegisterTerminalSearchTests(tests);
  RegisterRegexUtilTests(tests);
  RegisterRuntimePathsTests(tests);
  RegisterStringUtilTests(tests);
  RegisterSha256Tests(tests);
  RegisterSubprocessTests(tests);
  RegisterTaskExecutorTests(tests);
  RegisterSerialWorkQueueTests(tests);
  RegisterWakePipeTests(tests);
  RegisterGenerationTests(tests);
  RegisterProjectBackgroundExecutorTests(tests);
  RegisterWindowPresentationTests(tests);
  RegisterLineEditSpanTests(tests);
  RegisterGitPorcelainDifferentialTests(tests);
  RegisterGitRepositoryStateTests(tests);
  RegisterEditorConfigTests(tests);
  RegisterMergeModelPropertyTests(tests);
  RegisterFileTreeOpsTests(tests);
  RegisterPhase3Tests(tests);
  RegisterCommandLineTests(tests);
  RegisterFileIndexWatcherTests(tests);
  RegisterFileIndexWatcherContractTests(tests);
  RegisterProjectChangeTests(tests);
  RegisterPatternCacheTests(tests);
  RegisterEditBatchOrderTests(tests);
}

}  // namespace

}  // namespace microide::tests

int main(int argc, char** argv) {
  microide::tests::TestSuiteConfig config;
  config.binary_name = "microide_kernel_tests";
  config.register_tests = &microide::tests::RegisterKernelTests;
  return microide::tests::RunTestSuite(argc, argv, config);
}
