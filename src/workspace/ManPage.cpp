#include "workspace/ManPage.h"

#include <string>
#include <vector>

#include "workspace/control/ControlProtocol.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"

namespace microide::workspace {

namespace {

// Fixed so the rendered page is byte-stable (the drift test compares it to the
// committed file). Bump by hand when the man page meaningfully changes.
constexpr const char* kManPageDate = "2026-06-20";

// Escape a block destined for a groff `.nf`/`.fi` (no-fill) region: escape the
// control character `\`, and guard any line beginning with `.` or `'` (which
// groff would otherwise read as a request) with `\&`.
std::string EscapeNoFillBlock(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 16);
  bool at_line_start = true;
  for (const char c : text) {
    if (at_line_start && (c == '.' || c == '\'')) {
      out += "\\&";
    }
    if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
    at_line_start = (c == '\n');
  }
  return out;
}

}  // namespace

std::string RenderManPage() {
  std::string page;
  page += ".TH MICROIDE 1 \"";
  page += kManPageDate;
  page += "\" \"microide\" \"User Commands\"\n";
  page +=
      ".SH NAME\n"
      "microide \\- compact native desktop IDE shell\n"
      ".SH SYNOPSIS\n"
      ".B microide\n"
      "[\\fB--disable-plugins\\fR] [\\fB--safe-mode\\fR] [\\fB--control\\fR] "
      "[\\fB--set\\fR \\fIid value\\fR]... [\\fB--control-spec\\fR \\fIfile\\fR] "
      "[\\fB--version\\fR] [\\fIproject-path\\fR]\n"
      ".br\n"
      ".B microide control-send\n"
      "[\\fB--pid\\fR \\fIn\\fR | \\fB--socket\\fR \\fIpath\\fR] "
      "(\\fIcommand\\fR... | \\fB--query\\fR \\fIverb\\fR | \\fB--json\\fR \\fIobject\\fR) "
      "[\\fB--wait\\fR \\fIevent\\fR] [\\fB--timeout\\fR \\fIsecs\\fR]\n"
      ".br\n"
      ".B microide control-help\n"
      ".br\n"
      ".B microide control-commands\n"
      ".br\n"
      ".B microide control-list\n"
      ".br\n"
      ".B microide dump-state\n"
      ".I persisted-file\n"
      ".SH DESCRIPTION\n"
      ".B microide\n"
      "is a single-window desktop editor and IDE shell (editor, compare, merge,\n"
      "search, git, terminal, plugin, and debugger workflows). With no\n"
      ".I project-path\n"
      "it opens the current working directory.\n"
      ".SH OPTIONS\n"
      ".TP\n"
      ".B --disable-plugins\n"
      "Start without loading the Lua plugin runtime.\n"
      ".TP\n"
      ".B --safe-mode\n"
      "Start without plugins and without restoring the previous session.\n"
      ".TP\n"
      ".B --control\n"
      "Force-start the control channel and mirror every response/event to stdout as\n"
      "JSONL (the headless entry point), independent of the\n"
      ".B control.enabled\n"
      "setting.\n"
      ".TP\n"
      ".B --set \\fIid value\\fR\n"
      "Apply a transient (never-persisted) setting override. May be repeated.\n"
      ".TP\n"
      ".B --control-spec \\fIfile\\fR\n"
      "Apply a cold-start control spec at launch: open a project with breakpoints\n"
      "already set (and optionally reveal files / start a debug session) before the\n"
      "window becomes interactive. See\n"
      ".B microide control-help\n"
      "for the JSON schema. Spec line numbers are 1-based; relative paths resolve\n"
      "against the project root.\n"
      ".TP\n"
      ".B --version, -V\n"
      "Print the microide version and exit.\n"
      ".SH CONTROL CHANNEL\n"
      "An external tool (such as an LLM) can drive a running instance over a private\n"
      "Unix-domain socket. The one-shot client\n"
      ".B microide control-send\n"
      "is the easiest way in. The full protocol, query verbs, commands, control-send\n"
      "usage, and end-to-end recipes follow verbatim from\n"
      ".BR \"microide control-help\" :\n"
      ".PP\n"
      ".nf\n";
  page += EscapeNoFillBlock(ControlChannelHelpText());
  page +=
      ".fi\n"
      ".SH COMMANDS\n"
      "Every runnable command (also printed by\n"
      ".BR \"microide control-commands\" ):\n"
      ".PP\n"
      ".nf\n";
  for (const std::string& usage : WorkspaceDocumentedCommandUsages()) {
    page += EscapeNoFillBlock("  " + usage);
    page += "\n";
  }
  page +=
      ".fi\n"
      ".SH SUBCOMMANDS\n"
      ".TP\n"
      ".B control-send \\fR[...]\\fP\n"
      "Send one command or query to a running instance and print the JSONL reply\n"
      "(and, with \\fB--wait\\fR, events) to stdout. Auto-targets the sole running\n"
      "instance; use \\fB--pid\\fR or \\fB--socket\\fR to disambiguate.\n"
      ".TP\n"
      ".B control-help\n"
      "Print the full control-channel protocol, query verbs, spec schema, and the list\n"
      "of runnable commands.\n"
      ".TP\n"
      ".B control-commands\n"
      "Print every runnable command name and its usage.\n"
      ".TP\n"
      ".B control-list\n"
      "Print the descriptor of each running instance that has the control channel\n"
      "enabled (pid, socket path, project).\n"
      ".TP\n"
      ".B control-man\n"
      "Print this man page (used by tools/gen-man.sh to regenerate docs/microide.1).\n"
      ".TP\n"
      ".B dump-state \\fIpersisted-file\\fR\n"
      "Decode and print a microide persisted-state file.\n"
      ".SH FILES\n"
      ".TP\n"
      ".I $XDG_RUNTIME_DIR/microide/<pid>.sock\n"
      "Per-instance control socket (mode 0600).\n"
      ".TP\n"
      ".I $XDG_RUNTIME_DIR/microide/instances/<pid>.json\n"
      "Per-instance discovery descriptor.\n"
      ".SH SECURITY\n"
      "The control channel grants a connecting client the same power as the command\n"
      "palette. It is off by default, the socket is user-private (0600) under the\n"
      "per-user runtime directory, and it must be explicitly enabled per instance.\n"
      ".SH SEE ALSO\n"
      "Project documentation under \\fIdev-docs/\\fR, in particular\n"
      "\\fIdev-docs/control/control-channel.md\\fR and\n"
      "\\fIdev-docs/debugger/dap-integration.md\\fR.\n";
  return page;
}

}  // namespace microide::workspace
