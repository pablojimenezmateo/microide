// Local/remote parity (dev-docs/design/remote-projects.md § 10.1, Groundwork G11):
// each scenario runs against the same tree opened locally and opened through the
// loopback non-local locality (tests/parity/LoopbackLocality.h), and what the user
// would see must be equal. A row that cannot be equal yet is in ParityKnownGaps.h
// with what removes it.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "parity/LoopbackLocality.h"
#include "parity/ParityHarness.h"
#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/HostPathTranslator.h"
#include "support/GitSidebarWait.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using parity::Outcome;
using parity::Scenario;
using parity::Tree;

std::string_view SectionName(workspace::GitSidebarEntry::Section section) {
  switch (section) {
    case workspace::GitSidebarEntry::Section::Conflicts:
      return "conflicts";
    case workspace::GitSidebarEntry::Section::Staged:
      return "staged";
    case workspace::GitSidebarEntry::Section::Changed:
      return "changed";
    case workspace::GitSidebarEntry::Section::Untracked:
      return "untracked";
    case workspace::GitSidebarEntry::Section::Outgoing:
      return "outgoing";
  }
  return "?";
}

// An editor save lands in the tree the project's processes see. A save that
// bypassed the project's write gate would leave the host's bytes unchanged.
Scenario SaveReachesTheTree() {
  return Scenario{
      .name = "Parity/SaveReachesTheTree",
      .build = [](const std::filesystem::path& root,
                  bool) { WriteFile(root / "notes.txt", "alpha\n"); },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            WorkspaceShellTestAccess::OpenFile(shell, tree.root / "notes.txt");
            auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
            editor.MoveCursorTo(0, 5);
            editor.InsertText("_beta");
            const bool saved = WorkspaceShellTestAccess::SaveTab(
                shell, WorkspaceShellTestAccess::ActiveTabIndex(shell));
            outcome.Add("save", "ok", saved ? "yes" : "no");
            outcome.Add("editor", "dirty", editor.dirty() ? "yes" : "no");
            outcome.Add("bytes", "notes.txt", ReadFile(tree.host_root / "notes.txt"));
          },
      .writes = true,
  };
}

// The git sidebar shows the host's working tree. The mirror has no `.git`
// (design § 6.2), so this is where every reader of a LOCAL `.git` shows up.
Scenario GitSidebarShowsTheWorkingTree() {
  return Scenario{
      .name = "Parity/GitSidebarShowsTheWorkingTree",
      .build =
          [](const std::filesystem::path& root, bool is_mirror) {
            if (!is_mirror) {
              InitializeGitRepo(root);
              WriteFile(root / "tracked.txt", "one\n");
              CommitAll(root, "base", "parity git fixture");
            }
            WriteFile(root / "tracked.txt", "one\ntwo\n");
            WriteFile(root / "untracked.txt", "new\n");
          },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            WorkspaceShellTestAccess::ShowGitSidebar(shell);
            (void)SettleGitSidebarRefresh(shell);
            // The local run is the reference, so it must be RIGHT on its own: while
            // this row is a known gap, a broken local run would still "differ" and
            // read as the expected gap.
            if (tree.locality == parity::Locality::kLocal) {
              Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == 2,
                     "parity reference: the local git sidebar lists both changes");
            }
            for (const auto& entry : WorkspaceShellTestAccess::GitSidebarEntries(shell)) {
              outcome.Add("git-entry", entry.relative_path, SectionName(entry.section));
            }
            for (const std::string& line : WorkspaceShellTestAccess::GitSidebarSummaryLines(shell)) {
              outcome.Add("git-summary", "", line);
            }
          },
      .spawns = true,
  };
}

// A language server that, like any process on a real host, sees only the host's
// files: its working directory is the host root, and a path outside it is one it
// cannot open. It reports what it could read (a diagnostic per TODO line, from
// the bytes ON ITS DISK) and answers definition with a host path, as a real
// server does. Whatever it is told, it echoes back -- so an untranslated mirror
// path reaches it as "not a host path", and an untranslated host path reaches
// the user as a path outside the project.
constexpr std::string_view kHostConfinedLspServer = R"py(import json, os, sys
from urllib.parse import quote, unquote

host = os.path.realpath(os.getcwd())

def uri_to_path(u):
    return unquote(u[len("file://"):]) if u.startswith("file://") else u

def path_to_uri(p):
    return "file://" + quote(p, safe="/-._~")

def on_host(p):
    p = os.path.realpath(p)
    return p == host or p.startswith(host + os.sep)

def read():
    n = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":", 1)[1])
    return json.loads(sys.stdin.buffer.read(n)) if n else None

def write(m):
    b = json.dumps(m).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    sys.stdout.buffer.flush()

def rng(line):
    return {"start": {"line": line, "character": 0}, "end": {"line": line, "character": 1}}

while True:
    m = read()
    if m is None:
        break
    method = m.get("method")
    if method == "initialize":
        root = uri_to_path(m["params"].get("rootUri") or "")
        write({"jsonrpc": "2.0", "id": m["id"], "result": {"capabilities": {
            "textDocumentSync": 1, "definitionProvider": True}}})
        if not on_host(root):
            write({"jsonrpc": "2.0", "method": "window/logMessage",
                   "params": {"type": 1, "message": "root is not a host path"}})
    elif method == "textDocument/didOpen":
        uri = m["params"]["textDocument"]["uri"]
        path = uri_to_path(uri)
        if on_host(path) and os.path.isfile(path):
            with open(path) as f:
                diags = [{"range": rng(i), "message": "todo", "severity": 2}
                         for i, l in enumerate(f.read().split("\n")) if "TODO" in l]
        else:
            diags = [{"range": rng(0), "message": "not a host path", "severity": 1}]
        write({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
               "params": {"uri": uri, "diagnostics": diags}})
    elif method == "textDocument/definition":
        write({"jsonrpc": "2.0", "id": m["id"], "result": [
            {"uri": path_to_uri(os.path.join(host, "defs.md")), "range": rng(1)}]})
    elif method == "shutdown":
        write({"jsonrpc": "2.0", "id": m["id"], "result": None})
    elif method == "exit":
        break
)py";

// The language server runs on the host, reads the host's bytes, and every path
// it is given or gives back crosses the mirror/host boundary: diagnostics land on
// the editor's buffer and go-to-definition opens the editor's copy of the file.
Scenario LanguageServerSeesTheHostTree() {
  return Scenario{
      .name = "Parity/LanguageServerSeesTheHostTree",
      .build =
          [](const std::filesystem::path& root, bool) {
            WriteFile(root / "notes.md", "alpha\nTODO one\nbeta\nTODO two\n");
            WriteFile(root / "defs.md", "first\nthe definition\n");
          },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            const std::filesystem::path notes = tree.root / "notes.md";
            WorkspaceShellTestAccess::OpenFile(shell, notes);
            const auto pump = [&shell] { WorkspaceShellTestAccess::ConsumeLspCallbacks(shell); };
            (void)WaitUntil(
                [&] { return WorkspaceShellTestAccess::DiagnosticsForPath(shell, notes) != nullptr; },
                std::chrono::seconds(10), std::chrono::milliseconds(5), pump);
            if (const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, notes)) {
              for (const auto& diagnostic : *diagnostics) {
                outcome.Add("diagnostic", "notes.md:" + std::to_string(diagnostic.range.start.line + 1),
                            diagnostic.message);
              }
            } else {
              outcome.Add("diagnostic", "notes.md", "none arrived");
            }
            if (tree.locality == parity::Locality::kLocal) {
              Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, notes) != nullptr &&
                         WorkspaceShellTestAccess::DiagnosticsForPath(shell, notes)->size() == 2,
                     "parity reference: the local server reports both TODO lines");
            }

            WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 1);
            Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto-definition"),
                   "goto-definition runs");
            (void)WaitUntil(
                [&] { return WorkspaceShellTestAccess::ActiveEditor(shell).path() != notes; },
                std::chrono::seconds(10), std::chrono::milliseconds(5), pump);
            const auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
            outcome.Add("definition", "opened", editor.path().generic_string());
            outcome.Add("definition", "line", std::to_string(editor.cursor_line() + 1));
          },
      .spawns = true,
  };
}

// "relative/path d|f" for every entry under `root` except `.git`, sorted.
std::vector<std::string> ListTree(const std::filesystem::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const std::filesystem::path relative = it->path().lexically_relative(root);
    if (!relative.empty() && *relative.begin() == ".git") {
      it.disable_recursion_pending();
      continue;
    }
    out.push_back(relative.generic_string() + (it->is_directory() ? " d" : " f"));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// The sidebar's create-file, create-folder and rename land in BOTH trees -- the
// editor's and the one the project's processes see -- through the project's gate.
Scenario FileOperationsReachTheTree() {
  return Scenario{
      .name = "Parity/FileOperationsReachTheTree",
      .build =
          [](const std::filesystem::path& root, bool) {
            WriteFile(root / "a.txt", "a\n");
            std::filesystem::create_directories(root / "src");
          },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            using Action = workspace::PromptSurfaceState::Action;
            using Kind = workspace::PromptSurfaceState::Kind;
            const auto apply = [&shell](Action action, const std::filesystem::path& path,
                                        std::string input) {
              WorkspaceShellTestAccess::OpenPromptSurfaceForTest(shell, action, Kind::TextInput,
                                                                 path, "", std::move(input));
              WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
            };
            apply(Action::CreateFile, tree.root, "src/new.txt");
            outcome.Add("opened-after-create", "",
                        WorkspaceShellTestAccess::ActiveEditor(shell).path().generic_string());
            apply(Action::CreateDirectory, tree.root, "docs");
            apply(Action::RenamePath, tree.root / "a.txt", "b.txt");
            for (const std::string& entry : ListTree(tree.root)) {
              outcome.Add("editor-tree", "", entry);
            }
            for (const std::string& entry : ListTree(tree.host_root)) {
              outcome.Add("host-tree", "", entry);
            }
            if (tree.locality == parity::Locality::kLocal) {
              const std::vector<std::string> expected = {"b.txt f", "docs d", "src d",
                                                         "src/new.txt f"};
              Expect(ListTree(tree.root) == expected,
                     "parity reference: create, create-folder and rename all landed locally");
            }
          },
      .writes = true,
  };
}

// The contributed formatter runs where the project runs, and the formatted save
// reaches the host's bytes.
Scenario FormatOnSaveReachesTheTree() {
  return Scenario{
      .name = "Parity/FormatOnSaveReachesTheTree",
      .build = [](const std::filesystem::path& root,
                  bool) { WriteFile(root / "list.todo", "seed\n"); },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            const std::filesystem::path file = tree.root / "list.todo";
            WorkspaceShellTestAccess::OpenFile(shell, file);
            auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
            editor.SelectAll();
            editor.InsertText("alpha\n");
            const bool saved = WorkspaceShellTestAccess::SaveTab(
                shell, WorkspaceShellTestAccess::ActiveTabIndex(shell));
            outcome.Add("save", "ok", saved ? "yes" : "no");
            outcome.Add("bytes", "list.todo", ReadFile(tree.host_root / "list.todo"));
            if (tree.locality == parity::Locality::kLocal) {
              Expect(ReadFile(tree.root / "list.todo") == "ALPHA\n",
                     "parity reference: the local save ran the formatter, got: " +
                         ReadFile(tree.root / "list.todo"));
            }
            outcome.Add("editor", "dirty", editor.dirty() ? "yes" : "no");
          },
      .spawns = true,
      .writes = true,
  };
}

// A plugin's tool runs in the project's tree (where `git` sees the repository
// only the HOST has), and a plugin's write lands in the host's bytes.
Scenario PluginToolsFollowTheProject() {
  return Scenario{
      .name = "Parity/PluginToolsFollowTheProject",
      .build =
          [](const std::filesystem::path& root, bool is_mirror) {
            if (!is_mirror) {
              InitializeGitRepo(root);
            }
            WriteFile(root / "README.md", "readme\n");
          },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            WorkspaceShellTestAccess::ClearPluginMessages(shell);
            Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "parity.tools"),
                   "the plugin command runs");
            for (const std::string& message : WorkspaceShellTestAccess::PluginMessages(shell)) {
              outcome.Add("plugin-log", "", message);
            }
            outcome.Add("bytes", "plugin.txt", ReadFile(tree.host_root / "plugin.txt"));
            if (tree.locality == parity::Locality::kLocal) {
              const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
              Expect(std::find(messages.begin(), messages.end(),
                               "parity-tools: git:0:true\n") != messages.end(),
                     "parity reference: the local plugin tool ran git inside the repository; log: " +
                         [&] {
                           std::string all;
                           for (const auto& m : messages) all += "[" + m + "]";
                           return all;
                         }());
            }
          },
      .spawns = true,
      .writes = true,
  };
}

// One plugin for the format-on-save and plugin-tool rows: a `todo` filetype with
// an uppercasing formatter, and a command that runs git and writes a file.
void WriteParityToolsPlugin(const std::filesystem::path& config_home) {
  const std::filesystem::path plugin = config_home / "microide" / "plugins" / "parity-tools";
  WriteFile(plugin / "init.lua", R"lua(local ide = require("microide")
return ide.plugin({
  id = "parity-tools",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.formatters.add({
      id = "todo-uppercase",
      language_id = "todo",
      label = "TODO Uppercase",
      command = { "sh", "-c", "tr '[:lower:]' '[:upper:]'" },
    })
    ctx.commands.add("parity.tools", function(ctx, args)
      local git = ctx.process.run({ "git", "rev-parse", "--is-inside-work-tree" }, { cwd = "." })
      ctx.log("git:" .. tostring(git.exit_code) .. ":" .. (git.stdout or "") .. (git.stderr or ""))
      ctx.log("wrote:" .. tostring(ctx.files.write_text("plugin.txt", "from plugin\n")))
    end)
  end
})
)lua");
  WriteFile(plugin / "syntax" / "todo.lua", R"lua(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = { { pattern = "\\b[A-Z_]+\\b", group = "keyword" } }
}
)lua");
}

}  // namespace

void RegisterParityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Parity/SaveReachesTheTree",
          [] { parity::ExpectParity(SaveReachesTheTree()); });
  AddTest(tests, "Parity/GitSidebarShowsTheWorkingTree",
          [] { parity::ExpectParity(GitSidebarShowsTheWorkingTree()); });
  AddTest(tests, "Parity/FileOperationsReachTheTree",
          [] { parity::ExpectParity(FileOperationsReachTheTree()); });
  AddTest(tests, "Parity/FormatOnSaveReachesTheTree", [] {
#if !MICROIDE_HAS_LUA_PLUGINS
    return;
#endif
    TemporaryDirectory support_dir;
    WriteParityToolsPlugin(support_dir.path());
    ScopedPluginConfigHomeEnv scoped_config(support_dir.path());
    parity::ExpectParity(FormatOnSaveReachesTheTree());
  });
  AddTest(tests, "Parity/PluginToolsFollowTheProject", [] {
#if !MICROIDE_HAS_LUA_PLUGINS
    return;
#endif
    TemporaryDirectory support_dir;
    WriteParityToolsPlugin(support_dir.path());
    ScopedPluginConfigHomeEnv scoped_config(support_dir.path());
    parity::ExpectParity(PluginToolsFollowTheProject());
  });
  AddTest(tests, "Parity/LanguageServerSeesTheHostTree", [] {
#if !MICROIDE_HAS_LUA_PLUGINS
    return;
#endif
    TemporaryDirectory support_dir;
    const std::filesystem::path server = support_dir.path() / "host_confined_lsp.py";
    WriteFile(server, std::string(kHostConfinedLspServer));
    const std::filesystem::path config_home = support_dir.path() / "config";
    WritePluginInit(config_home / "microide" / "plugins", "parity-lsp",
                    R"lua(local ide = require("microide")
return ide.plugin({
  id = "parity-lsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "python3", ")lua" +
                        server.generic_string() + R"lua(" } })
  end
})
)lua");
    ScopedPluginConfigHomeEnv scoped_config(config_home);
    parity::ExpectParity(LanguageServerSeesTheHostTree());
  });

  // The transport's path translation (workspace/HostPathTranslator), unit-level.
  AddTest(tests, "HostPathTranslator/RewritesPathKeysAndNothingElse", [] {
    const parity::LoopbackProcessLauncher launcher(
        parity::LoopbackPathMap("/m/project", "/h/project"));
    const workspace::HostPathTranslator translator(launcher);
    Expect(translator.active(), "a non-local launcher's translator is active");
    const std::string mirror_uri = workspace::FileUriForPath("/m/project/a.md");
    const std::string host_uri = workspace::FileUriForPath("/h/project/a.md");

    util::JsonObject text_document;
    text_document["uri"] = util::JsonValue(mirror_uri);
    // A document that MENTIONS a mirror URI: the user's bytes, never rewritten.
    text_document["text"] = util::JsonValue("see " + mirror_uri);
    util::JsonObject params;
    params["textDocument"] = util::JsonValue(std::move(text_document));
    params["rootPath"] = util::JsonValue("/m/project");
    util::JsonValue message(std::move(params));
    translator.ToHost(message);
    Expect(message["textDocument"]["uri"].AsString() == host_uri, "a uri key goes to the host");
    Expect(message["textDocument"]["text"].AsString() == "see " + mirror_uri,
           "document text is never rewritten, even when it contains a file URI");
    Expect(message["rootPath"].AsString() == "/h/project", "a path key goes to the host");

    // WorkspaceEdit.changes: the URI is an object KEY.
    util::JsonObject changes;
    changes[host_uri] = util::JsonValue(util::JsonArray{});
    util::JsonObject edit;
    edit["changes"] = util::JsonValue(std::move(changes));
    util::JsonValue reply(std::move(edit));
    translator.FromHost(reply);
    Expect(reply["changes"].HasKey(mirror_uri) && !reply["changes"].HasKey(host_uri),
           "a WorkspaceEdit's changes come back keyed by the editor's URI");

    // DAP: a stack frame's source path, and a path outside the project untouched.
    util::JsonObject source;
    source["path"] = util::JsonValue("/h/project/main.c");
    util::JsonObject outside;
    outside["path"] = util::JsonValue("/usr/include/stdio.h");
    util::JsonArray frames;
    frames.push_back(util::JsonValue(std::move(source)));
    frames.push_back(util::JsonValue(std::move(outside)));
    util::JsonValue frames_value(std::move(frames));
    translator.FromHost(frames_value);
    Expect(frames_value[0]["path"].AsString() == "/m/project/main.c",
           "a debugger's host path comes back as the editor's");
    Expect(frames_value[1]["path"].AsString() == "/usr/include/stdio.h",
           "a path outside the project is left alone");

    const workspace::HostPathTranslator local(platform::LocalProcessLauncher());
    Expect(!local.active(), "a local project's translator is inactive: no walk at all");
  });

  // Positive controls: the runner must be able to fail. A parity suite that cannot
  // tell a leaked host path from a correct one, or that passes when nothing went
  // through the loopback, is green and worthless (validation-traps.md).
  AddTest(tests, "Parity/Control/LeakedHostPathFails", [] {
    const Scenario leak{
        .name = "Parity/Control/LeakedHostPath",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree& tree,
                  Outcome& outcome) { outcome.Add("path", "shown", (tree.host_root / "a.txt").string()); },
    };
    const std::string failure = parity::CheckParity(leak);
    Expect(failure.find("+ loopback: path | shown | ") != std::string::npos,
           "a host path shown to the user must fail parity, got: " + failure);
  });
  AddTest(tests, "Parity/Control/UnwiredLoopbackFails", [] {
    const Scenario unwired{
        .name = "Parity/Control/UnwiredLoopback",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree&, Outcome& outcome) { outcome.Add("k", "", "v"); },
        .spawns = true,
    };
    Expect(parity::CheckParity(unwired).find("saw no spawn") != std::string::npos,
           "a spawning scenario with no loopback spawn must fail as vacuous");
  });
  AddTest(tests, "Parity/Control/EmptyOutcomeFails", [] {
    const Scenario empty{
        .name = "Parity/Control/EmptyOutcome",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree&, Outcome&) {},
    };
    Expect(parity::CheckParity(empty).find("observed nothing") != std::string::npos,
           "a scenario that records nothing must fail as vacuous");
  });
}

}  // namespace microide::tests
