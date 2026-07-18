// Plugin-surface coverage regression net.
//
// A single fixture plugin registers ONE of every plugin contribution surface and
// drives the runtime publish/edit seams. The test then asserts, in one clean load,
// that:
//   * every Contributed* registration accessor is populated,
//   * every Query* provider seam dispatches into Lua and marshals a result back,
//   * the publish callbacks (diagnostics / decorations / surface / notify / log /
//     ghost text / buffer edit) fire,
//   * and host.Errors() stays empty.
//
// The point is breadth: if any single surface silently stops registering or
// dispatching, exactly one assertion here flips red. Deep per-surface behavior
// (atomic-undo edits, debounced reactive events, snippet Tab expansion, the
// zero-cost-when-unused sampler) lives in PluginHostTests / WorkspaceShellPluginTests;
// this file guarantees nothing falls off the contract wholesale.

#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "plugin/PluginSurfaceInterop.h"

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::plugin::PluginHost;

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
}

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

// One plugin, every surface. Registration uses the same idioms the dogfood plugins
// and the existing tests use; the language providers share the synthetic language
// id "qa-cov" so the Query* calls below match them deterministically (no dependence
// on filetype detection). lsp/debug commands are inert (`sh -c true`) — registration
// smoke only, never a live session.
constexpr std::string_view kCoveragePlugin = R"lua(
local ide = require("microide")

local LANG = "qa-cov"

return ide.plugin({
  id = "qa-coverage",
  capabilities = {
    fs = { read = "data", write = "data" },
    process = { exec = true },
  },

  setup = function(ctx)
    -- ---- command / menu / keybinding / settings / status -----------------
    ctx.settings.declare({
      id = "opt", type = "string", default = "x", scope = "project",
      label = "QA Opt", description = "coverage setting",
    })
    ctx.commands.add("qa.cov.cmd", function(ctx, args)
      return "ran:" .. table.concat(args or {}, ",")
    end)
    ctx.menus.add({ id = "m", menu = "Tools", action = "qa.cov.cmd", label = "QA Cov" })
    ctx.keybindings.add({ id = "k", action = "qa.cov.cmd", key = "Ctrl+Alt+J", context = "editor" })
    ctx.status.add({ id = "s", text = "QA", tooltip = "t", alignment = "right", priority = 5 })

    -- ---- sidebar + hover -------------------------------------------------
    ctx.sidebar.add({
      id = "qa-cov", label = "QA",
      snapshot = function() return { { label = "row", path = "README.md", line = 1 } } end,
    })
    ctx.hover.add({
      id = "qa-cov.hover",
      provide = function(buffer, position)
        if position.line == 1 then
          return { title = "QA", content = "hover@" .. tostring(position.line) }
        end
        return nil
      end,
    })

    -- ---- language providers (language id LANG) ---------------------------
    ctx.completion.add({
      id = "c", language_id = LANG, trigger_characters = { "." },
      provide = function(buffer, position, trigger)
        return { { label = "qa", detail = "d", documentation = "trigger:" .. tostring(trigger), insert_text = "QA" } }
      end,
    })
    ctx.code_actions.add({
      id = "ca", language_id = LANG,
      provide = function(buffer, range)
        return { { title = "QA fix", command = "qa.cov.cmd", arguments = { "fix" } } }
      end,
    })
    ctx.definition.add({
      id = "def", language_id = LANG,
      provide = function(buffer, position) return { { path = buffer.path, line = 1, column = 1 } } end,
    })
    ctx.references.add({
      id = "ref", language_id = LANG,
      provide = function(buffer, position, include_declaration)
        local out = { { path = buffer.path, line = position.line, column = position.column } }
        if include_declaration then out[#out + 1] = { path = buffer.path, line = 1, column = 1 } end
        return out
      end,
    })
    ctx.signature_help.add({
      id = "sig", language_id = LANG,
      provide = function()
        return {
          active_signature = 0,
          signatures = { { label = "qa(name)", documentation = "doc", active_parameter = 0,
                           parameters = { { label = "name" } } } },
        }
      end,
    })
    ctx.document_symbols.add({
      id = "sym", language_id = LANG,
      provide = function()
        return { { name = "Root", kind = "namespace", line = 1, column = 1,
                   children = { { name = "child", kind = "function", line = 2, column = 1 } } } }
      end,
    })

    -- ---- formatter / save participant ------------------------------------
    ctx.formatters.add({ id = "f", language_id = LANG, label = "QA", command = { "cat" } })
    ctx.save_participants.add("upper", function(buffer)
      return { text = string.upper(buffer.text or "") }
    end)

    -- ---- lsp / debug / tasks / tools (registration smoke) ----------------
    ctx.lsp.add({ id = "ls", language_ids = { LANG }, command = { "sh", "-c", "true" } })
    ctx.debug.add({ id = "dap", type = "qa-dap", command = { "sh", "-c", "true" } })
    ctx.debug.addConfig({ id = "launch", name = "QA", type = "qa-dap", request = "launch",
                          arguments = '{"program":"main"}' })
    ctx.tasks.add({ id = "task", label = "QA Task", group = "build",
                    command = { "sh", "-c", "true" }, cwd = ".", run_in_shell = false })
    ctx.tools.add({ id = "tool", label = "QA Tool", platform = "linux-x64",
                    url = "https://example.invalid/t.tgz",
                    sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
                    install_dir = "qa" })

    -- ---- test / scm / annotation / auth providers ------------------------
    ctx.tests.add({
      id = "t", language_id = LANG,
      discover = function(buffer)
        return { { id = "t.case", label = "QA case", file = buffer.relative_path, line = 1 } }
      end,
      run = function(test_ids)
        return { { test_id = test_ids[1], state = "passed", message = "ok", duration_ms = 1 } }
      end,
    })
    ctx.scm.add("scm", "QA SCM")
    ctx.annotations.add({ id = "blame", label = "QA Blame", type = "blame", language_id = LANG })
    ctx.auth.add("auth", "QA Auth")

    -- ---- language-contract tables ----------------------------------------
    ctx.brackets.add({ language_id = LANG, pairs = { { "(", ")" } },
                       auto_close = { { "(", ")" } }, surround = { { "(", ")" } } })
    ctx.comments.add({ language_id = LANG, line = "--", block_open = "--[[", block_close = "]]" })
    ctx.indents.add({ language_id = LANG, indent_after_open = { "do" }, dedent_on_close = { "end" } })
    ctx.snippets.add({ id = "sn", language_id = LANG, prefix = "qa", label = "QA", body = "snippet" })

    -- ---- presentation ----------------------------------------------------
    ctx.themes.add({ id = "th", label = "QA Theme", colors = { ["default"] = "#ffffff,#000000" } })
    ctx.file_icons.add({ id = "ic", rules = { { ext = "md", icon = "dot", color = "#ffffff" } } })

    -- ---- runtime publish seams (captured by the host callbacks) ----------
    ctx.log("coverage-setup")
    ctx.notify("info", "coverage-notify")
    ctx.diagnostics.publish("README.md", {
      { message = "qa diag", severity = "warning", line = 1, column = 1, end_column = 2 },
    })
    ctx.decorations.set("README.md", {
      text_styles = { { line = 1, start_col = 1, end_col = 2, fg = "#ff0000" } },
    })
    ctx.surface.set("qa-surface", {
      title = "QA",
      display_list = { width = 10, height = 10, ops = { { op = "rect", x = 0, y = 0, w = 10, h = 10, color = "#101010" } } },
      -- A hit region with an inverted (negative-w) and non-finite rect: the host
      -- must sanitize it (w clamped to >= 0, NaN folded to 0) at parse time.
      hit_regions = { { x = 0/0, y = 5, w = -8, h = 4, command = "noop" } },
    })

    -- ---- buffer-edit + ghost-text seams (invoked via command) ------------
    ctx.commands.add("qa.cov.edit", function(ctx, args)
      ctx.editor.apply_edits({ edits = { { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "X" } } })
      ctx.editor.set_ghost_text({ text = "ghost" })
    end)
  end,
})
)lua";

// Tracks which host callbacks the plugin exercised, so the test can assert that the
// runtime publish/edit seams actually fired (not just that registration parsed).
struct CallbackProbe {
  std::vector<std::string> logs;
  std::vector<std::string> notifications;
  bool diagnostics_published = false;
  bool decorations_published = false;
  bool surface_published = false;
  bool edit_applied = false;
  bool ghost_published = false;
  std::optional<editor::SurfaceContent> last_surface;
};

PluginHost::Callbacks MakeProbingCallbacks(CallbackProbe* probe) {
  PluginHost::Callbacks callbacks;
  callbacks.is_command_name_available = [](std::string_view) { return true; };
  callbacks.log_sink = [probe](const std::string& message) { probe->logs.push_back(message); };
  callbacks.show_notification = [probe](const std::string& level, const std::string& message) {
    probe->notifications.push_back(level + ":" + message);
  };
  callbacks.publish_diagnostics =
      [probe](std::string_view, const std::filesystem::path&, std::vector<editor::Diagnostic>) {
        probe->diagnostics_published = true;
      };
  callbacks.publish_decorations =
      [probe](std::string_view, const std::filesystem::path&, editor::PluginDecorationData) {
        probe->decorations_published = true;
      };
  callbacks.publish_surface =
      [probe](std::string_view, std::string_view, editor::SurfaceContent content) {
        probe->surface_published = true;
        probe->last_surface = std::move(content);
      };
  callbacks.apply_workspace_edit =
      [probe](std::string_view, const PluginHost::WorkspaceEditRequest&) {
        probe->edit_applied = true;
        return true;
      };
  callbacks.publish_ghost_text =
      [probe](std::string_view, const PluginHost::GhostTextRequest&) {
        probe->ghost_published = true;
      };
  return callbacks;
}

void TestPluginSurfaceCoverageRegistersEverySurface() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "alpha beta\nsecond line\n");
  WritePluginInit(plugins_root, "qa-coverage", kCoveragePlugin);

  ScopedPluginConfigHomeEnv config_env(config_home);

  CallbackProbe probe;
  PluginHost host;
  host.SetCallbacks(MakeProbingCallbacks(&probe));

  Expect(host.Reload(project_root), "coverage plugin should reload without load errors");
  Expect(host.Errors().empty(), "coverage plugin must register every surface with zero errors");
  Expect(host.LoadedPluginCount() == 1, "exactly the coverage plugin should be loaded");

  const std::filesystem::path readme = project_root / "README.md";
  const std::string_view lang = "qa-cov";

  // ---- registration accessors: each surface must have landed -------------
  auto has = [](const auto& vec) { return !vec.empty(); };
  Expect(has(host.CommandNames()), "commands surface should register");
  Expect(has(host.SidebarProviders()), "sidebar surface should register");
  Expect(has(host.ContributedMenuEntries()), "menu surface should register");
  Expect(has(host.ContributedKeybindings()), "keybinding surface should register");
  Expect(has(host.ContributedSettings()), "settings surface should register");
  Expect(has(host.ContributedStatusItems()), "status surface should register");
  Expect(has(host.ContributedFormatters()), "formatter surface should register");
  Expect(has(host.ContributedSaveParticipants()), "save-participant surface should register");
  Expect(has(host.ContributedCompletions()), "completion surface should register");
  Expect(has(host.ContributedCodeActions()), "code-action surface should register");
  Expect(has(host.ContributedLanguageServers()), "lsp surface should register");
  Expect(has(host.ContributedDebugAdapters()), "debug-adapter surface should register");
  Expect(has(host.ContributedLaunchConfigs()), "launch-config surface should register");
  Expect(has(host.ContributedTasks()), "task surface should register");
  Expect(has(host.ContributedTools()), "tool surface should register");
  Expect(has(host.ContributedTestProviders()), "test-provider surface should register");
  Expect(has(host.ContributedScmProviders()), "scm surface should register");
  Expect(has(host.ContributedAnnotationProviders()), "annotation surface should register");
  Expect(has(host.ContributedAuthProviders()), "auth surface should register");
  Expect(has(host.ContributedBrackets()), "bracket surface should register");
  Expect(has(host.ContributedComments()), "comment surface should register");
  Expect(has(host.ContributedIndents()), "indent surface should register");
  Expect(has(host.ContributedSnippets()), "snippet surface should register");
  Expect(has(host.ContributedThemes()), "theme surface should register");
  Expect(has(host.ContributedFileIconThemes()), "file-icon surface should register");

  // ---- provider dispatch: each Query* must reach Lua and marshal a result -
  std::string err;
  PluginHost::HoverResult hover;
  Expect(host.QueryHover(readme, 1, 1, &hover, &err), "hover provider should dispatch");

  Expect(!host.QueryCompletions(lang, readme, 1, 1, ".", &err).empty(),
         "completion provider should dispatch");
  Expect(!host.QueryCodeActions(lang, readme, 1, 1, 1, 1, &err).empty(),
         "code-action provider should dispatch");
  Expect(!host.QueryDefinition(lang, readme, 1, 1, &err).empty(),
         "definition provider should dispatch");
  Expect(!host.QueryReferences(lang, readme, 1, 1, true, &err).empty(),
         "references provider should dispatch");
  PluginHost::SignatureHelpResult signature;
  Expect(host.QuerySignatureHelp(lang, readme, 1, 1, &signature, &err) &&
             !signature.signatures.empty(),
         "signature-help provider should dispatch");
  Expect(!host.QueryDocumentSymbols(lang, readme, &err).empty(),
         "document-symbols provider should dispatch");

  std::string save_text = "lower\n";
  Expect(host.RunSaveParticipants(readme, &save_text, &err) && save_text == "LOWER\n",
         "save participant should transform the document");

  std::vector<PluginHost::TestCase> discovered;
  Expect(host.DiscoverTests("qa-coverage.t", readme, &discovered, &err) && !discovered.empty(),
         "test provider should discover");
  std::vector<PluginHost::TestRunResult> run_results;
  Expect(host.RunTests("qa-coverage.t", {"t.case"}, &run_results, &err) && !run_results.empty(),
         "test provider should run");

  // ---- command dispatch + runtime publish/edit seams ---------------------
  std::string command_error;
  std::string feedback;
  Expect(host.ExecuteCommand("qa.cov.cmd", {"a", "b"}, &command_error, &feedback) &&
             feedback == "ran:a,b",
         "command dispatch should return the plugin feedback string");
  Expect(host.ExecuteCommand("qa.cov.edit", {}, &command_error),
         "buffer-edit / ghost-text command should dispatch");

  Expect(probe.diagnostics_published, "ctx.diagnostics.publish seam should fire");
  Expect(probe.decorations_published, "ctx.decorations.set seam should fire");
  Expect(probe.surface_published, "ctx.surface.set seam should fire");
  // The published surface's hit-region rect must be sanitized: NaN x folded to 0,
  // negative width clamped to >= 0.
  Expect(probe.last_surface.has_value() && !probe.last_surface->hit_regions.empty(),
         "the published surface should carry the hit region");
  if (probe.last_surface.has_value() && !probe.last_surface->hit_regions.empty()) {
    const SDL_FRect& rect = probe.last_surface->hit_regions.front().rect;
    Expect(std::isfinite(rect.x) && rect.x == 0.0f, "a NaN x is folded to 0");
    Expect(rect.w >= 0.0f, "a negative width is clamped to >= 0");
  }
  Expect(probe.edit_applied, "ctx.editor.apply_edits seam should fire");
  Expect(probe.ghost_published, "ctx.editor.set_ghost_text seam should fire");
  Expect(!probe.notifications.empty() && probe.notifications.front() == "info:coverage-notify",
         "ctx.notify seam should fire with level + message");
  Expect(!probe.logs.empty(), "ctx.log seam should fire");
}

// Inverse guard: with no plugins installed, every surface accessor is empty. Catches
// any accidental ambient/default registration leaking a surface the host did not load.
void TestPluginSurfaceCoverageEmptyWithoutPlugins() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "x\n");
  std::filesystem::create_directories(config_home / "microide" / "plugins");

  ScopedPluginConfigHomeEnv config_env(config_home);

  CallbackProbe probe;
  PluginHost host;
  host.SetCallbacks(MakeProbingCallbacks(&probe));
  Expect(host.Reload(project_root), "empty plugin dir should reload cleanly");

  Expect(host.LoadedPluginCount() == 0, "no plugins should be loaded");
  Expect(host.Errors().empty(), "an empty plugin set produces no errors");
  Expect(host.CommandNames().empty(), "no commands without a plugin");
  Expect(host.ContributedThemes().empty(), "no themes without a plugin");
  Expect(host.ContributedDebugAdapters().empty(), "no debug adapters without a plugin");
  Expect(host.ContributedCompletions().empty(), "no completions without a plugin");
  Expect(host.ContributedScmProviders().empty(), "no scm providers without a plugin");
}

// Regression (TD-2026-07-17A-044): a raw rgba8 raster's cache key must fold in the
// declared dimensions (the same byte string is a valid 4x4 or 2x8 image), so two raw
// rasters with identical bytes but different geometry get distinct content hashes and
// don't alias the same decoded texture. Encoded bytes fully determine the image, so
// their key stays bytes-only; a raw/encoded discriminator keeps the two apart.
void TestRasterContentHashKeyIncludesDimensionsForRaw() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  using microide::plugin::surface_interop::ComputeRasterContentHash;
  const std::string bytes(64, '\x7f');

  // Raw rgba8: identical bytes, different geometry -> distinct keys.
  const std::uint64_t raw_4x4 = ComputeRasterContentHash(bytes, /*is_rgba=*/true, 4, 4);
  const std::uint64_t raw_2x8 = ComputeRasterContentHash(bytes, /*is_rgba=*/true, 2, 8);
  const std::uint64_t raw_4x4_again = ComputeRasterContentHash(bytes, /*is_rgba=*/true, 4, 4);
  Expect(raw_4x4 != raw_2x8,
         "raw rgba8 rasters with identical bytes but different dimensions differ");
  Expect(raw_4x4 == raw_4x4_again, "the raw raster key is stable for identical bytes+dimensions");

  // Encoded: bytes fully determine the image, so dimensions do not fragment the key.
  const std::uint64_t enc_a = ComputeRasterContentHash(bytes, /*is_rgba=*/false, 4, 4);
  const std::uint64_t enc_b = ComputeRasterContentHash(bytes, /*is_rgba=*/false, 2, 8);
  Expect(enc_a == enc_b, "encoded rasters key on bytes only (dimensions come from the bytes)");

  // A raw raster never aliases an encoded one with the same bytes.
  Expect(raw_4x4 != enc_a, "raw and encoded rasters with identical bytes have distinct keys");

  // Never the "no raster" sentinel.
  Expect(raw_4x4 != 0 && enc_a != 0, "the content hash is never the 0 sentinel");
}

}  // namespace

void RegisterPluginSurfaceCoverageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginSurface/RegistersEverySurface",
          TestPluginSurfaceCoverageRegistersEverySurface);
  AddTest(tests, "PluginSurface/EmptyWithoutPlugins",
          TestPluginSurfaceCoverageEmptyWithoutPlugins);
  AddTest(tests, "PluginSurface/RasterContentHashKeyIncludesDimensionsForRaw",
          TestRasterContentHashKeyIncludesDimensionsForRaw);
}

}  // namespace microide::tests
