#include "workspace/WorkspaceSettingsRegistry.h"

#include <algorithm>
#include <array>
#include <charconv>

#include "plugin/PluginHost.h"
#include "util/Parse.h"
#include "workspace/WorkspaceLayout.h"

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

const SettingEnumValue kHandleSizeValues[] = {
    {"compact", "Compact"},
    {"regular", "Regular"},
    {"large", "Large"},
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
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
      },
      SettingSpec{
          .id = "editor.indent_width",
          .label = "Indent Width",
          .description = "Number of spaces used for each indent level.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 4,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
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
          .group = {},
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
          .group = {},
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
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
      },
      SettingSpec{
          .id = "editor.font_size",
          .label = "Font Size",
          .description = "Editor font size in points (8..32).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 13,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
      },
      SettingSpec{
          .id = "editor.trim_trailing_whitespace",
          .label = "Trim Trailing Whitespace",
          .description = "Remove trailing whitespace on save.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
      },
      SettingSpec{
          .id = "editor.insert_final_newline",
          .label = "Insert Final Newline",
          .description = "Ensure files end with a newline on save.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
      },
      SettingSpec{
          .id = "editor.format_on_save",
          .label = "Format On Save",
          .description = "Run the configured formatter when saving.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
      },
      SettingSpec{
          .id = "editor.hover_delay_ms",
          .label = "Hover Delay (ms)",
          .description = "Milliseconds before hover popups open.",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 350,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
      },
      SettingSpec{
          .id = "ui.layout_compact_breakpoint_px",
          .label = "Compact Breakpoint (px)",
          .description = "Window width below which auto layout switches to Compact (600..2000).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = static_cast<int>(kWorkspaceLayoutCompactBreakpointDefault),
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
      },
      SettingSpec{
          .id = "ui.scrollbar_size",
          .label = "Scrollbar Size",
          .description = "Compact / Regular / Large scrollbar visual size.",
          .type = SettingType::Enum,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "regular",
          .enum_values = kHandleSizeValues,
          .group = {},
      },
      SettingSpec{
          .id = "ui.resize_handle_size",
          .label = "Resize Handle Size",
          .description = "Compact / Regular / Large resize-handle visual size.",
          .type = SettingType::Enum,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "regular",
          .enum_values = kHandleSizeValues,
          .group = {},
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
          .group = {},
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
          .group = {},
      },
      SettingSpec{
          .id = "terminal.font_size",
          .label = "Terminal Font Size",
          .description = "Terminal font size in points (8..32).",
          .type = SettingType::Int,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 13,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
          .group = {},
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
          .group = {},
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
          .default_bool = false,
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
          .default_bool = false,
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
      return value;
    }

    case SettingType::Float: {
      return util::ParseFloat(text);
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
