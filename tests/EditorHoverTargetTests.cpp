#include "TestSupport.h"

#include "editor/DiagnosticsStore.h"
#include "editor/EditorViewRenderer.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <utility>
#include <cmath>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::editor::Diagnostic;
using microide::editor::DiagnosticSeverity;
using microide::workspace::WorkspaceShell;
using HoverTarget = decltype(WorkspaceShellTestAccess::EditorHoverTargetAtPosition(
    std::declval<const WorkspaceShell&>(), 0.0f, 0.0f))::value_type;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;

std::string FixtureText() {
  std::string text;
  for (int i = 0; i < 60; ++i) {
    text += "\tconst auto value_" + std::to_string(i) +
            " = compute(\"ünïcødé — a wide string that runs well past the pane\", " +
            std::to_string(i) + ");\n";
  }
  return text;
}

std::filesystem::path MakeProject(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, FixtureText());
  return source;
}

void OpenShell(WorkspaceShell& shell, const std::filesystem::path& root,
               const std::filesystem::path& source) {
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
}

void SeedDiagnostics(WorkspaceShell& shell, const std::filesystem::path& source,
                     std::size_t line_count) {
  std::vector<Diagnostic> diagnostics;
  for (std::size_t line = 0; line < line_count; ++line) {
    Diagnostic diagnostic;
    diagnostic.range.start = {line, 4};
    diagnostic.range.end = {line, 18};
    diagnostic.severity = (line % 3 == 0)   ? DiagnosticSeverity::Error
                          : (line % 3 == 1) ? DiagnosticSeverity::Warning
                                            : DiagnosticSeverity::Info;
    diagnostic.message = "diagnostic on line " + std::to_string(line);
    diagnostics.push_back(std::move(diagnostic));
  }
  Expect(WorkspaceShellTestAccess::SetDiagnosticsForTest(shell, source, std::move(diagnostics)),
         "seeding diagnostics should take");
}

struct SweepCounts {
  int queries = 0;
  int hits = 0;
  int diagnostic_hits = 0;
};

SweepCounts SweepPane(const WorkspaceShell& shell, const SDL_FRect& pane, float step,
                      float line_height) {
  SweepCounts counts;
  for (float y = pane.y - step; y < pane.y + pane.h + step; y += step) {
    for (float x = pane.x - step; x < pane.x + pane.w + step; x += step) {
      const auto target = WorkspaceShellTestAccess::EditorHoverTargetAtPosition(shell, x, y);
      ++counts.queries;

      const auto again = WorkspaceShellTestAccess::EditorHoverTargetAtPosition(shell, x, y);
      Expect(target.has_value() == again.has_value(),
             "the hover hit test must be stable: two queries at one point disagreed");
      if (target.has_value() && again.has_value()) {
        Expect(target->kind == again->kind,
               "the hover hit test must resolve the same kind at the same point");
      }

      if (!target.has_value()) {
        continue;
      }
      ++counts.hits;

      Expect(target->anchor_rect.w > 0.0f && target->anchor_rect.h > 0.0f,
             "a resolved hover target must have a positive anchor rect");
      Expect(std::isfinite(target->anchor_rect.x) && std::isfinite(target->anchor_rect.y),
             "a resolved hover target must have a finite anchor origin");

      if (target->kind == HoverTarget::Kind::Diagnostic) {
        ++counts.diagnostic_hits;
        Expect(target->diagnostic.has_value(),
               "a Diagnostic hover target must carry the diagnostic it resolved");
        Expect(!target->diagnostic->message.empty(),
               "a resolved diagnostic should carry its message");
        Expect(std::abs((target->anchor_rect.y + target->anchor_rect.h * 0.5f) - y) <=
                   line_height + 1.0f,
               "a diagnostic anchor should sit on the row the pointer is on");
      }
    }
  }
  return counts;
}

void TestHoverResolvesDiagnosticsAcrossTheGrid() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  WorkspaceShell shell;
  OpenShell(shell, root, source);
  SeedDiagnostics(shell, source, 60);

  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  Expect(pane.w > 0.0f && pane.h > 0.0f, "the editor pane must have geometry to sweep");

  const float line_height = WorkspaceShellTestAccess::ActiveEditorMetrics(shell).line_height;
  const SweepCounts counts = SweepPane(shell, pane, 7.0f, line_height);
  Expect(counts.queries > 1000, "the sweep must actually cover the pane");
  Expect(counts.diagnostic_hits > 50,
         "the sweep must land on diagnostics often enough to be evidence");
}

void TestHoverResolvesNothingOutsideTheTextGrid() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  WorkspaceShell shell;
  OpenShell(shell, root, source);
  SeedDiagnostics(shell, source, 60);

  const float far_out = static_cast<float>(kWindowWidth + kWindowHeight);
  const float probes[][2] = {
      {-1.0f, -1.0f},         {-far_out, -far_out},   {far_out, far_out},
      {-1.0f, kWindowHeight / 2.0f},                  {kWindowWidth + 1.0f, kWindowHeight / 2.0f},
      {kWindowWidth / 2.0f, -1.0f},                   {kWindowWidth / 2.0f, kWindowHeight + 1.0f},
      {0.0f, 0.0f},
  };
  for (const auto& probe : probes) {
    Expect(!WorkspaceShellTestAccess::EditorHoverTargetAtPosition(shell, probe[0], probe[1])
                .has_value(),
           "a pointer outside the text grid must not resolve a hover target");
    Expect(!WorkspaceShellTestAccess::CodeLensAtPosition(shell, probe[0], probe[1]).has_value(),
           "a pointer outside the text grid must not resolve a code lens");
  }
}

void TestHoverSurvivesSoftWrapAndScroll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  WorkspaceShell shell;
  OpenShell(shell, root, source);
  SeedDiagnostics(shell, source, 60);

  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const float line_height = WorkspaceShellTestAccess::ActiveEditorMetrics(shell).line_height;
  int total_hits = 0;
  for (const bool soft_wrap : {false, true}) {
    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(viewport != nullptr, "the fixture must have an active viewport");
    viewport->SetSoftWrap(soft_wrap);
    for (const std::size_t scroll : {std::size_t{0}, std::size_t{5}, std::size_t{40}}) {
      viewport->SetScrollLine(scroll);
      total_hits += SweepPane(shell, pane, 11.0f, line_height).diagnostic_hits;
    }
  }
  Expect(total_hits > 50,
         "wrapped and scrolled sweeps must still land on diagnostics, or this proves nothing");
}

void TestHoverSurvivesADegeneratePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  for (const int width : {30, 90, 200}) {
    for (const int height : {30, 80}) {
      WorkspaceShell shell;
      WorkspaceShellTestAccess::SetProjectRoot(shell, root);
      WorkspaceShellTestAccess::SetWindowSize(shell, width, height);
      WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
      SeedDiagnostics(shell, source, 60);

      for (float y = -4.0f; y < static_cast<float>(height) + 4.0f; y += 3.0f) {
        for (float x = -4.0f; x < static_cast<float>(width) + 4.0f; x += 3.0f) {
          const auto target = WorkspaceShellTestAccess::EditorHoverTargetAtPosition(shell, x, y);
          if (target.has_value()) {
            Expect(target->anchor_rect.w > 0.0f && target->anchor_rect.h > 0.0f,
                   "even in a degenerate pane a resolved target needs a real anchor");
          }
          WorkspaceShellTestAccess::CodeLensAtPosition(shell, x, y);
        }
      }
    }
  }
}

void TestHoverResolvesNothingWithNoDiagnostics() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  WorkspaceShell shell;
  OpenShell(shell, root, source);

  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const float line_height = WorkspaceShellTestAccess::ActiveEditorMetrics(shell).line_height;
  const SweepCounts counts = SweepPane(shell, pane, 7.0f, line_height);
  Expect(counts.queries > 1000, "the sweep must actually cover the pane");
  Expect(counts.diagnostic_hits == 0,
         "with no diagnostics published nothing may resolve as a diagnostic hover");
}

}

void RegisterEditorHoverTargetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorHoverTarget/ResolvesDiagnosticsAcrossTheGrid",
          TestHoverResolvesDiagnosticsAcrossTheGrid);
  AddTest(tests, "EditorHoverTarget/ResolvesNothingOutsideTheTextGrid",
          TestHoverResolvesNothingOutsideTheTextGrid);
  AddTest(tests, "EditorHoverTarget/SurvivesSoftWrapAndScroll",
          TestHoverSurvivesSoftWrapAndScroll);
  AddTest(tests, "EditorHoverTarget/SurvivesADegeneratePane", TestHoverSurvivesADegeneratePane);
  AddTest(tests, "EditorHoverTarget/ResolvesNothingWithNoDiagnostics",
          TestHoverResolvesNothingWithNoDiagnostics);
}

}
