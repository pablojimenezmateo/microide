#pragma once

#include <string>

namespace microide::workspace {

// Render the complete groff(7) man page for `microide` (the committed
// docs/microide.1). The CONTROL CHANNEL and COMMANDS sections are generated from
// ControlChannelHelpText() and WorkspaceDocumentedCommandUsages() so the shipped
// man page can never drift from the implementation; the rest is static.
//
// Deterministic (no clock / environment input) so a test can assert the
// committed file equals this output. Backs `microide control-man` and
// tools/gen-man.sh.
std::string RenderManPage();

}  // namespace microide::workspace
