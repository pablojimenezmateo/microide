#include "workspace/registries/WorkspaceSettingsRegistry.h"

#include <algorithm>
#include <array>
#include <charconv>

#include "plugin/PluginHost.h"
#include "util/Parse.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

namespace {

const SettingEnumValue kColorschemeValues[] = {
    {"default", "Default"},
};

const SettingEnumValue kEditorWrapValues[] = {
    {"off", "Off"},
    {"word", "Word"},
};

const SettingEnumValue kEditorLineEndingsValues[] = {
    {"lf", "LF"},
    {"crlf", "CRLF"},
    {"auto", "Auto"},
};

const SettingEnumValue kEditorAutosaveValues[] = {
    {"off", "Off"},
    {"on_focus_change", "On focus change"},
    {"after_delay", "After delay"},
};

const SettingEnumValue kLayoutModeValues[] = {
    {"auto", "Auto"},
    {"regular", "Regular"},
    {"compact", "Compact"},
};

const SettingEnumValue kSeverityValues[] = {
    {"hint", "Hint"},
    {"info", "Info"},
    {"warning", "Warning"},
    {"error", "Error"},
};

}  // namespace

std::span<const SettingSpec> BuiltinSettingSpecs() {
  static const auto kSpecs = std::to_array<SettingSpec>({
      SettingSpec{
          .id = "editor.tab_size",
          .label = "Tab Size",
          .description = "Number of spaces per tab stop.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 4,
          .min_int = 1,
          .max_int = 16,
          .int_step = 1,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Indentation & Wrapping",
      },
      SettingSpec{
          .id = "editor.indent_width",
          .label = "Indent Width",
          .description = "Number of spaces used for each indent level.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 4,
          .min_int = 1,
          .max_int = 16,
          .int_step = 1,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Indentation & Wrapping",
      },
      SettingSpec{
          .id = "editor.soft_tabs",
          .label = "Soft Tabs",
          .description = "Insert spaces instead of tabs.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Indentation & Wrapping",
      },
      SettingSpec{
          .id = "editor.wrap",
          .label = "Word Wrap",
          .description = "Wrap long lines to the viewport width.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "off",
          .enum_values = kEditorWrapValues,
          .group = "Editor → Indentation & Wrapping",
      },
      SettingSpec{
          .id = "editor.colorscheme",
          .label = "Color Scheme",
          .description = "Active color scheme name.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "default",
          .enum_values = kColorschemeValues,
          .group = "Appearance",
      },
      SettingSpec{
          .id = "ui.scale",
          .label = "UI Scale",
          .description = "Interface zoom factor.",
          .type = SettingType::Float,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 1.0f,
          .min_float = kMinUiScale,
          .max_float = kMaxUiScale,
          .default_string = {},
          .enum_values = {},
          .group = "Appearance",
      },
      SettingSpec{
          .id = "editor.font_family",
          .label = "Font Family",
          .description = "Editor font family (empty uses the platform default).",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Font",
          .suggests_fonts = true,
      },
      SettingSpec{
          .id = "editor.font_size",
          .label = "Font Size",
          .description = "Editor font size in points (8..32). Applies to all buffers in this project.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 13,
          .min_int = 8,
          .max_int = 32,
          .int_step = 1,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Font",
      },
      SettingSpec{
          .id = "editor.line_endings",
          .label = "Line Endings",
          .description = "Line ending style used when saving files.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "auto",
          .enum_values = kEditorLineEndingsValues,
          .group = "Editor → Text & Files",
      },
      SettingSpec{
          .id = "editor.format_on_save",
          .label = "Format On Save",
          .description = "Run the configured external formatter for the file's language when saving.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Text & Files",
      },
      SettingSpec{
          .id = "editor.autosave",
          .label = "Autosave",
          .description = "When to save dirty buffers automatically.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "off",
          .enum_values = kEditorAutosaveValues,
          .group = "Editor → Text & Files",
      },
      SettingSpec{
          .id = "editor.autosave.delay_ms",
          .label = "Autosave Delay (ms)",
          .description = "Idle delay before \"After delay\" autosave writes dirty buffers.",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 1000,
          .min_int = 200,
          .max_int = 60000,
          .int_step = 250,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Text & Files",
      },
      SettingSpec{
          .id = "ui.layout_mode",
          .label = "Layout Mode",
          .description = "Compact / Regular / Auto layout selection.",
          .type = SettingType::Enum,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "auto",
          .enum_values = kLayoutModeValues,
          .group = "Appearance → Layout",
      },
      SettingSpec{
          .id = "ui.layout_compact_breakpoint_px",
          .label = "Compact Breakpoint (px)",
          .description = "Window width below which auto layout switches to Compact (600..2000).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = static_cast<int>(kWorkspaceLayoutCompactBreakpointDefault),
          .min_int = 600,
          .max_int = 2000,
          .int_step = 20,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Appearance → Layout",
      },
      SettingSpec{
          .id = "ui.show_status_bar",
          .label = "Show Status Bar",
          .description = "Display the bottom status bar.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Appearance → Layout",
      },
      SettingSpec{
          .id = "debug.enabled",
          .label = "Enable Debugger",
          .description = "Master switch for debugging. When off, the editor has no breakpoint "
                         "gutter, debug panels, debug commands, or hover-to-inspect. When on, "
                         "the debug affordances and the Debug Adapter Protocol integration "
                         "become available.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Debugger",
      },
      SettingSpec{
          .id = "plugins.inline_surfaces",
          .label = "Inline Plugin Surfaces",
          .description = "Let plugins render a content surface inset directly below the line it is "
                         "anchored to, instead of only in the sidebar or bottom panel. Off by "
                         "default; when off, no inset row gaps are laid out and the editor's row "
                         "geometry stays on its fast path.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Plugins",
      },
      SettingSpec{
          .id = "plugins.code_lens_above",
          .label = "Code Lenses Above The Line",
          .description = "Render plugin code lenses as a strip inset above their line, the way "
                         "VSCode does, instead of as an end-of-line annotation. Off by default; "
                         "when off, lenses stay inline and no inset row gaps are laid out.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Plugins",
      },
      SettingSpec{
          .id = "plugins.ghost_text",
          .label = "Inline Suggestions (Ghost Text)",
          .description = "Let plugins (e.g. a Copilot bridge) show dimmed inline AI suggestions at "
                         "the caret. Press Tab to accept the whole suggestion, Esc to dismiss; "
                         "typing or moving the caret clears it. Off by default; when off, no "
                         "suggestion state is stored and rendering stays zero-cost.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Plugins",
      },
      SettingSpec{
          .id = "control.enabled",
          .label = "Enable Control Channel",
          .description = "Allow an external tool to drive this instance over a private Unix-domain "
                         "socket (set breakpoints, step, open files/projects/terminals, observe "
                         "stops). Off by default. When on, the socket is created 0600 under "
                         "$XDG_RUNTIME_DIR; run `microide control-help` for the protocol.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Control",
      },
      SettingSpec{
          .id = "terminal.shell",
          .label = "Terminal Shell",
          .description = "Shell command used by new terminals (empty for platform default).",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Terminal",
      },
      SettingSpec{
          .id = "terminal.font_size",
          .label = "Terminal Font Size",
          .description = "Terminal font size in points (8..32).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 13,
          .min_int = 8,
          .max_int = 32,
          .int_step = 1,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Terminal",
      },
      SettingSpec{
          .id = "terminal.font_family",
          .label = "Terminal Font",
          .description = "Terminal font family. Empty uses the platform default monospace font.",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Terminal",
          .suggests_fonts = true,
      },
      SettingSpec{
          .id = "terminal.scrollback_lines",
          .label = "Terminal Scrollback",
          .description = "Maximum number of lines retained in terminal scrollback (200..100000).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_int = 2000,
          .min_int = 200,
          .max_int = 100000,
          .int_step = 1000,
          .default_string = {},
          .enum_values = {},
          .group = "Terminal",
      },
      SettingSpec{
          .id = "terminal.osc52_clipboard_write",
          .label = "Allow Terminal Clipboard Writes (OSC 52)",
          .description = "Let a program running in the terminal set the system clipboard via the "
                         "OSC 52 escape sequence. Off by default: otherwise any program printing "
                         "to the terminal could silently overwrite your clipboard (e.g. swapping a "
                         "copied command for a malicious one). Enable only if you rely on remote "
                         "yank-to-clipboard (tmux/vim over SSH).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Terminal",
      },
      SettingSpec{
          .id = "project.follow_out_of_root_symlinks",
          .label = "Follow Out-of-Root Symlinks",
          .description = "Follow directory symlinks whose target is outside the project root when "
                         "building the file tree and search index. Off by default: an opened project "
                         "is untrusted, and a symlink pointing at the whole filesystem (e.g. "
                         "'link -> /') would otherwise make the walk index the host filesystem — a "
                         "resource-exhaustion and information-disclosure risk. Enable for monorepos "
                         "or projects that reach shared code through external symlinks.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Workspace",
      },
      SettingSpec{
          .id = "project.files_exclude",
          .label = "Excluded Paths",
          .description = "Extra ignore patterns (gitignore syntax, one per line or comma-separated) "
                         "for the file tree, finder, search index, and file watcher — on top of the "
                         "built-in defaults (VCS metadata, node_modules/.cache, and build-output dirs "
                         "like build/out/dist/target). Excluded directories render grayed in the tree "
                         "(still expandable) but are not indexed or watched. Use a leading '!' to "
                         "re-include a directory the defaults would otherwise skip (e.g. '!build/').",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = "Workspace",
      },
      SettingSpec{
          .id = "editor.caret_blink.enabled",
          .label = "Caret Blink",
          .description = "Blink the text caret. When off, the caret stays solid (no idle wake-ups).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "editor.caret_blink.interval_ms",
          .label = "Caret Blink Interval (ms)",
          .description = "Milliseconds between caret blink phase changes (100..2000).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_int = 530,
          .min_int = 100,
          .max_int = 2000,
          .int_step = 50,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "chrome.project_tabs.hide_when_single",
          .label = "Hide Project Tabs When Single",
          .description = "Hide the project tab strip while one project (or none) is open. "
                         "On by default; the strip reappears when a second project opens.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Workspace",
      },
      SettingSpec{
          .id = "session.restore_on_launch",
          .label = "Restore Session on Launch",
          .description = "Reopen the previous projects, tabs, and layout when the app starts.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Workspace",
      },
      SettingSpec{
          .id = "diagnostics.min_severity",
          .label = "Minimum Diagnostic Severity",
          .description = "Suppress diagnostics below this severity.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "hint",
          .enum_values = kSeverityValues,
          .group = "Diagnostics",
      },
      // Editor essentials toggles. Defaults match the design table; project
      // overrides win over user preferences.
      SettingSpec{
          .id = "editor.fold.enabled",
          .label = "Code Folding",
          .description = "Show fold gutter and allow collapse/expand of bracket and indent blocks.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Block Structure",
      },
      SettingSpec{
          .id = "editor.fold.sticky_scroll.enabled",
          .label = "Sticky Scroll",
          .description = "Show the enclosing block headers as a fixed band at the top of the editor.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Block Structure",
      },
      SettingSpec{
          .id = "editor.fold.sticky_scroll.max_depth",
          .label = "Sticky Scroll Max Depth",
          .description = "Maximum number of sticky lines to render (1..8).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_int = 3,
          .min_int = 1,
          .max_int = 8,
          .int_step = 1,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Block Structure",
      },
      SettingSpec{
          .id = "editor.view.indent_guides.enabled",
          .label = "Indent Guides",
          .description = "Show vertical guide lines at indent boundaries.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Block Structure",
      },
      SettingSpec{
          .id = "editor.inlay_hints.enabled",
          .label = "Inlay Hints",
          .description = "Show inline type and parameter-name hints from the language server "
                         "(mid-line virtual text). When off, no inlay hints are requested or "
                         "rendered.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "editor.blame.inline.enabled",
          .label = "Inline Git Blame",
          .description = "Show the git blame overlay (author/commit per line) to the right of the "
                         "active editor. When off, no blame is requested or rendered.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "editor.line_numbers",
          .label = "Line Numbers",
          .description = "Show the line-number gutter in the editor.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "editor.view.render_whitespace",
          .label = "Render Whitespace",
          .description = "Render whitespace characters as low-contrast glyphs.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Block Structure",
      },
      SettingSpec{
          .id = "editor.view.overview_ruler.enabled",
          .label = "Scrollbar Overview Ruler",
          .description = "Show a marker bar beside the editor scrollbar with search matches, "
                         "diagnostics, and the caret position, so you can see where they are "
                         "across the whole file at a glance.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → View",
      },
      SettingSpec{
          .id = "sidebar.file_icons",
          .label = "File Type Icons",
          .description =
              "Show built-in type icons beside files in the explorer tree. Off by "
              "default; plugin-contributed icon themes enable icons regardless.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_string = {},
          .enum_values = {},
          .group = "Sidebar",
      },
      SettingSpec{
          .id = "editor.brackets.match_highlight.enabled",
          .label = "Bracket Match Highlight",
          .description = "Highlight the matching bracket adjacent to the caret.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.brackets.auto_close.enabled",
          .label = "Auto-Close Pairs",
          .description = "Insert the matching close character when an open is typed.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.brackets.surround.enabled",
          .label = "Surround Selection",
          .description = "Wrap a non-empty selection when an open character is typed.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.indent.smart.enabled",
          .label = "Smart Indent",
          .description = "Add one indent unit after lines that end with an opener; drop one indent on close.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.editorconfig.enabled",
          .label = "Honor .editorconfig",
          .description = "Let a project's .editorconfig override indent, line-ending, and save-normalization settings.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.indent.detect_on_open",
          .label = "Auto-Detect Indent",
          .description = "Inspect the first lines of an opened file to choose tabs vs. spaces and indent width.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.shaping.toggle_comment.enabled",
          .label = "Toggle Comment Action",
          .description = "Enable the toggle-line-comment / toggle-block-comment commands.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.shaping.line_ops.enabled",
          .label = "Line Move / Duplicate / Delete",
          .description = "Enable the move-line / duplicate-line / delete-line commands.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.shaping.sort_lines.enabled",
          .label = "Sort Lines Action",
          .description = "Enable the sort-lines-ascending / sort-lines-descending commands.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.multicursor.add_at_match.enabled",
          .label = "Add Cursor at Match",
          .description = "Enable Ctrl+D / Ctrl+Shift+L to add cursors at matching words.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.search.case_sensitive",
          .label = "Case-Sensitive Search Seed Matching",
          .description =
              "When enabled, occurrence highlight plus Ctrl+D / Ctrl+Shift+L add-cursor scans treat "
              "the seeded text as case-sensitive (default off).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.occurrences.enabled",
          .label = "Occurrences Highlight",
          .description = "Highlight matches of the word under the caret in the visible viewport.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.snippets.enabled",
          .label = "Snippets",
          .description = "Enable snippet expansion and the Insert Snippet overlay.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.save.trim_trailing_whitespace",
          .label = "Trim Trailing Whitespace on Save",
          .description = "Remove trailing spaces and tabs on every line when saving.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      SettingSpec{
          .id = "editor.save.ensure_final_newline",
          .label = "Ensure Final Newline on Save",
          .description = "Make the saved file end with exactly one newline.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = true,
          .default_string = {},
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      // Language-contract user overrides. The strings are comma-separated
      // tokens consumed by `WorkspaceLanguageContract::Refresh`. Empty values
      // mean "no override". Project scope wins over user scope.
      SettingSpec{
          .id = "editor.brackets.user_pairs",
          .label = "Extra Bracket Pairs",
          .description =
              "Comma-separated open|close pairs added to every language (e.g. \"<|>,⟦|⟧\").",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_string = "",
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.brackets.user_disabled",
          .label = "Disabled Bracket Pairs",
          .description =
              "Comma-separated open|close pairs to remove from the resolved contract.",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_string = "",
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.comments.user_line",
          .label = "Line Comment Override",
          .description =
              "If non-empty, replaces the line-comment marker for every language.",
          .type = SettingType::String,
          .scope = SettingScope::Project,
          .default_string = "",
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.indents.user_open_patterns",
          .label = "Extra Smart-Indent Patterns",
          .description =
              "Comma-separated suffix tokens added to every language's indent-after-open list.",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_string = "",
          .enum_values = {},
          .group = "Editor → Essentials → Pair And Indent",
      },
      SettingSpec{
          .id = "editor.snippets.user_disabled",
          .label = "Disabled Snippets",
          .description =
              "Comma-separated snippet IDs to suppress from the resolved contract.",
          .type = SettingType::String,
          .scope = SettingScope::User,
          .default_string = "",
          .enum_values = {},
          .group = "Editor → Essentials → Shaping And Save",
      },
      // Language Server Protocol. `lsp.enabled` is the master switch (mirrors
      // `debug.enabled`): when off, no language server subprocess is started, all
      // features go dark, and LSP menu entries hide. Each per-feature toggle below
      // gates one capability while the master stays on. All default on.
      SettingSpec{
          .id = "lsp.enabled",
          .label = "Enable Language Server Protocol",
          .description =
              "Master switch for LSP. When off, no language server is started (or a running "
              "one is stopped), and completion, hover, diagnostics, code actions, formatting, "
              "rename, go-to-definition, find-references, signature help, semantic highlighting, "
              "and the document outline are all unavailable. When on, the per-feature toggles "
              "below decide which capabilities are active.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP",
      },
      SettingSpec{
          .id = "lsp.completion.enabled",
          .label = "Completion",
          .description = "Offer language-server code completion (Ctrl+Space).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.hover.enabled",
          .label = "Hover",
          .description = "Show language-server hover documentation when the pointer rests on a symbol.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.diagnostics.enabled",
          .label = "Diagnostics",
          .description = "Display language-server diagnostics (errors/warnings) inline and in the "
                         "Problems panel.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.code_actions.enabled",
          .label = "Code Actions / Quick Fix",
          .description = "Offer language-server code actions and quick fixes (Ctrl+.).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.formatting.enabled",
          .label = "Formatting",
          .description = "Allow the language server to format the active document (Ctrl+Shift+I).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.rename.enabled",
          .label = "Rename Symbol",
          .description = "Allow language-server symbol rename across the workspace (F2).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.goto_definition.enabled",
          .label = "Go to Definition",
          .description = "Enable language-server go-to-definition (F12).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.find_references.enabled",
          .label = "Find References",
          .description = "Enable language-server find-all-references (Shift+F12).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.call_hierarchy.enabled",
          .label = "Call Hierarchy",
          .description = "Enable language-server incoming/outgoing call hierarchy.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.navigation.enabled",
          .label = "Extended Navigation",
          .description = "Enable language-server go-to type definition, implementation, and "
                         "declaration.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.workspace_symbol.enabled",
          .label = "Workspace Symbol Search",
          .description = "Enable language-server project-wide symbol search (workspace/symbol).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.signature_help.enabled",
          .label = "Signature Help",
          .description = "Show language-server parameter/signature hints while typing a call "
                         "(Ctrl+Shift+Space).",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.semantic_tokens.enabled",
          .label = "Semantic Highlighting",
          .description = "Recolor identifiers using language-server semantic tokens. When off, the "
                         "built-in lexical highlighter is used alone.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.code_lens.enabled",
          .label = "Code Lens",
          .description = "Show the language server's line-level actions (reference counts, run "
                         "test) above or at the end of their line. Click one to run it.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.document_highlight.enabled",
          .label = "Semantic Occurrence Highlighting",
          .description = "Highlight other uses of the symbol under the caret using the language "
                         "server, which resolves the symbol instead of matching its spelling. "
                         "When off, the built-in word scan is used alone.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
      SettingSpec{
          .id = "lsp.document_symbols.enabled",
          .label = "Document Outline",
          .description = "Populate the Outline sidebar view from language-server document symbols.",
          .type = SettingType::Bool,
          .scope = SettingScope::User,
          .default_bool = true,
          .group = "LSP → Features",
      },
  });
  return kSpecs;
}

const SettingSpec* FindBuiltinSettingSpec(std::string_view id) {
  const auto specs = BuiltinSettingSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const SettingSpec& s) { return s.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

SettingValue DefaultSettingValue(const SettingSpec& spec) {
  switch (spec.type) {
    case SettingType::Bool:
      return spec.default_bool;
    case SettingType::Int:
      return spec.default_int;
    case SettingType::Float:
      return spec.default_float;
    case SettingType::String:
    case SettingType::Enum:
      return std::string(spec.default_string);
  }
  return std::string{};
}

std::optional<SettingValue> ParseSettingValue(const SettingSpec& spec, std::string_view text) {
  switch (spec.type) {
    case SettingType::Bool:
      if (text == "true" || text == "1" || text == "on" || text == "yes") {
        return true;
      }
      if (text == "false" || text == "0" || text == "off" || text == "no") {
        return false;
      }
      return std::nullopt;

    case SettingType::Int: {
      int value = 0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
      }
      // Clamp to the spec's inclusive range at store time so the stored/displayed
      // value never diverges from the clamped value the editor actually applies
      // (e.g. `set-setting editor.font_size 999` stores 32, not 999). Specs with
      // no explicit range keep the INT_MIN..INT_MAX sentinels, so this is a no-op.
      return std::clamp(value, spec.min_int, spec.max_int);
    }

    case SettingType::Float: {
      const std::optional<float> value = util::ParseFloat(text);
      if (!value.has_value()) {
        return std::nullopt;
      }
      // Clamp to the spec's inclusive range at store time (mirrors the Int case)
      // so e.g. `set-setting ui.scale 999` stores the applied max, not 999. Specs
      // with no explicit range keep the lowest..max sentinels, so this is a no-op.
      return std::clamp(*value, spec.min_float, spec.max_float);
    }

    case SettingType::String:
      return std::string(text);

    case SettingType::Enum:
      for (const SettingEnumValue& ev : spec.enum_values) {
        if (ev.value == text) {
          return std::string(text);
        }
      }
      return std::nullopt;
  }
  return std::nullopt;
}

std::string SerializeSettingValue(const SettingValue& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
          return std::to_string(v);
        } else {
          return v;
        }
      },
      value);
}

bool ApplyCanonicalEditorPreference(EditorPreferences& prefs, std::string_view id,
                                    const SettingValue& value) {
  if (id == "editor.tab_size") {
    if (const int* parsed = std::get_if<int>(&value); parsed != nullptr) {
      prefs.tab_size = static_cast<std::size_t>(std::clamp(*parsed, 1, 16));
    }
    return true;
  }
  if (id == "editor.indent_width") {
    if (const int* parsed = std::get_if<int>(&value); parsed != nullptr) {
      prefs.indent_width = static_cast<std::size_t>(std::clamp(*parsed, 1, 16));
    }
    return true;
  }
  if (id == "editor.font_size") {
    if (const int* parsed = std::get_if<int>(&value); parsed != nullptr) {
      prefs.font_size = std::clamp(*parsed, 8, 32);
    }
    return true;
  }
  if (id == "editor.soft_tabs") {
    if (const bool* parsed = std::get_if<bool>(&value); parsed != nullptr) {
      prefs.soft_tabs = *parsed;
    }
    return true;
  }
  if (id == "editor.wrap") {
    if (const std::string* parsed = std::get_if<std::string>(&value); parsed != nullptr) {
      prefs.soft_wrap = (*parsed == "word");
    }
    return true;
  }
  return false;
}

std::vector<SettingInfo> AllSettingInfos(const plugin::PluginHost& plugin_host) {
  std::vector<SettingInfo> infos;

  for (const SettingSpec& spec : BuiltinSettingSpecs()) {
    SettingInfo info;
    info.id = std::string(spec.id);
    info.label = std::string(spec.label);
    info.description = std::string(spec.description);
    info.type = spec.type;
    info.scope = spec.scope;
    info.default_value = DefaultSettingValue(spec);
    info.group = std::string(spec.group);
    info.suggests_fonts = spec.suggests_fonts;
    for (const SettingEnumValue& ev : spec.enum_values) {
      info.enum_values.emplace_back(ev.value);
    }
    infos.push_back(std::move(info));
  }

  for (const auto& contrib : plugin_host.ContributedSettings()) {
    SettingInfo info;
    info.id = contrib.id;
    info.label = contrib.label;
    info.description = contrib.description;
    info.plugin_id = contrib.plugin_id;

    if (contrib.type == "bool") {
      info.type = SettingType::Bool;
      info.default_value = contrib.default_value == "true";
    } else if (contrib.type == "int") {
      info.type = SettingType::Int;
      int v = 0;
      std::from_chars(contrib.default_value.data(),
                      contrib.default_value.data() + contrib.default_value.size(), v);
      info.default_value = v;
    } else if (contrib.type == "float") {
      info.type = SettingType::Float;
      info.default_value = util::ParseFloat(contrib.default_value).value_or(0.0f);
    } else if (contrib.type == "enum") {
      info.type = SettingType::Enum;
      info.default_value = contrib.default_value;
      info.enum_values = contrib.enum_values;
    } else {
      info.type = SettingType::String;
      info.default_value = contrib.default_value;
    }

    if (contrib.scope == "user") {
      info.scope = SettingScope::User;
    } else {
      info.scope = SettingScope::Project;
    }

    infos.push_back(std::move(info));
  }

  return infos;
}

std::optional<SettingInfo> FindSettingInfo(std::string_view id,
                                            const plugin::PluginHost& plugin_host) {
  const auto infos = AllSettingInfos(plugin_host);
  const auto it = std::find_if(infos.begin(), infos.end(),
                               [id](const SettingInfo& info) { return info.id == id; });
  if (it == infos.end()) {
    return std::nullopt;
  }
  return *it;
}

}  // namespace microide::workspace
