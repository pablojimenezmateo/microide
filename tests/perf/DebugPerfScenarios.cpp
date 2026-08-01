// Performance scenarios for the debugger / DAP subsystem.
//
// These bring the debug subsystem to perf-coverage parity with the editor,
// which previously had ~10 smoke + ~13 gated scenarios while debug had none.
//
// Two flavors:
//   * Pure-unit micro-benchmarks construct the real data structures directly and
//     measure the hot paths the render/step loop consumes (value-tree rebuild is
//     literally the render-ready row list the bottom-panel render TU draws). They
//     ignore the app driver, so they are deterministic and allocation-stable —
//     which is why the six of them are now promoted to gated
//     (`smoke = true, baseline_gated = true`) with committed reference-runner
//     baselines, per the promotion path in dev-docs/performance/perf-harness.md.
//   * One live mock-adapter session scenario drives a real DebugService stack
//     (DapManager + DebugSession + a Python DAP adapter) and measures the
//     stop -> stackTrace -> scopes -> variables latency end to end. It is
//     inherently noisier (subprocess + IPC) and is skipped gracefully when the
//     platform has no python3, so it stays advisory
//     (`smoke = false, baseline_gated = false`).
#include "perf/PerfHarness.h"

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugBreakpointsModel.h"
#include "workspace/DebugPaneRegistry.h"
#include "workspace/DebugSession.h"
#include "workspace/DebugValueTree.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceDapManager.h"
#include "workspace/WorkspaceLayout.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests::perf {
namespace {

namespace codec = microide::workspace::dap_protocol;
using microide::workspace::DapManager;
using microide::workspace::DebugBreakpointsModel;
using microide::workspace::DebugSession;
using microide::workspace::DebugValueTree;
using microide::workspace::LaunchConfig;

// ---- Synthetic data builders (no adapter) ---------------------------------

// Build `count` flat scalar variables plus `structured` expandable ones. Scalars
// dominate a real Locals scope; the structured tail exercises the has-children
// classification + nested-expand bookkeeping.
std::vector<codec::DapVariable> MakeVariables(int count, int structured, int first_child_ref) {
  std::vector<codec::DapVariable> vars;
  vars.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    codec::DapVariable v;
    v.name = "v" + std::to_string(i);
    const bool is_struct = i < structured;
    if (is_struct) {
      v.value = "{...}";
      v.type = "struct S";
      v.variables_reference = first_child_ref + i;  // expandable
      v.named_variables = 4;
      v.count_reported = true;
    } else {
      v.value = std::to_string(i * 7 - 3);
      v.type = "int";
    }
    vars.push_back(std::move(v));
  }
  return vars;
}

// Build a value tree with `scopes` scopes, each expanded with `per_scope`
// children, and return it ready to rebuild/inspect. Mirrors what DebugService
// installs on a stop: AddRoot scopes, expand, ApplyVariables.
DebugValueTree BuildExpandedTree(int scopes, int per_scope) {
  DebugValueTree tree;
  int ref = 1000;
  for (int s = 0; s < scopes; ++s) {
    const int scope_ref = ref++;
    tree.AddRoot("Scope" + std::to_string(s), /*value=*/{}, /*type=*/{}, scope_ref,
                 /*is_scope=*/true, per_scope, /*total_known=*/true);
  }
  tree.Rebuild();
  // Expand each scope root and attach its children. Re-find the scope row each
  // pass since rebuilds renumber the flat list as earlier scopes expand.
  for (int s = 0; s < scopes; ++s) {
    // The s-th scope root sits at the head of the flat list among unexpanded
    // scopes; toggling it returns its bounded child fetch.
    std::size_t scope_row = 0;
    const auto& rows = tree.Rows();
    for (std::size_t r = 0; r < rows.size(); ++r) {
      if (rows[r].display_name == "Scope" + std::to_string(s)) {
        scope_row = r;
        break;
      }
    }
    const DebugValueTree::ChildFetch fetch = tree.ToggleRow(scope_row);
    if (fetch.reference != 0) {
      tree.ApplyVariables(fetch.reference, MakeVariables(per_scope, /*structured=*/4,
                                                         /*first_child_ref=*/100000 + s * 1000),
                          /*start=*/0);
    }
  }
  return tree;
}

// ---- Pure-unit scenarios ---------------------------------------------------

// Building + expanding a large variables tree is the per-stop cost: AddRoot the
// scopes, expand, attach hundreds of children, flatten.
void RunDebugValueTreeExpandLarge(ScenarioContext& context) {
  context.Measure("value_tree.build_expand", [&]() {
    for (int iter = 0; iter < 20; ++iter) {
      DebugValueTree tree = BuildExpandedTree(/*scopes=*/4, /*per_scope=*/250);
      // Touch the rows so the work is not optimized away.
      volatile std::size_t sink = tree.Rows().size();
      (void)sink;
    }
  });
}

// Rebuild() rematerializes the flat row list the render TU draws; it runs on
// every tree mutation (expand/collapse/edit). Measure it on an already-large
// expanded tree, isolated from the construction cost above.
void RunDebugValueTreeRebuild(ScenarioContext& context) {
  DebugValueTree tree = BuildExpandedTree(/*scopes=*/4, /*per_scope=*/250);
  context.Measure("value_tree.rebuild", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      tree.Rebuild();
    }
  });
}

// Paging a large indexed container: a 5000-element array streamed in 200-row
// pages via the "show more…" affordance. Bounds the per-page attach + rebuild.
void RunDebugValueTreePaging(ScenarioContext& context) {
  context.Measure("value_tree.paging", [&]() {
    DebugValueTree tree;
    const int array_ref = 2000;
    tree.AddRoot("bigArray", "[5000]", "int[5000]", array_ref, /*is_scope=*/false,
                 /*total_count=*/5000, /*total_known=*/true);
    tree.Rebuild();
    DebugValueTree::ChildFetch fetch = tree.ToggleRow(0);  // expand → first page
    int start = 0;
    while (fetch.reference != 0 && start < 5000) {
      const int page = fetch.count > 0 ? fetch.count : DebugValueTree::kChildPageSize;
      tree.ApplyVariables(fetch.reference, MakeVariables(page, /*structured=*/0,
                                                         /*first_child_ref=*/0),
                          start);
      start += page;
      // Click the trailing "show more…" row to fetch the next page.
      const auto& rows = tree.Rows();
      if (rows.empty() || !rows.back().is_show_more) {
        break;
      }
      fetch = tree.ToggleRow(rows.size() - 1);
    }
  });
}

// DAP wire encode/decode throughput over the payloads that dominate a stop: a
// large `variables` body and a deep `stackTrace` body. Build the bodies once,
// measure parse (decode) and serialize (encode) separately.
void RunDapProtocolEncodeDecode(ScenarioContext& context) {
  using microide::util::JsonArray;
  using microide::util::JsonObject;
  using microide::util::JsonValue;
  using microide::util::SerializeJson;

  // Synthesize a `variables` response body with 500 entries.
  JsonArray variables;
  variables.reserve(500);
  for (int i = 0; i < 500; ++i) {
    JsonObject v;
    v["name"] = JsonValue(std::string("var") + std::to_string(i));
    v["value"] = JsonValue(std::to_string(i * 3));
    v["type"] = JsonValue(std::string("int"));
    v["variablesReference"] = JsonValue(static_cast<std::int64_t>(i % 5 == 0 ? 9000 + i : 0));
    variables.push_back(JsonValue(std::move(v)));
  }
  JsonObject vars_body_obj;
  vars_body_obj["variables"] = JsonValue(std::move(variables));
  const JsonValue vars_body(std::move(vars_body_obj));

  // Synthesize a `stackTrace` body with 200 frames.
  JsonArray frames;
  frames.reserve(200);
  for (int i = 0; i < 200; ++i) {
    JsonObject src;
    src["name"] = JsonValue(std::string("file") + std::to_string(i) + ".cpp");
    src["path"] = JsonValue(std::string("/src/file") + std::to_string(i) + ".cpp");
    JsonObject f;
    f["id"] = JsonValue(static_cast<std::int64_t>(i));
    f["name"] = JsonValue(std::string("frame_") + std::to_string(i));
    f["line"] = JsonValue(static_cast<std::int64_t>(i + 1));
    f["column"] = JsonValue(static_cast<std::int64_t>(1));
    f["source"] = JsonValue(std::move(src));
    frames.push_back(JsonValue(std::move(f)));
  }
  JsonObject frames_body_obj;
  frames_body_obj["stackFrames"] = JsonValue(std::move(frames));
  const JsonValue frames_body(std::move(frames_body_obj));

  context.Measure("dap_protocol.decode", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto vars = codec::ParseVariables(vars_body);
      const auto fr = codec::ParseStackFrames(frames_body);
      volatile std::size_t sink = vars.size() + fr.size();
      (void)sink;
    }
  });
  context.Measure("dap_protocol.encode", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const std::string a = SerializeJson(codec::MakeRequest(iter, "variables", vars_body));
      const std::string b = SerializeJson(codec::MakeRequest(iter, "stackTrace", frames_body));
      volatile std::size_t sink = a.size() + b.size();
      (void)sink;
    }
  });
}

// DebugBreakpointsModel.Rebuild rematerializes the Breakpoints-panel row list
// (exception filters + line breakpoints + function breakpoints). Exercise it
// with many breakpoints across many files.
void RunDebugBreakpointsModelRebuild(ScenarioContext& context) {
  editor::BreakpointStore breakpoints;
  for (int file = 0; file < 20; ++file) {
    const std::filesystem::path path = "/src/file" + std::to_string(file) + ".cpp";
    for (int line = 0; line < 25; ++line) {
      breakpoints.Set(path, static_cast<std::size_t>(line * 3 + 1), /*enabled=*/true);
    }
  }
  editor::FunctionBreakpointStore functions;
  for (int i = 0; i < 50; ++i) {
    functions.Add("func_" + std::to_string(i));
  }
  DebugBreakpointsModel model;
  context.Measure("breakpoints_model.rebuild", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      model.Rebuild(breakpoints, functions);
      volatile std::size_t sink = model.RowCount();
      (void)sink;
    }
  });
}

// DebugPaneRowAtPoint is the shared row-geometry mapper hit on every click,
// hover, and cursor move in the debug pane. Sweep many points across a tall
// scrolled list to bound the per-hit cost.
void RunDebugPaneHittestGeometry(ScenarioContext& context) {
  const SDL_FRect content = microide::workspace::MakeRect(0.0f, 30.0f, 240.0f, 800.0f);
  const float text_y = 38.0f;
  const float line_height = 16.0f;
  const int visible_rows = 48;
  const std::size_t line_count = 5000;
  // The sweep is repeated rather than extended: `scroll` feeds the row mapping,
  // so widening its range would change which inputs are exercised (and clamp
  // against line_count). One sweep resolves ~40k points in about 0.1 ms, which is
  // far below what this runner can time -- the gate on it swung 0.148-0.225 ms
  // across repeat runs of one binary and sat permanently red against a 0.103 ms
  // baseline. 100 sweeps put the measurement in a range where the median means
  // something, over exactly the same input distribution.
  constexpr int kSweeps = 100;
  context.Measure("pane.hittest", [&]() {
    volatile int hits = 0;
    for (int sweep = 0; sweep < kSweeps; ++sweep) {
      for (int scroll = 0; scroll < 200; ++scroll) {
        for (int y = 30; y < 820; y += 4) {
          const auto hit = microide::workspace::DebugPaneRowAtPoint(
              content, text_y, line_height, visible_rows, scroll, line_count, /*x=*/100.0f,
              static_cast<float>(y));
          hits += hit.row_index;
        }
      }
    }
    (void)hits;
  });
}

// ---- Live mock-adapter session scenario ------------------------------------

// A compact DAP adapter (mirrors the test mock) that walks
// initialize -> initialized -> launch -> configurationDone -> stopped, then
// answers threads / stackTrace / scopes / variables. The variable count is
// argv[1] so the scenario can scale the stop payload.
const char* DebugPerfAdapterSource() {
  return R"py(import json
import sys

var_count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
seq = 0

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    if not body:
        return None
    return json.loads(body)

def send(msg):
    global seq
    seq += 1
    msg["seq"] = seq
    data = json.dumps(msg).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(data))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

def respond(req, body=None, success=True):
    send({"type": "response", "request_seq": req["seq"], "success": success,
          "command": req["command"], "body": body or {}})

def event(name, body=None):
    send({"type": "event", "event": name, "body": body or {}})

while True:
    req = read_message()
    if req is None:
        break
    cmd = req.get("command")
    if cmd == "initialize":
        respond(req, {"supportsConfigurationDoneRequest": True})
        event("initialized")
    elif cmd == "launch" or cmd == "attach":
        respond(req)
    elif cmd == "configurationDone":
        respond(req)
        event("stopped", {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True})
    elif cmd == "threads":
        respond(req, {"threads": [{"id": 1, "name": "main"}]})
    elif cmd == "stackTrace":
        frames = [{"id": 1, "name": "main", "line": 10, "column": 1,
                   "source": {"name": "main.c", "path": "/src/main.c"}}]
        respond(req, {"stackFrames": frames, "totalFrames": 1})
    elif cmd == "scopes":
        respond(req, {"scopes": [{"name": "Locals", "variablesReference": 1000,
                                  "namedVariables": var_count}]})
    elif cmd == "variables":
        ref = req.get("arguments", {}).get("variablesReference", 0)
        start = req.get("arguments", {}).get("start", 0)
        count = req.get("arguments", {}).get("count", 0) or var_count
        vs = []
        for i in range(start, min(start + count, var_count)):
            vs.append({"name": "v%d" % i, "value": str(i), "type": "int",
                       "variablesReference": 0})
        respond(req, {"variables": vs})
    elif cmd == "disconnect" or cmd == "terminate":
        respond(req)
        break
    else:
        respond(req)
)py";
}

bool HavePython3() {
  // Best-effort: the scenario writes a python adapter and execs `python3`.
  return std::system("python3 --version >/dev/null 2>&1") == 0;
}

void RunDebugSessionStopToVariables(ScenarioContext& context) {
#if !defined(__unix__) && !defined(__APPLE__)
  (void)context;
  return;
#else
  if (!HavePython3()) {
    std::fprintf(stderr, "debug_session_stop_to_variables: python3 unavailable; skipping\n");
    return;
  }
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "microide_perf_dap";
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path adapter = dir / "perf_adapter.py";
  {
    std::ofstream out(adapter, std::ios::binary | std::ios::trunc);
    out << DebugPerfAdapterSource();
  }

  const auto poll = [](DapManager& manager, const std::function<bool()>& pred,
                       int timeout_ms) -> bool {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      manager.DrainCallbacks();
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    manager.DrainCallbacks();
    return pred();
  };

  context.Measure("session.stop_to_variables", [&]() {
    DapManager manager;
    manager.RegisterAdapter("mock", {"python3", adapter.string(), "400"});

    int stop_count = 0;
    std::vector<codec::DapStackFrame> last_frames;
    DebugSession::Callbacks callbacks;
    callbacks.on_stopped = [&](const codec::DapStoppedEvent&,
                               const std::vector<codec::DapStackFrame>& frames) {
      last_frames = frames;
      ++stop_count;
    };

    LaunchConfig config;
    config.type = "mock";
    config.request = "launch";
    if (!manager.StartSession(config, std::move(callbacks))) {
      return;
    }
    if (!poll(manager, [&]() { return stop_count >= 1; }, 5000) || last_frames.empty()) {
      manager.ShutdownAll();
      return;
    }
    DebugSession* session = manager.ActiveSession();
    if (session == nullptr) {
      manager.ShutdownAll();
      return;
    }
    const int frame_id = last_frames.front().id;

    DebugValueTree tree;
    bool scopes_done = false;
    std::vector<DebugValueTree::ChildFetch> scope_fetches;
    session->RequestScopes(frame_id, [&](std::vector<codec::DapScope> scopes) {
      for (const codec::DapScope& scope : scopes) {
        tree.AddRoot(scope.name, {}, {}, scope.variables_reference, /*is_scope=*/true,
                     scope.named_variables + scope.indexed_variables, scope.count_reported);
      }
      tree.Rebuild();
      if (!tree.Rows().empty()) {
        scope_fetches.push_back(tree.ToggleRow(0));  // expand the first scope
      }
      scopes_done = true;
    });
    (void)poll(manager, [&]() { return scopes_done; }, 5000);

    if (!scope_fetches.empty() && scope_fetches.front().reference != 0) {
      const int ref = scope_fetches.front().reference;
      bool vars_done = false;
      session->RequestVariables(ref, 0, DebugValueTree::kChildPageSize,
                                [&](bool ok, std::vector<codec::DapVariable> vars) {
                                  if (ok) {
                                    tree.ApplyVariables(ref, vars, 0);
                                  }
                                  vars_done = true;
                                });
      (void)poll(manager, [&]() { return vars_done; }, 5000);
    }
    manager.ShutdownAll();
  });

  std::filesystem::remove_all(dir, ec);
#endif
}

// ---- Registration ----------------------------------------------------------

// Three of the pure-unit micro-benchmarks below measure single-digit
// milliseconds of deterministic computation, and this shared reference runner
// cannot hold a 10%/20%/50% wall envelope over work that small: their
// ALLOCATION counts are byte-identical to baseline run after run, while their
// wall medians measured 16-120% over the committed p50 -- including on an
// unmodified checkout, built and run in a worktree for exactly that comparison.
// Gating wall at the allocation percent means three permanently red gates, which
// is strictly worse than no gate: a real regression lands in a row that was
// already red.
//
// So decouple them exactly as the tech-debt coverage scenarios already do
// (see TechDebtCoveragePerfScenarios.cpp): allocations stay at the tight default
// -- that is the complexity gate, and it is exact here -- while the wall envelope
// widens to absorb scheduler jitter. A constant-factor wall regression is still
// caught precisely by the interleaved tools/perf-compare.py current-vs-main run,
// where shared machine load cancels.
//
// This is deliberately NOT applied to every scenario in this file: the ones that
// hold their envelope keep their tight wall gate.

const ScenarioRegistration g_perf_debug_value_tree_expand_large({Scenario{
    .name = "debug_value_tree_expand_large",
    .smoke = true,
    .baseline_gated = true,
    .run = RunDebugValueTreeExpandLarge,
}});
const ScenarioRegistration g_perf_debug_value_tree_rebuild({Scenario{
    .name = "debug_value_tree_rebuild",
    .smoke = true,
    .baseline_gated = true,
    .run = RunDebugValueTreeRebuild,
}});
const ScenarioRegistration g_perf_debug_value_tree_paging({Scenario{
    .name = "debug_value_tree_paging",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunDebugValueTreePaging,
}});
const ScenarioRegistration g_perf_dap_protocol_encode_decode({Scenario{
    .name = "dap_protocol_encode_decode",
    .smoke = true,
    .baseline_gated = true,
    .run = RunDapProtocolEncodeDecode,
}});
const ScenarioRegistration g_perf_debug_breakpoints_model_rebuild({Scenario{
    .name = "debug_breakpoints_model_rebuild",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunDebugBreakpointsModelRebuild,
}});
const ScenarioRegistration g_perf_debug_pane_hittest_geometry({Scenario{
    .name = "debug_pane_hittest_geometry",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunDebugPaneHittestGeometry,
}});
const ScenarioRegistration g_perf_debug_session_stop_to_variables({Scenario{
    .name = "debug_session_stop_to_variables",
    .smoke = false,
    .baseline_gated = false,  // advisory: reference-runner baseline pending; promote later
    .run = RunDebugSessionStopToVariables,
}});

}  // namespace
}  // namespace microide::tests::perf
