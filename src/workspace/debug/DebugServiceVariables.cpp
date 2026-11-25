#include "workspace/debug/DebugService.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/DebugTrace.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

void DebugService::FocusFrame(int frame_id) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  util::DebugTrace::Note("locals", "focus-frame request-scopes frame",
                         static_cast<long long>(frame_id));
  CurrentProjectState().debug_hover.Clear();
  CurrentProjectState().debug_variables.BeginFrame(frame_id);
  const std::uint64_t generation = frame_generation_.bump();
  session->RequestScopes(frame_id, [this, generation](std::vector<dap_protocol::DapScope> scopes) {
    if (!frame_generation_.is_current(generation)) {
      return;
    }
    DebugVariablesModel& model = CurrentProjectState().debug_variables;
    for (const DebugValueTree::ChildFetch& fetch : model.ApplyScopes(scopes)) {
      FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
    }
    util::InvokeIfSet(operations_.request_frame);
  });
  EvaluateWatches(frame_id);
}

void DebugService::ToggleVariableRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_variables.ToggleRow(row);
  util::DebugTrace::Note("locals", "toggle-row ref", static_cast<long long>(fetch.reference),
                         static_cast<long long>(fetch.start));
  if (fetch.reference > 0) {
    FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
  }
  util::InvokeIfSet(operations_.request_frame);
}

template <typename Model>
void DebugService::FetchTreeChildrenPage(Model ProjectWorkspaceState::*model_member,
                                         util::Generation& generation,
                                         int reference,
                                         int start,
                                         int count) {
  if (reference <= 0) {
    return;
  }
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    (CurrentProjectState().*model_member).MarkChildrenError(reference);
    return;
  }
  const std::uint64_t token = generation.current();
  session->RequestVariables(
      reference, start, count,
      [this, model_member, &generation, reference, start, token](
          bool ok, std::vector<dap_protocol::DapVariable> variables) {
        if (!generation.is_current(token)) {
          return;
        }
        if (ok) {
          for (const DebugValueTree::ChildFetch& fetch :
               (CurrentProjectState().*model_member).ApplyVariables(reference, variables, start)) {
            FetchTreeChildrenPage(model_member, generation, fetch.reference, fetch.start,
                                  fetch.count);
          }
        } else {
          (CurrentProjectState().*model_member).MarkChildrenError(reference);
        }
        util::InvokeIfSet(operations_.request_frame);
      });
}

void DebugService::FetchVariablesPage(int reference, int start, int count) {
  FetchTreeChildrenPage(&ProjectWorkspaceState::debug_variables, frame_generation_, reference,
                        start, count);
}

void DebugService::AppendConsoleLine(int session_id, const std::string& label,
                                     const std::string& text) {
  if (!operations_.append_console_output) {
    return;
  }
  dap_protocol::DapOutputEvent event;
  event.category = "console";
  event.output = text + "\n";
  operations_.append_console_output(session_id, label, event);
}

bool DebugService::EvaluateRepl(const std::string& expression) {
  if (expression.empty()) {
    return false;
  }
  DapManager& manager = CurrentDapManager();
  DebugSession* session = manager.ActiveSession();
  if (session == nullptr) {
    return false;
  }
  const int session_id = manager.ActiveSessionId();
  const std::string label = manager.SessionLabel(session_id);
  AppendConsoleLine(session_id, label, "> " + expression);
  util::InvokeIfSet(operations_.show_debug_console, session_id, label);
  session->RequestEvaluate(
      expression, FocusedFrameId(), "repl",
      [this, session_id, label](bool ok, dap_protocol::DapEvaluateResult result) {
        if (!ok) {
          AppendConsoleLine(session_id, label, "error: could not evaluate expression");
          util::InvokeIfSet(operations_.request_frame);
          return;
        }
        std::string line = result.result.empty() ? std::string("(no value)") : result.result;
        if (!result.type.empty()) {
          line += "  : " + result.type;
        }
        AppendConsoleLine(session_id, label, line);
        if (result.variables_reference > 0) {
          if (DebugSession* child_session = CurrentDapManager().SessionById(session_id);
              child_session != nullptr) {
            child_session->RequestVariables(
                result.variables_reference, 0, DebugValueTree::kChildPageSize,
                [this, session_id, label](bool ok, std::vector<dap_protocol::DapVariable> variables) {
                  if (!ok) {
                    return;
                  }
                  for (const dap_protocol::DapVariable& variable : variables) {
                    AppendConsoleLine(session_id, label, "    " + variable.name + ": " +
                                                             variable.value);
                  }
                  util::InvokeIfSet(operations_.request_frame);
                });
          }
        }
        util::InvokeIfSet(operations_.request_frame);
      });
  util::InvokeIfSet(operations_.request_frame);
  return true;
}

int DebugService::FocusedFrameId() const {
  const DebugStackFrameView* frame = CurrentProjectState().debug_execution.FocusedFrame();
  return frame != nullptr ? frame->id : -1;
}

void DebugService::EvaluateWatches(int frame_id) {
  DebugWatchModel& watch = CurrentProjectState().debug_watch;
  if (watch.NeedsPlaceholderRebuild()) {
    watch.BeginEvaluation();
  }
  const std::uint64_t generation = watch_generation_.bump();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session != nullptr) {
    const std::vector<std::string>& expressions = watch.Expressions();
    for (std::size_t i = 0; i < expressions.size(); ++i) {
      session->RequestEvaluate(
          expressions[i], frame_id, "watch",
          [this, i, generation](bool ok, dap_protocol::DapEvaluateResult result) {
            if (!watch_generation_.is_current(generation)) {
              return;
            }
            if (ok) {
              CurrentProjectState().debug_watch.ApplyEvaluate(i, result);
            }
            util::InvokeIfSet(operations_.request_frame);
          });
    }
  }
  util::InvokeIfSet(operations_.request_frame);
}

std::size_t DebugService::AddWatch(std::string expression) {
  const std::size_t index = CurrentProjectState().debug_watch.AddExpression(std::move(expression));
  EvaluateWatches(FocusedFrameId());
  return index;
}

void DebugService::EditWatch(std::size_t index, std::string expression) {
  CurrentProjectState().debug_watch.EditExpression(index, std::move(expression));
  EvaluateWatches(FocusedFrameId());
}

void DebugService::RemoveWatch(std::size_t index) {
  CurrentProjectState().debug_watch.RemoveExpression(index);
  EvaluateWatches(FocusedFrameId());
}

void DebugService::FetchWatchChildren(int reference, int start, int count) {
  FetchTreeChildrenPage(&ProjectWorkspaceState::debug_watch, watch_generation_, reference, start,
                        count);
}

void DebugService::ToggleWatchRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_watch.ToggleRow(row);
  if (fetch.reference > 0) {
    FetchWatchChildren(fetch.reference, fetch.start, fetch.count);
  }
  util::InvokeIfSet(operations_.request_frame);
}

DebugValueTreePane& DebugService::EditablePane(EditableTree tree) {
  ProjectWorkspaceState& state = CurrentProjectState();
  return tree == EditableTree::Variables ? static_cast<DebugValueTreePane&>(state.debug_variables)
                                         : static_cast<DebugValueTreePane&>(state.debug_watch);
}

util::Generation& DebugService::EditableGeneration(EditableTree tree) {
  return tree == EditableTree::Variables ? frame_generation_ : watch_generation_;
}

void DebugService::BeginTreeEdit(EditableTree tree, std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (EditablePane(tree).BeginEdit(row)) {
    util::InvokeIfSet(operations_.request_frame);
  }
}

void DebugService::CommitTreeEdit(EditableTree tree) {
  DebugValueTreePane& pane = EditablePane(tree);
  const auto target = pane.EditTargetForCommit();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (target.has_value() && session != nullptr) {
    const std::string value = pane.EditBuffer().text();
    const std::uint32_t node_id = target->node_id;
    const std::uint64_t generation = EditableGeneration(tree).current();
    session->SetVariable(
        target->container_reference, target->name, value,
        [this, tree, node_id, generation](bool ok, dap_protocol::DapSetVariableResult result) {
          if (!EditableGeneration(tree).is_current(generation)) {
            return;
          }
          if (ok) {
            EditablePane(tree).ApplySetVariable(node_id, result);
          }
          util::InvokeIfSet(operations_.request_frame);
        });
  }
  pane.CancelEdit();
  util::InvokeIfSet(operations_.request_frame);
}

void DebugService::CancelTreeEdit(EditableTree tree) {
  EditablePane(tree).CancelEdit();
  util::InvokeIfSet(operations_.request_frame);
}

void DebugService::EvaluateHover(int frame_id, const std::string& expression) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  DebugHoverModel& hover = CurrentProjectState().debug_hover;
  if (hover.Classify(frame_id, expression) != DebugHoverModel::Lookup::Miss) {
    util::DebugTrace::Note("hover", "evaluate suppressed (not a miss) expr", expression,
                           static_cast<long long>(frame_id));
    return;
  }
  util::DebugTrace::Note("hover", "evaluate begin expr", expression,
                         static_cast<long long>(frame_id));
  const std::uint64_t generation = hover.Begin(frame_id, expression);
  session->RequestEvaluate(
      expression, frame_id, "hover",
      [this, generation](bool ok, dap_protocol::DapEvaluateResult result) {
        DebugHoverModel& model = CurrentProjectState().debug_hover;
        if (ok) {
          util::DebugTrace::Note("hover", "evaluate resolved value", result.result);
          model.Resolve(generation, std::move(result.result), std::move(result.type));
        } else {
          util::DebugTrace::Note("hover", "evaluate FAILED");
          model.Fail(generation);
        }
        util::InvokeIfSet(operations_.queue_editor_hover_refresh);
        util::InvokeIfSet(operations_.request_frame);
      });
}


bool DebugService::SupportsStepBack() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->Client().Capabilities().supports_step_back;
}

}
