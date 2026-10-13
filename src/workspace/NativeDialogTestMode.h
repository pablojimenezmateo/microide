#pragma once

namespace microide::workspace {

// Process-wide: when set, the native OS dialogs (Open Folder…, Open File…, the
// font file picker) report themselves unavailable instead of opening, unless the
// shell has a test launcher installed for that dialog. The test runner and the
// perf harness set it once at startup, like SetUsePlaceholderTerminalsForTesting:
// a test that reached a real dialog popped it on the developer's desktop and,
// under LeakSanitizer, failed its shard on SDL's portal allocations.
void SetNativeDialogsDisabledForTesting(bool disabled);
bool NativeDialogsDisabledForTesting();

}  // namespace microide::workspace
