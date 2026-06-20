#include "workspace/DebugService.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/DebugTrace.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

void DebugService::FocusFrame(int frame_id) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  // Hover values are frame-scoped: a frame switch must not serve a value (or let a
  // still-in-flight request resolve) keyed to the previously focused frame.
  util::DebugTrace::Note("locals", "focus-frame request-scopes frame",
                         static_cast<long long>(frame_id));
  CurrentProjectState().debug_hover.Clear();
  CurrentProjectState().debug_variables.BeginFrame(frame_id);
  // A new frame context: in-flight scopes/variables/setVariable for the prior frame
  // (whose variablesReference values the adapter may now recycle) must not apply.
  const std::uint64_t generation = ++frame_generation_;
  session->RequestScopes(frame_id, [this, generation](std::vector<dap_protocol::DapScope> scopes) {
    if (generation != frame_generation_) {
      return;  // a newer frame focus / stop / clear superseded this request
    }
    DebugVariablesModel& model = CurrentProjectState().debug_variables;
    // ApplyScopes re-expands the scopes the user had open before this stop and
    // returns the bounded fetches needed to repopulate them; issue each (children
    // that arrive cascade further) so a step does not collapse the tree.
    for (const DebugValueTree::ChildFetch& fetch : model.ApplyScopes(scopes)) {
      FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
    }
    // Scopes are installed collapsed; their variables are fetched lazily when the
    // user expands a row (ToggleVariableRow). We deliberately do NOT auto-expand
    // on every stop: a stop frequently lands where in-scope locals are not yet
    // constructed (function entry, `stopAtBeginningOfMainSubprogram`, any line
    // before a declaration), and formatting that uninitialized memory can make a
    // single-threaded adapter spin for an unbounded time (gdb's STL
    // pretty-printers loop on garbage container pointers). Because the adapter
    // serializes requests, that spin would block the *next* execution-control
    // request (continue/step/pause) — i.e. stepping would silently stop working.
    // Lazy expansion keeps the stop cheap and execution control responsive.
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  });
  // Re-evaluate watch expressions in the (now focused) frame's scope. Runs on
  // every stop (top frame) and on a call-stack frame switch.
  EvaluateWatches(frame_id);
}

void DebugService::ToggleVariableRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_variables.ToggleRow(row);
  util::DebugTrace::Note("locals", "toggle-row ref", static_cast<long long>(fetch.reference),
                         static_cast<long long>(fetch.start));
  if (fetch.reference > 0) {
    FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::FetchVariablesPage(int reference, int start, int count) {
  if (reference <= 0) {
    return;
  }
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    // No live session to service the fetch: clear the loading state so the row
    // does not spin forever.
    CurrentProjectState().debug_variables.MarkChildrenError(reference);
    return;
  }
  // Bind this fetch to the current frame generation. A page that returns after a
  // frame switch / stop / clear is dropped — the adapter recycles
  // variablesReference values, so applying it could attach children to an unrelated
  // node of the new frame.
  const std::uint64_t generation = frame_generation_;
  session->RequestVariables(
      reference, start, count,
      [this, reference, start, generation](bool ok,
                                           std::vector<dap_protocol::DapVariable> variables) {
        if (generation != frame_generation_) {
          return;
        }
        if (ok) {
          // Restoring expansion can cascade: applying a page may re-expand
          // descendants the user had open, whose own pages we fetch in turn.
          for (const DebugValueTree::ChildFetch& fetch :
               CurrentProjectState().debug_variables.ApplyVariables(reference, variables, start)) {
            FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
          }
        } else {
          CurrentProjectState().debug_variables.MarkChildrenError(reference);
        }
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      });
}

void DebugService::BeginVariableEdit(std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  // Gate edit entry on the adapter capability so an unsupported adapter never
  // shows an edit field that cannot commit.
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (CurrentProjectState().debug_variables.BeginEdit(row) &&
      operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CommitVariableEdit() {
  DebugVariablesModel& model = CurrentProjectState().debug_variables;
  const auto target = model.EditTargetForCommit();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (target.has_value() && session != nullptr) {
    const std::string value = model.EditBuffer().text();
    const std::uint32_t node_id = target->node_id;
    const std::uint64_t generation = frame_generation_;
    session->SetVariable(
        target->container_reference, target->name, value,
        [this, node_id, generation](bool ok, dap_protocol::DapSetVariableResult result) {
          // Drop a response that lands after a frame switch / stop: node ids are
          // globally monotonic so a stale id can no longer alias a live node, but
          // guarding here also avoids a pointless rebuild of a superseded tree.
          if (generation != frame_generation_) {
            return;
          }
          // Authoritative: apply only the adapter's returned (possibly normalized)
          // value, never the raw typed text.
          if (ok) {
            CurrentProjectState().debug_variables.ApplySetVariable(node_id, result);
          }
          if (operations_.request_debug_pane_redraw) {
            operations_.request_debug_pane_redraw();
          }
        });
  }
  // Leave edit mode immediately; the row's value updates when the response lands.
  model.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CancelVariableEdit() {
  CurrentProjectState().debug_variables.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
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
  std::string label;
  for (const DapSessionInfo& info : manager.Sessions()) {
    if (info.id == session_id) {
      label = info.name;
      break;
    }
  }
  // Echo the typed expression, then surface the console so the result is visible.
  AppendConsoleLine(session_id, label, "> " + expression);
  if (operations_.show_debug_console) {
    operations_.show_debug_console(session_id, label);
  }
  // Frame 0 when running (no stopped frame); the adapter evaluates in global scope.
  session->RequestEvaluate(
      expression, FocusedFrameId(), "repl",
      [this, session_id, label](bool ok, dap_protocol::DapEvaluateResult result) {
        if (!ok) {
          AppendConsoleLine(session_id, label, "error: could not evaluate expression");
          if (operations_.request_debug_pane_redraw) {
            operations_.request_debug_pane_redraw();
          }
          return;
        }
        std::string line = result.result.empty() ? std::string("(no value)") : result.result;
        if (!result.type.empty()) {
          line += "  : " + result.type;
        }
        AppendConsoleLine(session_id, label, line);
        // Structured result: expand one level of children inline as indented
        // `name: value` lines so a dict/object prints its fields (Phase 10). Deep
        // lazy expansion would need a tree surface; the console is text, so a single
        // eager level covers the common "print this object" case.
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
                  if (operations_.request_debug_pane_redraw) {
                    operations_.request_debug_pane_redraw();
                  }
                });
          }
        }
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      });
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
  return true;
}

int DebugService::FocusedFrameId() const {
  const DebugStackFrameView* frame = CurrentProjectState().debug_execution.FocusedFrame();
  // -1 (not 0) when no frame is focused: frame id 0 is gdb's valid top frame, so a
  // 0 here would wrongly request evaluation in a real frame. -1 means "no frame"
  // and MakeEvaluateArguments omits frameId (global-scope evaluate while running).
  return frame != nullptr ? frame->id : -1;
}

void DebugService::EvaluateWatches(int frame_id) {
  DebugWatchModel& watch = CurrentProjectState().debug_watch;
  // Rebuild one placeholder root per expression so rows stay stable/ordered while
  // the (async) results stream in by index.
  watch.BeginEvaluation();
  // Each pass clears + rebuilds expression_root_ids_; a result from a prior pass
  // (the user added/removed/edited a watch, or stepped) would otherwise land on a
  // reshuffled index and stamp a value onto the wrong expression. Bind every
  // evaluate to this pass's generation and drop late ones.
  const std::uint64_t generation = ++watch_generation_;
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session != nullptr) {
    const std::vector<std::string> expressions = watch.Expressions();
    for (std::size_t i = 0; i < expressions.size(); ++i) {
      session->RequestEvaluate(
          expressions[i], frame_id, "watch",
          [this, i, generation](bool ok, dap_protocol::DapEvaluateResult result) {
            if (generation != watch_generation_) {
              return;
            }
            if (ok) {
              CurrentProjectState().debug_watch.ApplyEvaluate(i, result);
            }
            if (operations_.request_debug_pane_redraw) {
              operations_.request_debug_pane_redraw();
            }
          });
    }
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
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

void DebugService::ToggleWatchRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_watch.ToggleRow(row);
  if (fetch.reference > 0) {
    DebugSession* session = CurrentDapManager().ActiveSession();
    if (session != nullptr) {
      const int reference = fetch.reference;
      const int start = fetch.start;
      // A re-evaluation pass (add/remove/edit/step) clears the watch tree and the
      // adapter recycles references; drop a child page that returns after one.
      const std::uint64_t generation = watch_generation_;
      session->RequestVariables(
          reference, start, fetch.count,
          [this, reference, start, generation](bool ok,
                                               std::vector<dap_protocol::DapVariable> variables) {
            if (generation != watch_generation_) {
              return;
            }
            if (ok) {
              CurrentProjectState().debug_watch.ApplyVariables(reference, variables, start);
            } else {
              CurrentProjectState().debug_watch.MarkChildrenError(reference);
            }
            if (operations_.request_debug_pane_redraw) {
              operations_.request_debug_pane_redraw();
            }
          });
    } else {
      CurrentProjectState().debug_watch.MarkChildrenError(fetch.reference);
    }
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::BeginWatchEdit(std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (CurrentProjectState().debug_watch.BeginEdit(row) && operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CommitWatchEdit() {
  DebugWatchModel& model = CurrentProjectState().debug_watch;
  const auto target = model.EditTargetForCommit();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (target.has_value() && session != nullptr) {
    const std::string value = model.EditBuffer().text();
    const std::uint32_t node_id = target->node_id;
    const std::uint64_t generation = watch_generation_;
    session->SetVariable(target->container_reference, target->name, value,
                         [this, node_id, generation](
                             bool ok, dap_protocol::DapSetVariableResult result) {
                           if (generation != watch_generation_) {
                             return;
                           }
                           if (ok) {
                             CurrentProjectState().debug_watch.ApplySetVariable(node_id, result);
                           }
                           if (operations_.request_debug_pane_redraw) {
                             operations_.request_debug_pane_redraw();
                           }
                         });
  }
  model.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CancelWatchEdit() {
  CurrentProjectState().debug_watch.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::EvaluateHover(int frame_id, const std::string& expression) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  DebugHoverModel& hover = CurrentProjectState().debug_hover;
  // Dedup: the per-frame hover trigger re-asks every mouse-move; only the first
  // ask for a given (frame, expression) issues a request. Pending/Resolved/Failed
  // for the same key are all served (or suppressed) without re-issuing.
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
        // Queue a hover refresh first so the redraw re-resolves the now-cached
        // value into an active popup (mirrors ClearDiagnosticsForPath). Without
        // this the value sits in the cache and the tooltip never appears.
        if (operations_.queue_editor_hover_refresh) {
          operations_.queue_editor_hover_refresh();
        }
        if (operations_.request_editor_redraw) {
          operations_.request_editor_redraw();
        }
      });
}

bool DebugService::SupportsEvaluateForHovers() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->Client().Capabilities().supports_evaluate_for_hovers;
}

bool DebugService::SupportsStepBack() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->Client().Capabilities().supports_step_back;
}

}  // namespace microide::workspace
