#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/ErrorOut.h"
#include "workspace/services/SidebarService.h"

namespace microide::workspace {

namespace {

const std::string& DetectActiveLanguageId(const editor::TextViewport& viewport) {
  return viewport.language_id();
}

}

bool WorkspaceShell::DiscoverTestsForActiveBuffer(std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
  const auto provider_it =
      std::find_if(plugin_runtime_.Host().ContributedTestProviders().begin(),
                   plugin_runtime_.Host().ContributedTestProviders().end(),
                   [&](const auto& provider) { return provider.language_id == language_id; });
  if (provider_it == plugin_runtime_.Host().ContributedTestProviders().end()) {
    return util::Fail(error_message, "No test provider registered");
  }

  std::vector<plugin::PluginHost::TestCase> discovered;
  std::string provider_error;
  if (!plugin_runtime_.Host().DiscoverTests(provider_it->id, viewport->path(), &discovered,
                                            &provider_error)) {
    return util::Fail(error_message, provider_error);
  }

  test_controller_.Clear();
  context_.current_project_state.sidebar.tests.entries.clear();
  context_.current_project_state.sidebar.tests.provider_id = provider_it->id;
  context_.current_project_state.sidebar.tests.error.clear();
  for (const auto& test : discovered) {
    test_controller_.RegisterTestItem(TestItem{
        .id = test.id,
        .label = test.label,
        .file = test.file.string(),
        .line = test.line,
        .parent_id = test.parent_id,
    });
  }
  ShowTestsSidebar();
  MakeSidebarService().RefreshTests();
  return true;
}

bool WorkspaceShell::RunTests(const std::vector<std::string>& test_ids, std::string* error_message) {
  util::ClearError(error_message);
  if (context_.current_project_state.sidebar.tests.provider_id.empty()) {
    return util::Fail(error_message, "No active test provider");
  }

  std::vector<plugin::PluginHost::TestRunResult> results;
  std::string provider_error;
  if (!plugin_runtime_.Host().RunTests(context_.current_project_state.sidebar.tests.provider_id,
                                       test_ids, &results, &provider_error)) {
    return util::Fail(error_message, provider_error);
  }

  for (const auto& result : results) {
    TestResultState state = TestResultState::Queued;
    if (result.state == "passed") {
      state = TestResultState::Passed;
    } else if (result.state == "failed") {
      state = TestResultState::Failed;
    } else if (result.state == "skipped") {
      state = TestResultState::Skipped;
    } else if (result.state == "errored") {
      state = TestResultState::Errored;
    } else if (result.state == "running" || result.state == "in_progress") {
      state = TestResultState::InProgress;
    }
    test_controller_.RecordTestResult(TestResult{
        .test_id = result.test_id,
        .state = state,
        .message = result.message,
        .duration_ms = result.duration_ms,
    });
  }
  MakeSidebarService().RefreshTests();
  return true;
}

bool WorkspaceShell::RunAllDiscoveredTests(std::string* error_message) {
  std::vector<std::string> test_ids;
  for (const TestItem& item : test_controller_.TestItems()) {
    test_ids.push_back(item.id);
  }
  if (test_ids.empty()) {
    return util::Fail(error_message, "No discovered tests");
  }
  return RunTests(test_ids, error_message);
}

}
