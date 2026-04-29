#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::workspace {

namespace {

std::string DetectActiveLanguageId(const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

}  // namespace

bool WorkspaceShell::DiscoverTestsForActiveBuffer(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
  const auto provider_it =
      std::find_if(plugin_runtime_.Host().ContributedTestProviders().begin(),
                   plugin_runtime_.Host().ContributedTestProviders().end(),
                   [&](const auto& provider) { return provider.language_id == language_id; });
  if (provider_it == plugin_runtime_.Host().ContributedTestProviders().end()) {
    if (error_message != nullptr) {
      *error_message = "No test provider registered";
    }
    return false;
  }

  std::vector<plugin::PluginHost::TestCase> discovered;
  std::string provider_error;
  if (!plugin_runtime_.Host().DiscoverTests(provider_it->id, viewport->path(), &discovered,
                                            &provider_error)) {
    if (error_message != nullptr) {
      *error_message = provider_error;
    }
    return false;
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
  RefreshTestsSidebar();
  return true;
}

bool WorkspaceShell::RunTests(const std::vector<std::string>& test_ids, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (context_.current_project_state.sidebar.tests.provider_id.empty()) {
    if (error_message != nullptr) {
      *error_message = "No active test provider";
    }
    return false;
  }

  std::vector<plugin::PluginHost::TestRunResult> results;
  std::string provider_error;
  if (!plugin_runtime_.Host().RunTests(context_.current_project_state.sidebar.tests.provider_id,
                                       test_ids, &results, &provider_error)) {
    if (error_message != nullptr) {
      *error_message = provider_error;
    }
    return false;
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
  RefreshTestsSidebar();
  return true;
}

bool WorkspaceShell::RunAllDiscoveredTests(std::string* error_message) {
  std::vector<std::string> test_ids;
  for (const TestItem& item : test_controller_.TestItems()) {
    test_ids.push_back(item.id);
  }
  if (test_ids.empty()) {
    if (error_message != nullptr) {
      *error_message = "No discovered tests";
    }
    return false;
  }
  return RunTests(test_ids, error_message);
}

}  // namespace microide::workspace
